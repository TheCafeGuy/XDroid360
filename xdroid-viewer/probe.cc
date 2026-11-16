// MIRROR-1 PC viewer, --probe: what this PC offers the mirror (display
// paths, decoder, audio). Opens no window and plays nothing.
#include <dxgi1_6.h>

#include <cstdio>
#include <set>
#include <vector>

#include "audio.h"
#include "common.h"
#include "decode.h"

namespace xv {
namespace {

namespace mp = xdroid::mirror;

void PrintOverlay(IDXGIOutput3* output, ID3D11Device* device,
                  DXGI_FORMAT format, const char* name) {
  UINT flags = 0;
  if (FAILED(output->CheckOverlaySupport(format, device, &flags))) {
    flags = 0;
  }
  std::printf("    overlay %s: %s%s%s\n", name, flags ? "" : "none",
              flags & DXGI_OVERLAY_SUPPORT_FLAG_DIRECT ? "direct " : "",
              flags & DXGI_OVERLAY_SUPPORT_FLAG_SCALING ? "scaling" : "");
}

void ProbeDisplays() {
  ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    std::printf("dxgi: no factory\n");
    return;
  }
  BOOL tearing = FALSE;
  factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                               sizeof tearing);
  std::printf("tearing (ALLOW_TEARING): %s\n", tearing ? "yes" : "no");
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    std::printf("adapter %u: %ls (vendor %04x device %04x, %llu MB%s)\n", a,
                desc.Description, desc.VendorId, desc.DeviceId,
                (unsigned long long)(desc.DedicatedVideoMemory >> 20),
                desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ? ", software" : "");
    // CheckOverlaySupport asks about a device on this adapter.
    ComPtr<ID3D11Device> device;
    D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                      nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o) {
      DXGI_OUTPUT_DESC out;
      output->GetDesc(&out);
      DEVMODEW mode{};
      mode.dmSize = sizeof mode;
      EnumDisplaySettingsW(out.DeviceName, ENUM_CURRENT_SETTINGS, &mode);
      std::printf("  output %u: %ls %lux%lu @ %lu Hz\n", o, out.DeviceName,
                  mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency);
      UINT count = 0;
      output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count,
                                 nullptr);
      std::vector<DXGI_MODE_DESC> modes(count);
      output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count,
                                 modes.data());
      std::set<double> rates;
      for (const DXGI_MODE_DESC& m : modes) {
        if (m.Width == mode.dmPelsWidth && m.Height == mode.dmPelsHeight &&
            m.RefreshRate.Denominator) {
          rates.insert(double(m.RefreshRate.Numerator) /
                       m.RefreshRate.Denominator);
        }
      }
      std::printf("    refresh rates:");
      for (double rate : rates) {
        std::printf(" %.2f", rate);
      }
      std::printf("\n");
      ComPtr<IDXGIOutput2> output2;
      if (SUCCEEDED(output.As(&output2))) {
        std::printf("    overlays (MPO): %s\n",
                    output2->SupportsOverlays() ? "yes" : "no");
      }
      ComPtr<IDXGIOutput3> output3;
      if (device && SUCCEEDED(output.As(&output3))) {
        PrintOverlay(output3.Get(), device.Get(), DXGI_FORMAT_NV12, "NV12");
        PrintOverlay(output3.Get(), device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM,
                     "BGRA");
      }
    }
  }
}

// Decode latency of each access unit, fed at 30 fps like the app does.
void ProbeDecode(ID3D11Device* device, const wchar_t* name, uint32_t width,
                 uint32_t height) {
  std::vector<uint8_t> stream = ReadTestStream(name);
  if (stream.empty()) {
    std::printf("decode %ls: missing (build/testdata)\n", name);
    return;
  }
  Decoder decoder;
  std::vector<int64_t> times;
  decoder.on_frame = [&](const FrameTimes& t) {
    times.push_back(t.decoded_us - t.receipt_us);
  };
  if (!decoder.Start(device, mp::Codec::kAvc, width, height)) {
    std::printf("decode %ls: no decoder\n", name);
    return;
  }
  auto units = SplitAccessUnits(stream);
  for (size_t i = 0; i < units.size(); ++i) {
    std::vector<uint8_t> unit;
    bool key = false;
    for (std::span<const uint8_t> nal : units[i]) {
      unit.insert(unit.end(), nal.begin(), nal.end());
      key |= NalType(nal) == 5;
    }
    int64_t pts = int64_t(i) * 33333;
    decoder.Decode(unit, key, {pts, pts, NowUs(), 0});
    Sleep(33);
  }
  auto [p50, p95, max] = LatencyMs(times);
  std::printf(
      "decode %ls (%ux%u): %s, %zu/%zu frames, p50 %.2f p95 %.2f max %.2f "
      "ms\n",
      name, width, height, decoder.name(), times.size(), units.size(), p50, p95,
      max);
}

}  // namespace

int RunProbe() {
  ProbeDisplays();
  ComPtr<ID3D11Device> device = CreateDevice();
  if (device) {
    std::printf("decoder MFT: Microsoft H.264 (CLSID_CMSH264DecoderMFT)\n");
    ProbeDecode(device.Get(), L"test720.h264", 1280, 720);
    ProbeDecode(device.Get(), L"test1080.h264", 1920, 1080);
  }
  ProbeAudio();
  return 0;
}

}  // namespace xv
