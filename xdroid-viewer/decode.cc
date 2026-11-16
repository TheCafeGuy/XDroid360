#include "decode.h"

#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "common.h"

namespace xv {
namespace {

namespace mp = xdroid::mirror;

// Inputs the MFT may hold before a picture comes out; older ones are lost.
constexpr size_t kMaxPending = 16;

}  // namespace

ComPtr<ID3D11Device> CreateDevice() {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
  ComPtr<ID3D11Device> device;
  if (FAILED(D3D11CreateDevice(
          nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
          D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
          levels, 2, D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
    std::fprintf(stderr, "xv: no D3D11 hardware device\n");
    return nullptr;
  }
  ComPtr<ID3D11Multithread> multithread;
  if (SUCCEEDED(device.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }
  return device;
}

std::vector<std::vector<std::span<const uint8_t>>> SplitAccessUnits(
    std::span<const uint8_t> stream) {
  std::vector<size_t> starts;  // First byte of each start code.
  for (size_t i = 2; i < stream.size(); ++i) {
    if (stream[i] == 1 && stream[i - 1] == 0 && stream[i - 2] == 0) {
      starts.push_back(i >= 3 && stream[i - 3] == 0 ? i - 3 : i - 2);
    }
  }
  std::vector<std::vector<std::span<const uint8_t>>> units;
  for (size_t k = 0; k < starts.size(); ++k) {
    size_t end = k + 1 < starts.size() ? starts[k + 1] : stream.size();
    std::span<const uint8_t> nal = stream.subspan(starts[k], end - starts[k]);
    if (nal.size() <= (nal[2] == 1 ? 3u : 4u)) {
      continue;
    }
    // NAL type 9: access unit delimiter.
    if (NalType(nal) == 9 || units.empty()) {
      units.emplace_back();
    }
    units.back().push_back(nal);
  }
  return units;
}

std::vector<uint8_t> ReadTestStream(const wchar_t* name) {
  wchar_t path[MAX_PATH];
  DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring file(path, length);
  file.resize(file.find_last_of(L"\\/") + 1);
  file += L"testdata\\";
  file += name;
  std::vector<uint8_t> data;
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
  LARGE_INTEGER size;
  if (handle == INVALID_HANDLE_VALUE) {
    return data;
  }
  DWORD read = 0;
  if (GetFileSizeEx(handle, &size)) {
    data.resize(size_t(size.QuadPart));
    ReadFile(handle, data.data(), DWORD(data.size()), &read, nullptr);
  }
  CloseHandle(handle);
  data.resize(read);
  return data;
}

std::array<double, 3> LatencyMs(std::vector<int64_t> times_us) {
  if (times_us.empty()) {
    return {};
  }
  std::sort(times_us.begin(), times_us.end());
  auto at = [&](double q) {
    return times_us[size_t(q * (times_us.size() - 1) + 0.5)] / 1000.0;
  };
  return {at(0.5), at(0.95), times_us.back() / 1000.0};
}

Decoder::Decoder()
    : fence_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      frame_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
  MFStartup(MF_VERSION, MFSTARTUP_LITE);
}

Decoder::~Decoder() {
  mft_.Reset();
  output_.Reset();
  manager_.Reset();
  CloseHandle(fence_event_);
  CloseHandle(frame_event_);
  MFShutdown();
}

bool Decoder::Start(ID3D11Device* device, mp::Codec codec, uint32_t width,
                    uint32_t height) {
  mft_.Reset();
  pending_.clear();
  config_.clear();
  dxva_ = false;
  // Windows has no inbox HEVC decoder (it is a Store extension).
  if (codec != mp::Codec::kAvc) {
    return false;
  }
  if (device_.Get() != device) {
    device_ = device;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    ComPtr<ID3D11Device5> device5;
    if (FAILED(context.As(&context_)) || FAILED(device_.As(&lock_))) {
      return false;
    }
    fence_.Reset();
    if (SUCCEEDED(device_.As(&device5))) {
      device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    }
    manager_.Reset();
    if (FAILED(MFCreateDXGIDeviceManager(&manager_token_, &manager_)) ||
        FAILED(manager_->ResetDevice(device, manager_token_))) {
      manager_.Reset();
    }
  }
  return Open(true, width, height) || Open(false, width, height);
}

bool Decoder::Open(bool dxva, uint32_t width, uint32_t height) {
  ComPtr<IMFTransform> mft;
  ComPtr<IMFAttributes> attributes;
  if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft))) ||
      FAILED(mft->GetAttributes(&attributes))) {
    return false;
  }
  // Same GUID as CODECAPI_AVLowLatencyMode: a picture leaves as soon as its
  // access unit is in, instead of after a reorder delay.
  attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
  if (dxva &&
      (!manager_ ||
       !MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE) ||
       FAILED(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                  ULONG_PTR(manager_.Get()))))) {
    return false;
  }
  ComPtr<IMFMediaType> input;
  MFCreateMediaType(&input);
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (width && height) {
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height);
  }
  mft_ = mft;
  if (FAILED(mft->SetInputType(0, input.Get(), 0)) || !SetOutputType() ||
      FAILED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
      FAILED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
    mft_.Reset();
    return false;
  }
  std::fprintf(stderr, "xv: H.264 decoder open (%s)\n",
               dxva ? "DXVA" : "software");
  return true;
}

bool Decoder::SetOutputType() {
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> type;
    GUID subtype;
    if (FAILED(mft_->GetOutputAvailableType(0, i, &type))) {
      return false;
    }
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
        subtype != MFVideoFormat_NV12) {
      continue;
    }
    if (FAILED(mft_->SetOutputType(0, type.Get(), 0))) {
      return false;
    }
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &coded_width_,
                       &coded_height_);
    stride_ =
        MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, coded_width_);
    // H.264 codes 1080 lines as 1088: the aperture is the part to show.
    MFVideoArea area;
    width_ = coded_width_;
    height_ = coded_height_;
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                reinterpret_cast<UINT8*>(&area), sizeof area,
                                nullptr))) {
      width_ = area.Area.cx;
      height_ = area.Area.cy;
    }
    MFT_OUTPUT_STREAM_INFO info{};
    mft_->GetOutputStreamInfo(0, &info);
    provides_samples_ = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
    output_.Reset();
    if (!provides_samples_) {
      ComPtr<IMFMediaBuffer> buffer;
      MFCreateSample(&output_);
      MFCreateMemoryBuffer(info.cbSize, &buffer);
      output_->AddBuffer(buffer.Get());
    }
    return true;
  }
}

void Decoder::SetConfig(std::span<const uint8_t> config) {
  config_.assign(config.begin(), config.end());
}

void Decoder::Decode(std::span<const uint8_t> access_unit, bool key,
                     const FrameTimes& times) {
  if (!mft_) {
    return;
  }
  ComPtr<IMFMediaBuffer> buffer;
  ComPtr<IMFSample> sample;
  BYTE* data;
  DWORD size = DWORD(config_.size() + access_unit.size());
  if (FAILED(MFCreateMemoryBuffer(size, &buffer)) ||
      FAILED(buffer->Lock(&data, nullptr, nullptr))) {
    return;
  }
  std::copy(access_unit.begin(), access_unit.end(),
            std::copy(config_.begin(), config_.end(), data));
  buffer->Unlock();
  buffer->SetCurrentLength(size);
  config_.clear();
  MFCreateSample(&sample);
  sample->AddBuffer(buffer.Get());
  // The MFT copies the time onto the picture, which finds its times by it.
  sample->SetSampleTime(times.pts_us * 10);
  if (key) {
    sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
  }
  pending_.push_back(times);
  if (pending_.size() > kMaxPending) {
    pending_.pop_front();
  }
  HRESULT result = mft_->ProcessInput(0, sample.Get(), 0);
  if (result == MF_E_NOTACCEPTING) {
    Drain();
    result = mft_->ProcessInput(0, sample.Get(), 0);
  }
  if (FAILED(result)) {
    std::fprintf(stderr, "xv: ProcessInput failed (%08lx)\n", result);
    return;
  }
  Drain();
}

void Decoder::Drain() {
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER output{};
    output.pSample = provides_samples_ ? nullptr : output_.Get();
    DWORD status = 0;
    HRESULT result = mft_->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) {
      output.pEvents->Release();
    }
    ComPtr<IMFSample> provided;
    if (provides_samples_) {
      provided.Attach(output.pSample);  // The MFT handed us its reference.
    }
    if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (!SetOutputType()) {
        return;
      }
      continue;
    }
    if (FAILED(result)) {
      if (result != MF_E_TRANSFORM_NEED_MORE_INPUT) {
        std::fprintf(stderr, "xv: ProcessOutput failed (%08lx)\n", result);
      }
      return;
    }
    Publish(provides_samples_ ? provided.Get() : output_.Get());
  }
}

void Decoder::Publish(IMFSample* sample) {
  ComPtr<IMFMediaBuffer> buffer;
  LONGLONG time = 0;
  if (!sample || FAILED(sample->GetBufferByIndex(0, &buffer))) {
    return;
  }
  sample->GetSampleTime(&time);
  // Pictures leave in input order; inputs that yielded none are skipped.
  while (pending_.size() > 1 && pending_.front().pts_us * 10 != time) {
    pending_.pop_front();
  }
  FrameTimes times{};
  if (!pending_.empty()) {
    times = pending_.front();
    pending_.pop_front();
  }

  ComPtr<IMFDXGIBuffer> surface;
  ComPtr<ID3D11Texture2D> source;
  UINT index = 0;
  D3D11_TEXTURE2D_DESC desc{};
  bool dxva = SUCCEEDED(buffer.As(&surface)) &&
              SUCCEEDED(surface->GetResource(IID_PPV_ARGS(&source))) &&
              SUCCEEDED(surface->GetSubresourceIndex(&index));
  if (dxva) {
    source->GetDesc(&desc);
  } else {
    desc.Width = coded_width_;
    desc.Height = coded_height_;
  }
  uint64_t n = published_.load(std::memory_order_relaxed);
  if (n + 2 - shown_.load(std::memory_order_acquire) >= kSlots) {
    ++drops_;  // The renderer stalled: it keeps the pictures it has queued.
    return;
  }
  Picture& picture = pictures_[n % kSlots];
  if (picture.texture_width != desc.Width ||
      picture.texture_height != desc.Height) {
    picture = Picture{};
    D3D11_TEXTURE2D_DESC own{};
    own.Width = desc.Width;
    own.Height = desc.Height;
    own.MipLevels = 1;
    own.ArraySize = 1;
    own.Format = DXGI_FORMAT_NV12;
    own.SampleDesc.Count = 1;
    own.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    HRESULT result = device_->CreateTexture2D(&own, nullptr, &picture.texture);
    if (FAILED(result)) {
      // Mostly a lost device: the renderer's next Present closes the viewer.
      std::fprintf(stderr, "xv: picture texture failed (%08lx)\n", result);
      return;
    }
    view.Format = DXGI_FORMAT_R8_UNORM;
    device_->CreateShaderResourceView(picture.texture.Get(), &view,
                                      &picture.luma);
    view.Format = DXGI_FORMAT_R8G8_UNORM;
    device_->CreateShaderResourceView(picture.texture.Get(), &view,
                                      &picture.chroma);
    picture.texture_width = desc.Width;
    picture.texture_height = desc.Height;
  }

  // Copy out at once: the MFT has only a few surfaces and reuses them.
  lock_->Enter();
  if (dxva) {
    context_->CopySubresourceRegion(picture.texture.Get(), 0, 0, 0, 0,
                                    source.Get(), index, nullptr);
  } else if (BYTE* data; SUCCEEDED(buffer->Lock(&data, nullptr, nullptr))) {
    // One NV12 subresource: the chroma rows follow the luma rows.
    context_->UpdateSubresource(picture.texture.Get(), 0, nullptr, data,
                                stride_, 0);
    buffer->Unlock();
  }
  if (fence_) {
    context_->Signal(fence_.Get(), ++fence_value_);
    fence_->SetEventOnCompletion(fence_value_, fence_event_);
  }
  context_->Flush();
  lock_->Leave();
  // Decode time ends when the GPU is done, and the renderer never waits on
  // the GPU for a picture.
  if (fence_) {
    WaitForSingleObject(fence_event_, 100);
  }
  dxva_ = dxva;
  picture.width = width_;
  picture.height = height_;
  picture.times = times;
  picture.times.decoded_us = NowUs();
  if (on_frame) {
    on_frame(picture.times);
  }
  published_.store(n + 1, std::memory_order_release);
  SetEvent(frame_event_);
}

const Picture* Decoder::Acquire(uint32_t step) {
  uint64_t shown = shown_.load(std::memory_order_relaxed);
  step = std::min(step, Queued());
  if (step) {
    drops_ += step - 1;
    shown_.store(shown += step, std::memory_order_release);
  }
  return shown ? &pictures_[(shown - 1) % kSlots] : nullptr;
}

}  // namespace xv
