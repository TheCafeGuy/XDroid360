// MIRROR-1 PC viewer: audio output. The net thread pushes packets into a
// lock-free SPSC ring; the output thread pulls them through a windowed-sinc
// resampler whose ratio a PI loop trims so playback holds an adaptive line.
#include "audio.h"

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

#include "common.h"

namespace xv {
namespace {

using Microsoft::WRL::ComPtr;

// Start replaces impl_ on the network thread while the render thread may be
// in GetStats: the swap and the read take this lock.
std::mutex g_impl_swap;
std::atomic<bool> g_low_latency{false};

constexpr uint32_t kRingFrames = 1 << 15;  // 683 ms at 48 kHz.
// Kept free behind the read position: the underrun fade-out mirrors them.
constexpr uint32_t kGuardFrames = 1024;
constexpr int kTaps = 48;  // 0.5 ms of resampler delay at 48 kHz.
constexpr int kPhases = 256;
constexpr double kPassband = 0.92;  // Of the lower Nyquist rate: 22 kHz.
constexpr int64_t kFadeUs = 2500;
constexpr int64_t kMinTargetUs = 8000;
constexpr int64_t kMaxTargetUs = 60000;  // Low-latency mode.
// Safe mode: the phone's audio pump hands the stream over in bursts, 40 ms
// apart, often 75 and up to 154 ms (mirror1.log). The wait covers the latest
// packet of the last kLateBlocks blocks of kLateBlockS seconds, so a gap
// that long chops once, not every time it comes back.
constexpr int64_t kSafeTargetUs = 250000;
constexpr int kLateBlocks = 16;
constexpr uint32_t kLateBlockS = 4;
// On top of the latest packet: resampler taps, output wake and push jitter.
constexpr int64_t kMarginUs = 4000;
constexpr int64_t kOverflowUs = 40000;
// The line moves at 0.2 % of real time: the pitch shift that makes is
// 3.5 cents, below the ~5 cent threshold of hearing.
constexpr double kTargetSlew = 0.002;
constexpr double kMaxCorrection = 0.005;
constexpr double kLoopS = 1.0;      // PI time constant (critically damped).
constexpr int kJitterWindow = 384;  // Packets: ~2 s of 256 frames.
constexpr int64_t kJitterBinUs = 250;
constexpr int kJitterBins = 256;
// 0.5 s of arrivals before playback, whatever the packet size or the backlog
// a connection opens with: an estimate from less runs low, so the first late
// packets underrun.
constexpr int64_t kMinJitterUs = 500000;
constexpr int64_t kNullPeriodUs = 3000;
// PKEY_Device_FriendlyName, defined here to avoid an INITGUID include dance.
constexpr PROPERTYKEY kFriendlyName = {
    {0xa45c254e,
     0xdf1c,
     0x4efd,
     {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};

struct Frame {
  float l, r;
};

int64_t FramesToUs(uint64_t frames, uint32_t rate) {
  return int64_t(frames * 1000000 / rate);
}
uint64_t UsToFrames(int64_t us, uint32_t rate) {
  return us > 0 ? uint64_t(us) * rate / 1000000 : 0;
}

bool IsFloat(const WAVEFORMATEX* f) {
  const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
  return f->wBitsPerSample == 32 &&
         (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
          (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
}

double BesselI0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 32; ++k) {
    term *= x * x / (4.0 * k * k);
    sum += term;
  }
  return sum;
}

// Kaiser-windowed sinc, polyphase. Interpolating between adjacent phases makes
// any fractional position exact to ~1/kPhases^2, so the ratio is continuous.
class Resampler {
 public:
  // passband: cutoff as a fraction of the input Nyquist rate.
  void Init(double passband) {
    constexpr double kBeta = 8.0;  // ~80 dB stopband.
    for (int p = 0; p <= kPhases; ++p) {
      double sum = 0;
      for (int j = 0; j < kTaps; ++j) {
        const double d = j - (kTaps / 2 - 1) - double(p) / kPhases;
        const double x = 2 * d / kTaps, a = std::numbers::pi * passband * d;
        const double v = (a == 0 ? 1 : std::sin(a) / a) *
                         BesselI0(kBeta * std::sqrt(std::max(0.0, 1 - x * x)));
        table_[p][j] = float(v);
        sum += v;
      }
      // Unity gain at DC in every phase: no ripple as the phase moves.
      for (float& c : table_[p]) {
        c = float(c / sum);
      }
    }
    hist_ = {};
    frac_ = 0;
  }

  // One output frame. pull() supplies input frames as the position advances
  // by step input frames per output frame.
  template <typename Pull>
  Frame Next(double step, Pull pull) {
    for (; frac_ >= 1; frac_ -= 1) {
      hist_[pos_] = hist_[pos_ + kTaps] = pull();
      pos_ = (pos_ + 1) % kTaps;
    }
    const double phase = frac_ * kPhases;
    const int p = int(phase);
    const float t = float(phase - p);
    const float* h0 = table_[p].data();
    const float* h1 = table_[p + 1].data();
    const Frame* x = &hist_[pos_];
    Frame out{0, 0};
    for (int j = 0; j < kTaps; ++j) {
      const float c = h0[j] + t * (h1[j] - h0[j]);
      out.l += c * x[j].l;
      out.r += c * x[j].r;
    }
    frac_ += step;
    return out;
  }

  // Input frames the next n output frames consume, at most.
  uint64_t Need(uint32_t n, double step) const {
    return uint64_t(frac_ + n * step) + 1;
  }

 private:
  std::array<std::array<float, kTaps>, kPhases + 1> table_;
  std::array<Frame, 2 * kTaps> hist_{};  // Twice, so the window is linear.
  int pos_ = 0;
  double frac_ = 0;
};

}  // namespace

struct AudioOut::Impl {
  void Push(const int16_t* samples, uint32_t frames, int64_t arrival_us,
            bool loss);
  void Run();
  bool Open();
  void Close();
  bool Fill();
  void Render(float* out, uint32_t n, uint32_t pad);
  Frame Pull();
  void Splice(uint64_t drop);

  uint32_t in_rate_ = 0, in_channels_ = 0;
  bool null_sink_ = false;
  std::thread thread_;
  std::atomic<int> opened_{0};  // 1 open, -1 failed.
  std::atomic<bool> stop_{false}, started_{false};

  std::array<Frame, kRingFrames> ring_{};
  alignas(64) std::atomic<uint64_t> write_{0};  // Frames the net thread wrote.
  alignas(64) std::atomic<uint64_t> tail_{0};   // Frames the output freed.
  // On-time line: min over ~2 s of (arrival - stream time before the packet).
  // A packet that arrives on it has zero lateness.
  std::atomic<int64_t> anchor_us_{0};
  // Play line: a frame leaves the device this long after its stream time
  // (anchor + wait). Absolute, so the output does not hear the anchor's
  // noise under bursts. 0 until enough samples.
  std::atomic<int64_t> line_us_{0};
  std::atomic<uint64_t> gap_{0};  // Ring index of the first frame after a loss.
  std::atomic<int64_t> device_us_{0};  // Device write-ahead (buffer size).
  std::atomic<uint32_t> buffered_us_{0}, target_us_{0}, underruns_{0},
      overflows_{0};
  std::atomic<double> ratio_{1};

  // Net thread only.
  uint64_t w_ = 0;
  bool dropped_ = false;
  int64_t block_min_ = INT64_MAX;
  uint64_t block_frames_ = 0;
  std::array<int64_t, 4> blocks_ = {INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX};
  // Largest lateness per block, [0] the running one.
  std::array<int64_t, kLateBlocks> late_{};
  uint64_t late_frames_ = 0;
  int64_t first_us_ = 0;  // Arrival of the first packet.
  std::array<uint16_t, kJitterBins> hist_{};
  std::array<uint8_t, kJitterWindow> hist_ring_{};
  int hist_pos_ = 0, hist_n_ = 0;

  // Output thread only.
  ComPtr<IAudioClient> client_;
  ComPtr<IAudioRenderClient> render_;
  HANDLE wake_ = nullptr;  // Device event, or the null sink's timer.
  uint32_t out_rate_ = 0, out_channels_ = 0, buffer_frames_ = 0;
  int64_t t0_us_ = 0, written_ = 0;  // Null sink: virtual device clock.
  std::vector<float> scratch_;
  Resampler rs_;
  double rate_ratio_ = 1, correction_ = 0, integral_ = 0, line_ = 0;
  uint64_t r_ = 0, w_seen_ = 0, gap_at_ = 0, xf_from_ = 0, mirror_ = 0;
  uint32_t xf_len_ = 0, xf_pos_ = 0, xfade_frames_ = 0, fade_frames_ = 0;
  uint32_t fade_left_ = 0;
  bool muted_ = true;
  float gain_ = 0;
};

void AudioOut::Impl::Push(const int16_t* samples, uint32_t frames,
                          int64_t arrival_us, bool loss) {
  if (w_ + frames - tail_.load(std::memory_order_acquire) >
      kRingFrames - kGuardFrames) {
    dropped_ = true;  // Output stalled: drop, restart the line next packet.
    overflows_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int64_t offset = arrival_us - FramesToUs(w_, in_rate_);
  if (!w_) {
    first_us_ = arrival_us;
  }
  if (loss || dropped_) {
    // The stream jumped ahead of the frame count: restart the on-time line
    // and have the output crossfade across the gap.
    dropped_ = false;
    blocks_.fill(INT64_MAX);
    block_min_ = offset;
    gap_.store(w_, std::memory_order_release);
  }
  // Windowed minimum in half-second blocks, so clock drift cannot pile up.
  block_min_ = std::min(block_min_, offset);
  if ((block_frames_ += frames) >= in_rate_ / 2) {
    std::copy_backward(blocks_.begin(), blocks_.end() - 1, blocks_.end());
    blocks_[0] = block_min_;
    block_min_ = INT64_MAX;
    block_frames_ = 0;
  }
  const int64_t anchor =
      std::min(block_min_, *std::min_element(blocks_.begin(), blocks_.end()));
  anchor_us_.store(anchor, std::memory_order_relaxed);
  // Lateness against the on-time line of its own moment. A connection opens
  // with the phone's backlog (270 ms at once, 2026-11-15), which lowers the
  // line after the first packet: against the later line that packet held the
  // wait at its cap for the whole window.
  late_[0] = std::max(late_[0], offset - anchor);
  const int64_t latest = *std::max_element(late_.begin(), late_.end());
  if ((late_frames_ += frames) >= kLateBlockS * in_rate_) {
    std::copy_backward(late_.begin(), late_.end() - 1, late_.end());
    late_[0] = 0;
    late_frames_ = 0;
  }

  // Lateness histogram over the last kJitterWindow packets; p99 from the top.
  const int bin =
      int(std::min<int64_t>((offset - anchor) / kJitterBinUs, kJitterBins - 1));
  if (hist_n_ == kJitterWindow) {
    --hist_[hist_ring_[hist_pos_]];
  } else {
    ++hist_n_;
  }
  ++hist_[hist_ring_[hist_pos_] = uint8_t(bin)];
  hist_pos_ = (hist_pos_ + 1) % kJitterWindow;
  if (arrival_us - first_us_ >= kMinJitterUs) {
    int b = kJitterBins, above = 0;
    while (b > 0 && (above + hist_[b - 1]) * 100 <= hist_n_) {
      above += hist_[--b];
    }
    // Low latency: one bin above the p99 bin (when the estimate steps up a
    // bin, the level trails it by about the loop time constant). Safe: the
    // latest packet still has to be there when the device asks for it.
    const int64_t device = device_us_.load(std::memory_order_relaxed);
    line_us_.store(anchor + (AudioLowLatency()
                                 ? std::clamp((b + 1) * kJitterBinUs + device,
                                              kMinTargetUs, kMaxTargetUs)
                                 : std::clamp(latest + kMarginUs + device,
                                              kMinTargetUs, kSafeTargetUs)),
                   std::memory_order_relaxed);
  }

  for (uint32_t i = 0; i < frames; ++i) {
    const int16_t* s = samples + size_t(i) * in_channels_;
    ring_[(w_ + i) % kRingFrames] = {s[0] / 32768.0f,
                                     s[in_channels_ > 1] / 32768.0f};
  }
  w_ += frames;
  write_.store(w_, std::memory_order_release);
}

// Continues the stream `drop` frames later, crossfading over the jump.
void AudioOut::Impl::Splice(uint64_t drop) {
  xf_from_ = r_;
  xf_pos_ = 0;
  xf_len_ = uint32_t(std::min<uint64_t>(xfade_frames_, drop));
  r_ += drop;
}

Frame AudioOut::Impl::Pull() {
  if (!muted_ && gap_at_ > r_ && r_ + xfade_frames_ >= gap_at_) {
    Splice(gap_at_ - r_);
  }
  if (!muted_ && r_ >= w_seen_) {
    // Underrun: fade out on the stream mirrored at its last frame, so the
    // fade has material without holding back a reserve (latency).
    muted_ = true;
    fade_left_ = fade_frames_;
    mirror_ = xf_pos_ < xf_len_ ? xf_from_ + xf_pos_ : r_;
    xf_len_ = 0;
    underruns_.fetch_add(1, std::memory_order_relaxed);
  }
  if (muted_) {
    return ring_[--mirror_ % kRingFrames];
  }
  Frame f = ring_[r_++ % kRingFrames];
  if (xf_pos_ < xf_len_) {
    const Frame& o = ring_[(xf_from_ + xf_pos_) % kRingFrames];
    const float g = (xf_pos_++ + 0.5f) / xf_len_;
    f = {o.l + g * (f.l - o.l), o.r + g * (f.r - o.r)};
  }
  return f;
}

void AudioOut::Impl::Render(float* out, uint32_t n, uint32_t pad) {
  // Load gap_ before write_, so every frame before a seen gap is seen too.
  gap_at_ = std::max(gap_at_, gap_.load(std::memory_order_acquire));
  w_seen_ = write_.load(std::memory_order_acquire);
  const int64_t line = line_us_.load(std::memory_order_relaxed);
  const double dt = double(n) / out_rate_;
  // When the next input frame leaves the device, less its stream time: on the
  // line it plays on time, above it late. Arrival jitter does not move it.
  int64_t play =
      NowUs() + FramesToUs(pad, out_rate_) - FramesToUs(r_, in_rate_);
  // The line rises at once while silent (no audible cost), else slowly.
  const double slew = kTargetSlew * dt * 1e6;
  line_ = muted_ ? std::max(line_, double(line))
                 : line_ + std::clamp(line - line_, -slew, slew);
  // Frames too late to play on the line, and frames beyond what this call
  // reads at the fastest ratio, keeping room for a loss splice.
  const int64_t late = int64_t(UsToFrames(play - int64_t(line_), in_rate_));
  const int64_t spare =
      int64_t(w_seen_ - r_ - rs_.Need(n, rate_ratio_ * (1 + kMaxCorrection))) -
      int64_t(xfade_frames_);
  if (muted_) {
    // Drop what is too late (it played as silence), so a stall does not stay
    // as latency, and resume once the frame that plays on the line is
    // here. Dropping as data comes also keeps a burst from filling the ring.
    const int64_t skip = std::clamp<int64_t>(spare, 0, late);
    r_ += skip;
    play -= FramesToUs(skip, in_rate_);
    if (line > 0 && fade_left_ == 0 && play >= line_ && late <= spare) {
      muted_ = false;
      gain_ = 0;
    }
  } else {
    // A burst lands over several calls: drop the excess once all of it is
    // here, in one splice.
    if (play > line_ + kOverflowUs && late <= spare) {
      Splice(late);
      play -= FramesToUs(late, in_rate_);
      overflows_.fetch_add(1, std::memory_order_relaxed);
    }
    const double error = (play - line_) * 1e-6;
    const double p = 2 * error / kLoopS;
    // Integrate only while the output is not pinned at the limit: winding up
    // during a long drain would undershoot the target into underruns.
    if (std::fabs(p + (integral_ + error * dt) / (kLoopS * kLoopS)) <
        kMaxCorrection) {
      integral_ += error * dt;
    }
    correction_ = std::clamp(p + integral_ / (kLoopS * kLoopS), -kMaxCorrection,
                             kMaxCorrection);
  }
  const double step = rate_ratio_ * (1 + correction_);

  std::fill(out, out + size_t(n) * out_channels_, 0.0f);
  for (uint32_t i = 0; i < n && !(muted_ && fade_left_ == 0); ++i) {
    const Frame f = rs_.Next(step, [this] { return Pull(); });
    float g = gain_;
    gain_ = std::min(1.0f, gain_ + 1.0f / fade_frames_);
    if (muted_) {
      g *= float(fade_left_--) / fade_frames_;
    }
    float* o = out + size_t(i) * out_channels_;
    if (out_channels_ == 1) {
      o[0] = 0.5f * g * (f.l + f.r);
    } else {
      o[0] = g * f.l;  // Front left and right; other channels stay silent.
      o[1] = g * f.r;
    }
  }
  const uint64_t tail = xf_pos_ < xf_len_ ? xf_from_ + xf_pos_ : r_;
  // Only a running fade-out reads behind r_. Pinning the tail at mirror_ after
  // it would keep the ring full while muted: Push drops every packet, so the
  // jitter estimate never fills and the output never unmutes.
  tail_.store(muted_ && fade_left_ ? std::min(tail, mirror_) : tail,
              std::memory_order_release);
  // Both from the on-time line, as waits. Silent: nothing queued plays,
  // whatever the clock says. Before the first line the target reads 0.
  const int64_t anchor = anchor_us_.load(std::memory_order_relaxed);
  const auto wait = [anchor](int64_t at) {
    return uint32_t(std::clamp<int64_t>(at - anchor, 0, UINT32_MAX));
  };
  buffered_us_.store(muted_ ? 0 : wait(play), std::memory_order_relaxed);
  target_us_.store(wait(int64_t(line_)), std::memory_order_relaxed);
  ratio_.store(1 + correction_, std::memory_order_relaxed);
}

bool AudioOut::Impl::Fill() {
  UINT32 pad = 0;
  if (null_sink_) {
    // Virtual device: plays out_rate_ frames per second from t0_us_.
    const int64_t played = int64_t(UsToFrames(NowUs() - t0_us_, out_rate_));
    written_ = std::max(written_, played);
    pad = UINT32(written_ - played);
  } else if (FAILED(client_->GetCurrentPadding(&pad))) {
    return false;
  }
  const uint32_t n = buffer_frames_ - pad;
  float* out = scratch_.data();
  if (n == 0) {
    return true;
  }
  if (!null_sink_ &&
      FAILED(render_->GetBuffer(n, reinterpret_cast<BYTE**>(&out)))) {
    return false;
  }
  Render(out, n, pad);
  written_ += n;
  return null_sink_ || SUCCEEDED(render_->ReleaseBuffer(n, 0));
}

bool AudioOut::Impl::Open() {
  Close();
  if (null_sink_) {
    out_rate_ = in_rate_;
    out_channels_ = 2;
    // Two periods, like the engine's double buffer, so the target and the
    // loop see the same write-ahead as with a device.
    buffer_frames_ = uint32_t(UsToFrames(2 * kNullPeriodUs, out_rate_));
    scratch_.resize(size_t(buffer_frames_) * out_channels_);
    wake_ = CreateWaitableTimerExW(nullptr, nullptr,
                                   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -kNullPeriodUs * 10;  // Relative, 100 ns units.
    if (!wake_ || !SetWaitableTimer(wake_, &due, kNullPeriodUs / 1000, nullptr,
                                    nullptr, FALSE)) {
      return false;
    }
    t0_us_ = NowUs();
    written_ = 0;
  } else {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    WAVEFORMATEX* format = nullptr;
    const auto activate = [&] {
      return device->Activate(
          __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
          reinterpret_cast<void**>(client_.ReleaseAndGetAddressOf()));
    };
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
        FAILED(activate()) || FAILED(client_->GetMixFormat(&format))) {
      return false;
    }
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> owner(
        format, CoTaskMemFree);
    if (!IsFloat(format)) {
      std::fprintf(stderr, "audio: the mix format is not float32\n");
      return false;
    }
    ComPtr<IAudioClient3> client3;
    UINT32 def, fundamental, min_period, max_period;
    HRESULT hr = E_FAIL;
    if (SUCCEEDED(client_.As(&client3)) &&
        SUCCEEDED(client3->GetSharedModeEnginePeriod(
            format, &def, &fundamental, &min_period, &max_period))) {
      hr = client3->InitializeSharedAudioStream(
          AUDCLNT_STREAMFLAGS_EVENTCALLBACK, min_period, format, nullptr);
    }
    if (FAILED(hr)) {
      // A client whose Initialize failed is not reusable: take a new one.
      client3.Reset();
      if (FAILED(activate())) {
        return false;
      }
      hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               100000 /* 10 ms */, 0, format, nullptr);
    }
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !wake_ || FAILED(client_->SetEventHandle(wake_)) ||
        FAILED(client_->GetBufferSize(&buffer_frames_)) ||
        FAILED(client_->GetService(IID_PPV_ARGS(&render_)))) {
      return false;
    }
    out_rate_ = format->nSamplesPerSec;
    out_channels_ = format->nChannels;
  }
  rate_ratio_ = double(in_rate_) / out_rate_;
  rs_.Init(kPassband * std::min(1.0, 1 / rate_ratio_));
  fade_frames_ = std::max<uint32_t>(1, UsToFrames(kFadeUs, out_rate_));
  xfade_frames_ = uint32_t(UsToFrames(kFadeUs, in_rate_));
  device_us_.store(FramesToUs(buffer_frames_, out_rate_),
                   std::memory_order_relaxed);
  muted_ = true;
  fade_left_ = 0;
  // Pre-roll (silence) before Start, so the first period does not glitch.
  return Fill() && (null_sink_ || SUCCEEDED(client_->Start()));
}

void AudioOut::Impl::Close() {
  if (client_) {
    client_->Stop();
  }
  render_.Reset();
  client_.Reset();
  if (wake_) {
    CloseHandle(wake_);
  }
  wake_ = nullptr;
}

void AudioOut::Impl::Run() {
  const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  DWORD task = 0;
  // Pro Audio: real-time priority, so a wake every period (down to ~1 ms)
  // is not late behind the decoder and the UI.
  const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  const bool ok = Open();
  opened_.store(ok ? 1 : -1);
  opened_.notify_one();
  while (ok && !stop_.load(std::memory_order_relaxed)) {
    WaitForSingleObject(wake_, 100);
    if (Fill()) {
      continue;
    }
    // The device went away (unplugged, format change, audio service
    // restart): reopen the default one and resume from silence.
    underruns_.fetch_add(1, std::memory_order_relaxed);
    while (!Open() && !stop_.load(std::memory_order_relaxed)) {
      Sleep(500);
    }
  }
  Close();
  if (mmcss) {
    AvRevertMmThreadCharacteristics(mmcss);
  }
  if (com) {
    CoUninitialize();
  }
}

AudioOut::AudioOut() : impl_(std::make_unique<Impl>()) {}

AudioOut::~AudioOut() { Stop(); }

bool AudioOut::Start(uint32_t rate, uint32_t channels, bool null_sink) {
  if (rate == 0 || channels == 0 || impl_->thread_.joinable()) {
    return false;
  }
  // A restart begins from a clean state.
  auto fresh = std::make_unique<Impl>();
  {
    std::lock_guard lock(g_impl_swap);
    impl_.swap(fresh);
  }
  fresh.reset();  // Outside the lock: GetStats never waits on a free.
  Impl& m = *impl_;
  m.in_rate_ = rate;
  m.in_channels_ = channels;
  m.null_sink_ = null_sink;
  m.thread_ = std::thread([&m] { m.Run(); });
  m.opened_.wait(0);
  if (m.opened_.load() < 0) {
    m.thread_.join();
    return false;
  }
  m.started_.store(true, std::memory_order_release);
  return true;
}

void AudioOut::Push(const int16_t* samples, uint32_t frames, int64_t arrival_us,
                    bool loss) {
  if (impl_->started_.load(std::memory_order_acquire)) {
    impl_->Push(samples, frames, arrival_us, loss);
  }
}

void AudioOut::Stop() {
  impl_->started_.store(false);
  impl_->stop_.store(true);
  if (impl_->thread_.joinable()) {
    impl_->thread_.join();
  }
}

AudioOut::Stats AudioOut::GetStats() const {
  std::lock_guard lock(g_impl_swap);
  const Impl& m = *impl_;
  constexpr auto kRelaxed = std::memory_order_relaxed;
  return {m.buffered_us_.load(kRelaxed), m.target_us_.load(kRelaxed),
          m.underruns_.load(kRelaxed), m.overflows_.load(kRelaxed),
          m.ratio_.load(kRelaxed)};
}

void SetAudioLowLatency(bool on) { g_low_latency = on; }
bool AudioLowLatency() { return g_low_latency; }

void ProbeAudio() {
  const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IPropertyStore> props;
    ComPtr<IAudioClient3> client;
    WAVEFORMATEX* format = nullptr;
    PROPVARIANT name;
    PropVariantInit(&name);
    char utf8[256] = "?";
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
      std::printf("audio: no default render device\n");
    } else {
      if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
          SUCCEEDED(props->GetValue(kFriendlyName, &name)) &&
          name.vt == VT_LPWSTR) {
        WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, utf8, sizeof(utf8),
                            nullptr, nullptr);
      }
      std::printf("audio: device \"%s\"\n", utf8);
      UINT32 def, fundamental, min_period, max_period;
      if (FAILED(device->Activate(
              __uuidof(IAudioClient3), CLSCTX_ALL, nullptr,
              reinterpret_cast<void**>(client.GetAddressOf()))) ||
          FAILED(client->GetMixFormat(&format))) {
        std::printf("audio: IAudioClient3 unavailable\n");
      } else {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        std::printf(
            "audio: mix format %lu Hz, %u ch, %u-bit %s, channel mask 0x%lx\n",
            format->nSamplesPerSec, format->nChannels, format->wBitsPerSample,
            IsFloat(format) ? "float" : "int",
            format->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? ext->dwChannelMask
                                                         : 0ul);
        if (SUCCEEDED(client->GetSharedModeEnginePeriod(
                format, &def, &fundamental, &min_period, &max_period))) {
          const double ms = 1000.0 / format->nSamplesPerSec;
          std::printf(
              "audio: engine period frames: default %u (%.2f ms), "
              "fundamental %u (%.2f ms), min %u (%.2f ms), max %u (%.2f ms)\n",
              def, def * ms, fundamental, fundamental * ms, min_period,
              min_period * ms, max_period, max_period * ms);
        }
        CoTaskMemFree(format);
      }
    }
    PropVariantClear(&name);
  }
  if (com) {
    CoUninitialize();
  }
}

}  // namespace xv
