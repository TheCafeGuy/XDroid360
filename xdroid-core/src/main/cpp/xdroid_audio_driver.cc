// XDroid360 AAudio output. See xdroid_audio_driver.h for the threading model.
#include "xdroid_audio_driver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_system.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"

DEFINE_string(apu_aaudio_performance_mode, "power_saving",
              "AAudio performance mode: power_saving (fewer, larger device "
              "callbacks), none, or low_latency (more wakeups; the guest "
              "queue already holds tens of milliseconds).",
              "APU");
DECLARE_uint32(apu_max_queued_frames);
DECLARE_bool(xdroid_diagnostics);
DEFINE_uint32(apu_queue_bursts, 3,
              "Device bursts the guest audio queue covers (at least 2). Two "
              "leave no margin for a slow guest callback while the pump refills "
              "after each burst (power_saving bursts are 40 ms; underruns with "
              "a fresh producer, 2026-10-11).",
              "APU");

DEFINE_bool(xdroid_audio_conceal, true,
            "XDroid360 (AUD-2 arm D2): on an audio underrun, loop the last "
            "~10 ms of output (crossfaded, released after 40 ms) instead of "
            "fading to silence, so a short producer delay is not an audible "
            "cut. Live.",
            "APU");

DEFINE_uint32(xdroid_audio_drc_ppm, 0,
              "XDroid360 (AUD-5): dynamic rate control. When the guest queue "
              "is short of full at a device callback, play up to this many "
              "ppm slower (5000 = 0.5%, about 9 cents of pitch), scaled by "
              "how far behind the producer is (full at one device burst "
              "behind). The pump follows the output, so a producer that runs "
              "just under real time (AC6 hot: 5.17 of 5.33 ms per frame) is "
              "slowed a little instead of underrunning. 0 = off. Changes the "
              "guest's audio clock by at most this much while engaged. Live.",
              "APU");
DEFINE_bool(xdroid_audio_route_watch, false,
            "XDroid360 (AUD-5): every 500 ms, check the output device and "
            "burst size; when a route change did not disconnect the stream "
            "(Bluetooth headphones connecting), resize the guest queue for "
            "the new burst, which otherwise underruns every callback until a "
            "restart. Live.",
            "APU");

namespace xdroid {

namespace {
int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

aaudio_performance_mode_t ParsePerformanceMode(const std::string& name) {
  if (name == "low_latency") {
    return AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
  }
  if (name == "none") {
    return AAUDIO_PERFORMANCE_MODE_NONE;
  }
  return AAUDIO_PERFORMANCE_MODE_POWER_SAVING;
}

const char* PerformanceModeName(aaudio_performance_mode_t mode) {
  switch (mode) {
    case AAUDIO_PERFORMANCE_MODE_LOW_LATENCY:
      return "low_latency";
    case AAUDIO_PERFORMANCE_MODE_POWER_SAVING:
      return "power_saving";
    default:
      return "none";
  }
}

// Unity below the knee, then a smooth curve that approaches full scale. The
// 6 -> 2 downmix sums up to six channels into each side, so loud scenes go
// past 1.0, and AAudio hard-clips float output (heard as crackle).
inline float SoftLimit(float x, uint32_t& limited) {
  constexpr float kKnee = 0.9f;
  constexpr float kRoom = 1.0f - kKnee;
  const float magnitude = std::fabs(x);
  if (magnitude <= kKnee) {
    return x;
  }
  ++limited;
  return std::copysign(kKnee + kRoom * std::tanh((magnitude - kKnee) / kRoom),
                       x);
}

void LogIfFailed(const char* what, aaudio_result_t result) {
  if (result != AAUDIO_OK) {
    XELOGW("XDroid360: AAudio {} failed: {}", what,
           AAudio_convertResultToText(result));
  }
}

constexpr auto kFramePeriod = std::chrono::microseconds(
    1000000ull * xe::apu::AudioDriver::kChannelSamplesDefault /
    xe::apu::AudioDriver::kFrameFrequencyDefault);

}  // namespace

XDroidAAudioDriver::XDroidAAudioDriver(xe::threading::Semaphore* semaphore)
    : semaphore_(semaphore) {}

XDroidAAudioDriver::~XDroidAAudioDriver() { Shutdown(); }

bool XDroidAAudioDriver::Initialize() {
  ring_ = std::make_unique<float[]>(size_t(kRingSlots) * kFrameFloats);
  performance_mode_ =
      ParsePerformanceMode(cvars::apu_aaudio_performance_mode);
  AAudioStream* stream = OpenStream();
  if (!stream) {
    return false;
  }
  const aaudio_result_t result = AAudioStream_requestStart(stream);
  if (result != AAUDIO_OK) {
    XELOGE("XDroid360: AAudio start failed: {}",
           AAudio_convertResultToText(result));
    AAudioStream_close(stream);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    stream_ = stream;
  }
  service_thread_ = std::thread(&XDroidAAudioDriver::ServiceThreadMain, this);
  return true;
}

namespace {
// The driver WriteAudioClip reads; the mutex keeps it alive meanwhile.
std::mutex clip_driver_mutex;
XDroidAAudioDriver* clip_driver = nullptr;
}  // namespace

bool WriteAudioClip(const std::filesystem::path& wav,
                    const std::filesystem::path& events_csv) {
  std::lock_guard<std::mutex> lock(clip_driver_mutex);
  XDroidAAudioDriver* driver = clip_driver;
  if (!driver || !driver->clip_ring_) {
    return false;
  }
  const int32_t rate = driver->clip_rate_.load(std::memory_order_acquire);
  const uint64_t end =
      driver->clip_pairs_written_.load(std::memory_order_acquire);
  if (rate <= 0 || !end) {
    return false;
  }
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  const uint64_t pairs =
      std::min<uint64_t>({end, uint64_t(rate) * 7,
                          uint64_t(XDroidAAudioDriver::kClipPairs)});
  // The callback keeps writing: the oldest few ms may be newer audio.
  std::vector<int16_t> pcm(size_t(pairs) * 2);
  for (uint64_t i = 0; i < pairs; ++i) {
    const size_t at =
        size_t((end - pairs + i) % XDroidAAudioDriver::kClipPairs) * 2;
    for (int c = 0; c < 2; ++c) {
      const float v = std::clamp(driver->clip_ring_[at + c], -1.0f, 1.0f);
      pcm[size_t(i) * 2 + c] = int16_t(std::lround(v * 32767.0f));
    }
  }
  if (FILE* file = xe::filesystem::OpenFile(wav, "wb")) {
    const uint32_t data_bytes = uint32_t(pcm.size() * sizeof(int16_t));
    auto u32 = [file](uint32_t v) { std::fwrite(&v, 4, 1, file); };
    auto u16 = [file](uint16_t v) { std::fwrite(&v, 2, 1, file); };
    std::fwrite("RIFF", 1, 4, file);
    u32(36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, file);
    u32(16);
    u16(1);  // PCM
    u16(2);
    u32(uint32_t(rate));
    u32(uint32_t(rate) * 4);
    u16(4);
    u16(16);
    std::fwrite("data", 1, 4, file);
    u32(data_bytes);
    std::fwrite(pcm.data(), 1, data_bytes, file);
    std::fclose(file);
  }
  if (FILE* file = xe::filesystem::OpenFile(events_csv, "w")) {
    // t_ms on the steady clock of frames.csv (t_us / 1000); wav_s = the
    // position in the WAV.
    const int64_t wav_start_ms = now_ms - int64_t(pairs * 1000 / rate);
    std::fprintf(file, "event,t_ms,wav_s\n");
    const uint64_t count =
        driver->clip_underruns_.load(std::memory_order_acquire);
    for (uint64_t k = count > XDroidAAudioDriver::kClipUnderruns
                          ? count - XDroidAAudioDriver::kClipUnderruns
                          : 0;
         k < count; ++k) {
      const int64_t t =
          driver->clip_underrun_ms_[k % XDroidAAudioDriver::kClipUnderruns]
              .load(std::memory_order_relaxed);
      if (t >= wav_start_ms) {
        std::fprintf(file, "underrun,%lld,%.2f\n", static_cast<long long>(t),
                     double(t - wav_start_ms) / 1000.0);
      }
    }
    std::fprintf(file, "written,%lld,%.2f\n", static_cast<long long>(now_ms),
                 double(now_ms - wav_start_ms) / 1000.0);
    std::fclose(file);
  }
  return true;
}

void XDroidAAudioDriver::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(clip_driver_mutex);
    if (clip_driver == this) {
      clip_driver = nullptr;
    }
  }
  if (service_thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(service_mutex_);
      service_exit_ = true;
    }
    service_cv_.notify_all();
    service_thread_.join();
  }
  CloseStream();
  if (ring_) {
    XELOGI("XDroid360: audio closed: {} underruns, {} dropped frames, {} "
           "limited samples, {} non-finite samples",
           underruns_.load(), dropped_.load(), limited_.load(),
           nonfinite_.load());
    ring_.reset();
  }
}

AAudioStream* XDroidAAudioDriver::OpenStream() {
  AAudioStreamBuilder* builder = nullptr;
  if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
    return nullptr;
  }
  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setPerformanceMode(builder, performance_mode_);
  AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(builder, 2);
  AAudioStreamBuilder_setSampleRate(builder, kGuestRate);
  AAudioStreamBuilder_setDataCallback(builder, DataCallback, this);
  AAudioStreamBuilder_setErrorCallback(builder, ErrorCallback, this);
  AAudioStream* stream = nullptr;
  const aaudio_result_t result =
      AAudioStreamBuilder_openStream(builder, &stream);
  AAudioStreamBuilder_delete(builder);
  if (result != AAUDIO_OK) {
    XELOGE("XDroid360: AAudio open failed: {}",
           AAudio_convertResultToText(result));
    return nullptr;
  }
  if (AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_FLOAT ||
      AAudioStream_getChannelCount(stream) != 2) {
    XELOGE("XDroid360: AAudio did not grant float stereo");
    AAudioStream_close(stream);
    return nullptr;
  }
  // Read before the stream starts; the callback uses it afterwards.
  device_rate_ = AAudioStream_getSampleRate(stream);
  if (cvars::xdroid_diagnostics && !clip_ring_) {
    clip_ring_ = std::make_unique<float[]>(size_t(kClipPairs) * 2);
    std::lock_guard<std::mutex> lock(clip_driver_mutex);
    clip_driver = this;
  }
  const uint32_t depth = SizeQueue(stream);
  route_device_id_ = AAudioStream_getDeviceId(stream);
  route_burst_ = AAudioStream_getFramesPerBurst(stream);
  XELOGI("XDroid360: AAudio {} Hz, mode {}, burst {} frames, buffer {}/{}, "
         "guest queue {} frames ({} ms), device {}",
         device_rate_,
         PerformanceModeName(AAudioStream_getPerformanceMode(stream)),
         route_burst_, AAudioStream_getBufferSizeInFrames(stream),
         AAudioStream_getBufferCapacityInFrames(stream), depth,
         depth * 1000 * kChannelSamples / kGuestRate, route_device_id_);
  return stream;
}

uint32_t XDroidAAudioDriver::SizeQueue(AAudioStream* stream) {
  // The queue has to cover at least two device bursts (apu_queue_bursts,
  // plus one of margin by default): each callback takes a whole
  // burst, and the pump refills only what the previous one released. The
  // client semaphore stops at apu_max_queued_frames (16 = 85 ms), less than
  // one Bluetooth burst (7688 frames = 160 ms), which underran every callback
  // after switching to a headset.
  const int64_t burst = std::max(AAudioStream_getFramesPerBurst(stream), 1);
  const int64_t frame_device_samples = int64_t(device_rate_) * kChannelSamples;
  const uint32_t burst_frames = uint32_t(
      (burst * kGuestRate + frame_device_samples - 1) / frame_device_samples);
  const uint32_t semaphore_depth = std::clamp<uint32_t>(
      cvars::apu_max_queued_frames,
      uint32_t(xe::apu::AudioSystem::kMinimumQueuedFrames),
      uint32_t(xe::apu::AudioSystem::kMaximumQueuedFrames));
  const uint32_t depth = std::min(
      kRingSlots,
      std::max(semaphore_depth,
               std::max(cvars::apu_queue_bursts, 2u) * burst_frames));
  credit_target_.store(depth - semaphore_depth, std::memory_order_relaxed);
  queue_depth_.store(depth, std::memory_order_relaxed);
  burst_frames_.store(std::max(burst_frames, 1u), std::memory_order_relaxed);
  return depth;
}

void XDroidAAudioDriver::CheckRoute() {
  std::lock_guard<std::mutex> stream_lock(stream_mutex_);
  if (!stream_ || disconnected_.load()) {
    return;
  }
  const int32_t device_id = AAudioStream_getDeviceId(stream_);
  const int32_t burst = AAudioStream_getFramesPerBurst(stream_);
  if (device_id == route_device_id_ && burst == route_burst_) {
    return;
  }
  const uint32_t depth = SizeQueue(stream_);
  XELOGW("XDroid360: AAudio route changed without a disconnect: device {} -> "
         "{}, burst {} -> {} frames; guest queue now {} frames ({} ms)",
         route_device_id_, device_id, route_burst_, burst, depth,
         depth * 1000 * kChannelSamples / kGuestRate);
  route_device_id_ = device_id;
  route_burst_ = burst;
  // Look closely at the output for the next seconds.
  watch_request_.store(true, std::memory_order_relaxed);
}

void XDroidAAudioDriver::CloseStream() {
  std::lock_guard<std::mutex> lock(stream_mutex_);
  if (stream_) {
    // Close waits for a running data callback to return.
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }
}

void XDroidAAudioDriver::Pause() {
  std::lock_guard<std::mutex> lock(stream_mutex_);
  paused_ = true;
  if (stream_) {
    LogIfFailed("pause", AAudioStream_requestPause(stream_));
  }
}

void XDroidAAudioDriver::Resume() {
  std::lock_guard<std::mutex> lock(stream_mutex_);
  paused_ = false;
  // The producer was paused too: count its gap from now, and watch the
  // output level closely for the first seconds.
  last_submit_ms_.store(SteadyMs(), std::memory_order_relaxed);
  watch_request_.store(true, std::memory_order_relaxed);
  if (stream_) {
    // A stream that died while paused fails here; the error callback has
    // already handed it to the service thread, which reopens it.
    LogIfFailed("resume", AAudioStream_requestStart(stream_));
  }
}

void XDroidAAudioDriver::SubmitFrame(float* frame) {
  last_submit_ms_.store(SteadyMs(), std::memory_order_relaxed);
  const uint32_t tail = ring_tail_.load(std::memory_order_relaxed);
  const uint32_t head = ring_head_.load(std::memory_order_acquire);
  if (tail - head >= kRingSlots) {
    // Only reachable when the pump is not paced by the semaphore.
    const uint64_t dropped = dropped_.fetch_add(1) + 1;
    if (dropped == 1 || dropped % 4096 == 0) {
      XELOGW("XDroid360: audio ring full, {} frames dropped", dropped);
    }
    ReleaseSlots(1);
    return;
  }
  const uint32_t slot = tail % kRingSlots;
  std::memcpy(&ring_[size_t(slot) * kFrameFloats], frame,
              sizeof(float) * kFrameFloats);
  // Only this thread raises credit_queued_, so a stale read is high, never
  // low: the queue never goes past the target.
  const bool credit = credit_queued_.load(std::memory_order_relaxed) <
                      credit_target_.load(std::memory_order_relaxed);
  ring_credit_[slot] = credit;
  if (credit) {
    credit_queued_.fetch_add(1, std::memory_order_relaxed);
  }
  ring_tail_.store(tail + 1, std::memory_order_release);
  if (credit) {
    ReleaseSlots(1);
  }
}

void XDroidAAudioDriver::ReleaseSlots(uint32_t count) {
  if (count && semaphore_) {
    semaphore_->Release(int(count), nullptr);
  }
}

bool XDroidAAudioDriver::PopFrameToStage() {
  const uint32_t head = ring_head_.load(std::memory_order_relaxed);
  if (head == ring_tail_.load(std::memory_order_acquire)) {
    return false;
  }
  const uint32_t slot = head % kRingSlots;
  xe::apu::conversion::sequential_6_BE_to_interleaved_2_LE(
      stage_, &ring_[size_t(slot) * kFrameFloats], kChannelSamples);
  const bool credit = ring_credit_[slot];
  ring_head_.store(head + 1, std::memory_order_release);
  if (credit) {
    // Its semaphore slot went back on submit.
    credit_queued_.fetch_sub(1, std::memory_order_relaxed);
  } else {
    ++consumed_in_render_;
  }
  const float volume = volume_.load(std::memory_order_relaxed) *
                       float(std::min<uint32_t>(cvars::volume, 100)) / 100.0f;
  uint32_t limited = 0;
  uint32_t nonfinite = 0;
  float peak = 0.0f;
  for (float& sample : stage_) {
    float scaled = sample * volume;
    // The limiter maps inf to full scale and passes NaN through: either one
    // is heard as a full-volume burst.
    if (!std::isfinite(scaled)) {
      ++nonfinite;
      scaled = 0.0f;
    }
    peak = std::max(peak, std::fabs(scaled));
    sample = SoftLimit(scaled, limited);
  }
  if (limited) {
    limited_.fetch_add(limited, std::memory_order_relaxed);
  }
  if (nonfinite) {
    nonfinite_.fetch_add(nonfinite, std::memory_order_relaxed);
  }
  level_peak_ = std::max(level_peak_, peak);
  level_limited_ += limited;
  level_nonfinite_ += nonfinite;
  stage_pos_ = 0;
  return true;
}

bool XDroidAAudioDriver::NextPair(float& left, float& right) {
  if (stage_pos_ >= kChannelSamples && !PopFrameToStage()) {
    return false;
  }
  left = stage_[stage_pos_ * 2];
  right = stage_[stage_pos_ * 2 + 1];
  ++stage_pos_;
  return true;
}

void XDroidAAudioDriver::ConcealNext(float& left, float& right) {
  constexpr int32_t kSeam = kConcealFade;
  auto at = [this](int32_t index, int32_t channel) {
    return history_[((conceal_base_ + index) % kHistoryPairs) * 2 + channel];
  };
  left = at(conceal_pos_, 0);
  right = at(conceal_pos_, 1);
  if (conceal_pos_ >= conceal_loop_ - kSeam) {
    // Crossfade the end of the loop into its start, which plays from kSeam
    // after the wrap.
    const int32_t into = conceal_pos_ - (conceal_loop_ - kSeam);
    const float w = float(into + 1) / float(kSeam + 1);
    left = left * (1.0f - w) + at(into, 0) * w;
    right = right * (1.0f - w) + at(into, 1) * w;
  }
  if (++conceal_pos_ >= conceal_loop_) {
    conceal_pos_ = kSeam;
  }
}

void XDroidAAudioDriver::Render(float* out, int32_t pairs) {
  consumed_in_render_ = 0;
  int32_t written = 0;
  // AUD-5 rate control: how far short of full the queue is right before this
  // callback takes its burst. A producer keeping up refills it every period.
  const uint32_t drc_ppm = std::min<uint32_t>(cvars::xdroid_audio_drc_ppm,
                                              50000);  // 5% at most
  if (drc_ppm) {
    const uint32_t queued = ring_tail_.load(std::memory_order_acquire) -
                            ring_head_.load(std::memory_order_relaxed);
    const uint32_t depth = queue_depth_.load(std::memory_order_relaxed);
    const double deficit = depth > queued ? double(depth - queued) : 0.0;
    drc_deficit_ema_ += (deficit - drc_deficit_ema_) * 0.2;
    // One frame short is the pump's normal phase against the burst.
    const double behind = std::clamp(
        (drc_deficit_ema_ - 1.0) /
            double(burst_frames_.load(std::memory_order_relaxed)),
        0.0, 1.0);
    drc_slow_ppm_ = uint32_t(double(drc_ppm) * behind);
    drc_sum_ppm_ += drc_slow_ppm_;
    drc_max_ppm_ = std::max(drc_max_ppm_, drc_slow_ppm_);
    drc_sum_deficit_ += deficit;
    ++drc_calls_;
  } else {
    drc_slow_ppm_ = 0;
    drc_deficit_ema_ = 0.0;
  }
  // Once primed, stay on the resampler until an underrun resets it, so
  // turning rate control off live does not skip its pending sample.
  if (device_rate_ == int32_t(kGuestRate) && !drc_ppm && !resampler_primed_) {
    for (; written < pairs; ++written) {
      if (!NextPair(out[written * 2], out[written * 2 + 1])) {
        break;
      }
    }
  } else {
    const double step = double(kGuestRate) / double(device_rate_) *
                        (1.0 - double(drc_slow_ppm_) * 1e-6);
    for (; written < pairs; ++written) {
      bool starved = false;
      while (!resampler_primed_ || phase_ >= 1.0) {
        float left, right;
        if (!NextPair(left, right)) {
          starved = true;
          break;
        }
        if (resampler_primed_) {
          prev_[0] = next_[0];
          prev_[1] = next_[1];
          phase_ -= 1.0;
        } else {
          prev_[0] = left;
          prev_[1] = right;
          phase_ = 0.0;
          resampler_primed_ = true;
        }
        next_[0] = left;
        next_[1] = right;
      }
      if (starved) {
        break;
      }
      const float t = float(phase_);
      out[written * 2] = prev_[0] + (next_[0] - prev_[0]) * t;
      out[written * 2 + 1] = prev_[1] + (next_[1] - prev_[1]) * t;
      phase_ += step;
    }
  }
  if (concealing_ && written) {
    // Real audio is back: crossfade from the concealment loop into it.
    const int32_t fade = std::min(written, kConcealFade);
    for (int32_t i = 0; i < fade; ++i) {
      float left, right;
      ConcealNext(left, right);
      const float t = float(i + 1) / float(fade + 1);
      out[i * 2] = out[i * 2] * t + left * (1.0f - t);
      out[i * 2 + 1] = out[i * 2 + 1] * t + right * (1.0f - t);
    }
    concealing_ = false;
  }
  for (int32_t i = 0; i < written; ++i) {
    history_[history_pos_ * 2] = out[i * 2];
    history_[history_pos_ * 2 + 1] = out[i * 2 + 1];
    history_pos_ = (history_pos_ + 1) % kHistoryPairs;
  }
  history_filled_ = std::min(history_filled_ + written, kHistoryPairs);
  if (written) {
    hold_[0] = out[written * 2 - 2];
    hold_[1] = out[written * 2 - 1];
  }
  const int64_t now_ms = SteadyMs();
  if (drc_calls_ && now_ms - drc_log_ms_ >= 10000) {
    if (drc_log_ms_) {
      XELOGI("XDroid360: audio drc: slow mean {} ppm, max {} ppm over {} "
             "callbacks; queue deficit mean {:.1f} frames (depth {}, burst "
             "{})",
             drc_sum_ppm_ / drc_calls_, drc_max_ppm_, drc_calls_,
             drc_sum_deficit_ / drc_calls_,
             queue_depth_.load(std::memory_order_relaxed),
             burst_frames_.load(std::memory_order_relaxed));
    }
    drc_log_ms_ = now_ms;
    drc_sum_ppm_ = 0;
    drc_max_ppm_ = 0;
    drc_sum_deficit_ = 0.0;
    drc_calls_ = 0;
  }
  if (written < pairs) {
    const int32_t loop =
        std::clamp(device_rate_ / 100, 4 * kConcealFade, kHistoryPairs / 2);
    if (cvars::xdroid_audio_conceal && history_filled_ >= loop) {
      if (!concealing_) {
        concealing_ = true;
        conceal_loop_ = loop;
        conceal_base_ = (history_pos_ - loop + kHistoryPairs) % kHistoryPairs;
        conceal_pos_ = 0;
        conceal_pairs_ = 0;
      }
      const int32_t hold_pairs = device_rate_ / 25;     // 40 ms
      const int32_t release_pairs = device_rate_ / 50;  // 20 ms
      for (int32_t i = written; i < pairs; ++i) {
        float left, right;
        ConcealNext(left, right);
        float gain = 1.0f;
        if (conceal_pairs_ > hold_pairs) {
          gain = std::max(0.0f, 1.0f - float(conceal_pairs_ - hold_pairs) /
                                           float(release_pairs));
        }
        // Fade in from the last real pair over the first pairs.
        const float t = std::min(1.0f, float(conceal_pairs_ + 1) /
                                           float(kConcealFade + 1));
        hold_[0] *= 0.99f;
        hold_[1] *= 0.99f;
        out[i * 2] = (left * t + hold_[0] * (1.0f - t)) * gain;
        out[i * 2 + 1] = (right * t + hold_[1] * (1.0f - t)) * gain;
        ++conceal_pairs_;
      }
    } else {
      // Fade the last pair out instead of cutting to zero (no click).
      concealing_ = false;
      for (int32_t i = written; i < pairs; ++i) {
        hold_[0] *= 0.99f;
        hold_[1] *= 0.99f;
        out[i * 2] = hold_[0];
        out[i * 2 + 1] = hold_[1];
      }
    }
    resampler_primed_ = false;
    const uint64_t underruns = underruns_.fetch_add(1) + 1;
    ++level_underruns_;
    if (clip_ring_) {
      const uint64_t k = clip_underruns_.load(std::memory_order_relaxed);
      clip_underrun_ms_[k % kClipUnderruns].store(now_ms,
                                                  std::memory_order_relaxed);
      clip_underruns_.store(k + 1, std::memory_order_release);
    }
    // How long the producer (the guest audio callback) had not submitted:
    // tens of ms = the producer stalled; a few ms = output-side cadence.
    // Nothing to measure before its first frame.
    const int64_t last_submit_ms =
        last_submit_ms_.load(std::memory_order_relaxed);
    if (last_submit_ms) {
      const int64_t gap_ms = now_ms - last_submit_ms;
      underrun_gap_max_ms_ = std::max(underrun_gap_max_ms_, gap_ms);
      if (gap_ms >= 100) {
        // The producer stalled (guest or GPU hang); watch once it is back.
        producer_stalled_ = true;
      }
    }
    if (underruns == 1 || now_ms - underrun_log_ms_ >= 1000) {
      XELOGW("XDroid360: audio underrun #{} (+{}), producer gap max {} ms",
             underruns, underruns - underruns_logged_, underrun_gap_max_ms_);
      underruns_logged_ = underruns;
      underrun_log_ms_ = now_ms;
      underrun_gap_max_ms_ = 0;
    }
  }
  if (producer_stalled_ &&
      now_ms - last_submit_ms_.load(std::memory_order_relaxed) < 100) {
    // The first frames after a stall: one watch per stall, not one per
    // underrun while it lasts (this thread logs).
    producer_stalled_ = false;
    watch_request_.store(true, std::memory_order_relaxed);
  }
  double sum_sq = 0.0;
  for (int32_t i = 0; i < pairs * 2; ++i) {
    sum_sq += double(out[i]) * out[i];
  }
  level_sum_sq_ += sum_sq;
  level_samples_ += uint64_t(pairs) * 2;
  if (clip_ring_) {
    uint64_t at = clip_pairs_written_.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < pairs; ++i, ++at) {
      float* dst = &clip_ring_[size_t(at % kClipPairs) * 2];
      dst[0] = out[i * 2];
      dst[1] = out[i * 2 + 1];
    }
    clip_rate_.store(device_rate_, std::memory_order_relaxed);
    clip_pairs_written_.store(at, std::memory_order_release);
  }
  UpdateLevels(now_ms);
  ReleaseSlots(consumed_in_render_);
}

void XDroidAAudioDriver::UpdateLevels(int64_t now_ms) {
  if (watch_request_.exchange(false, std::memory_order_relaxed)) {
    // Resume or reconnect: start a fresh window on the new output.
    watch_until_ms_ = now_ms + 2000;
    level_window_ms_ = 0;
  }
  if (level_window_ms_) {
    const bool watching = now_ms < watch_until_ms_;
    if (now_ms - level_window_ms_ < (watching ? 250 : 1000)) {
      return;
    }
    const float rms =
        level_samples_
            ? float(std::sqrt(level_sum_sq_ / double(level_samples_)))
            : 0.0f;
    const bool loud = rms > 0.5f || level_nonfinite_ ||
                      uint64_t(level_limited_) * 100 > level_samples_;
    const char* tag = nullptr;
    if (watching) {
      tag = "watch";
    } else if (loud) {
      if (now_ms - level_loud_ms_ >= 5000) {
        tag = "loud";
        level_loud_ms_ = now_ms;
      } else {
        ++level_loud_skipped_;
      }
    } else if (now_ms - level_baseline_ms_ >= 30000) {
      tag = "level";
    }
    if (tag) {
      XELOGW(
          "XDroid360: audio {} {} ms: rms {:.3f} peak {:.2f} limited {} "
          "non-finite {} underruns {} queued {} volume {:.2f} (loud windows "
          "not logged {})",
          tag, now_ms - level_window_ms_, rms, level_peak_, level_limited_,
          level_nonfinite_, level_underruns_,
          ring_tail_.load(std::memory_order_acquire) -
              ring_head_.load(std::memory_order_relaxed),
          volume_.load(std::memory_order_relaxed) *
              float(std::min<uint32_t>(cvars::volume, 100)) / 100.0f,
          level_loud_skipped_);
      level_baseline_ms_ = now_ms;
      level_loud_skipped_ = 0;
    }
  } else if (!level_baseline_ms_) {
    level_baseline_ms_ = now_ms;
  }
  // Start the next window.
  level_window_ms_ = now_ms;
  level_peak_ = 0.0f;
  level_sum_sq_ = 0.0;
  level_samples_ = 0;
  level_limited_ = 0;
  level_nonfinite_ = 0;
  level_underruns_ = 0;
}

aaudio_data_callback_result_t XDroidAAudioDriver::DataCallback(
    AAudioStream* /*stream*/, void* user_data, void* audio_data,
    int32_t num_frames) {
  static_cast<XDroidAAudioDriver*>(user_data)->Render(
      static_cast<float*>(audio_data), num_frames);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void XDroidAAudioDriver::ErrorCallback(AAudioStream* /*stream*/,
                                       void* user_data,
                                       aaudio_result_t error) {
  // The stream must not be closed from this thread; hand it to the service
  // thread.
  auto* driver = static_cast<XDroidAAudioDriver*>(user_data);
  XELOGW("XDroid360: AAudio stream error: {}",
         AAudio_convertResultToText(error));
  {
    std::lock_guard<std::mutex> lock(driver->service_mutex_);
    driver->disconnected_.store(true);
  }
  driver->service_cv_.notify_all();
}

void XDroidAAudioDriver::ServiceThreadMain() {
  std::unique_lock<std::mutex> lock(service_mutex_);
  for (;;) {
    // Wake twice a second for the route watch (xdroid_audio_route_watch),
    // read on every timeout so it can be turned on live.
    if (!service_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
          return service_exit_ || disconnected_.load();
        })) {
      if (cvars::xdroid_audio_route_watch) {
        lock.unlock();
        CheckRoute();
        lock.lock();
      }
      continue;
    }
    if (service_exit_) {
      return;
    }
    lock.unlock();
    CloseStream();
    // No consumer is running now; start the next stream from a clean state.
    stage_pos_ = kChannelSamples;
    resampler_primed_ = false;
    auto next_open = std::chrono::steady_clock::now();
    auto next_drain = next_open;
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_open) {
        if (AAudioStream* stream = OpenStream()) {
          std::lock_guard<std::mutex> stream_lock(stream_mutex_);
          disconnected_.store(false);
          stream_ = stream;
          if (!paused_) {
            LogIfFailed("restart", AAudioStream_requestStart(stream_));
          }
          watch_request_.store(true, std::memory_order_relaxed);
          XELOGI("XDroid360: AAudio reconnected");
          break;
        }
        next_open = now + std::chrono::seconds(1);
      }
      // Without a device, play the queue into silence at the real-time rate
      // so a title waiting on its audio keeps running.
      bool paused;
      {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        paused = paused_;
      }
      if (!paused && now >= next_drain) {
        consumed_in_render_ = 0;
        PopFrameToStage();
        stage_pos_ = kChannelSamples;
        ReleaseSlots(consumed_in_render_);
        next_drain += kFramePeriod;
        if (next_drain < now) {
          next_drain = now + kFramePeriod;
        }
      }
      // While paused next_drain stays in the past; waiting for it would spin.
      const auto deadline =
          paused ? next_open : std::min(next_open, next_drain);
      lock.lock();
      service_cv_.wait_until(lock, deadline, [this] { return service_exit_; });
      const bool exit = service_exit_;
      lock.unlock();
      if (exit) {
        return;
      }
    }
    lock.lock();
  }
}

}  // namespace xdroid
