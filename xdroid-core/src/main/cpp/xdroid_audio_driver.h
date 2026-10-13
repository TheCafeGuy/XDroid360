// XDroid360 AAudio output.
//
// Threads and ownership:
// - Producer: the APU pump thread calls SubmitFrame once per guest frame
//   (6 channels x 256 samples, big-endian, 48 kHz). The pump may only submit
//   after taking a slot from the client semaphore, and the driver gives the
//   slot back when the frame leaves (played, or dropped). When one device
//   burst is longer than the semaphore's depth allows (Bluetooth: 160 ms per
//   callback vs 85 ms), frames past that depth give their slot back on
//   submit instead, so the queue always covers two bursts. The ring holds
//   kRingSlots >= that depth, so a paced producer never finds it full.
// - Consumer: the AAudio data callback. It converts frames to stereo, applies
//   volume and a soft limiter, resamples if the device is not at 48 kHz, and
//   releases the consumed slots once per callback. No allocation and no
//   driver locks on this thread (the semaphore release takes the
//   semaphore's own short lock, once per callback).
// - Service thread: owned by the driver. After a disconnect (headset or
//   Bluetooth change) it closes the dead stream, reopens on the new route
//   with a 1 s retry, and meanwhile plays the queue into silence at the
//   real-time rate so audio-paced titles keep running.
#ifndef XDROID_AUDIO_DRIVER_H_
#define XDROID_AUDIO_DRIVER_H_

#include <aaudio/AAudio.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace xdroid {

class XDroidAAudioDriver : public xe::apu::AudioDriver {
 public:
  explicit XDroidAAudioDriver(xe::threading::Semaphore* semaphore);
  ~XDroidAAudioDriver() override;

  bool Initialize() override;
  void Shutdown() override;
  void SubmitFrame(float* frame) override;
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override {
    volume_.store(volume, std::memory_order_relaxed);
  }

 private:
  // Power of two, >= the apu_max_queued_frames upper bound (64).
  static constexpr uint32_t kRingSlots = 64;
  static constexpr uint32_t kChannels = kFrameChannelsDefault;
  static constexpr uint32_t kChannelSamples = kChannelSamplesDefault;
  static constexpr uint32_t kFrameFloats = kChannels * kChannelSamples;
  static constexpr uint32_t kGuestRate = kFrameFrequencyDefault;

  static aaudio_data_callback_result_t DataCallback(AAudioStream* stream,
                                                   void* user_data,
                                                   void* audio_data,
                                                   int32_t num_frames);
  static void ErrorCallback(AAudioStream* stream, void* user_data,
                            aaudio_result_t error);

  AAudioStream* OpenStream();
  void CloseStream();
  void ServiceThreadMain();

  // Consumer side (data callback, or the service thread while disconnected).
  void Render(float* out, int32_t pairs);
  bool PopFrameToStage();
  bool NextPair(float& left, float& right);
  void ReleaseSlots(uint32_t count);
  void UpdateLevels(int64_t now_ms);

  xe::threading::Semaphore* semaphore_;
  std::atomic<float> volume_{1.0f};

  // Stream lifecycle (Initialize, Shutdown, Pause, Resume, service thread).
  std::mutex stream_mutex_;
  AAudioStream* stream_ = nullptr;
  bool paused_ = false;
  aaudio_performance_mode_t performance_mode_ = AAUDIO_PERFORMANCE_MODE_NONE;
  int32_t device_rate_ = kGuestRate;

  // Service thread.
  std::thread service_thread_;
  std::mutex service_mutex_;
  std::condition_variable service_cv_;
  bool service_exit_ = false;
  std::atomic<bool> disconnected_{false};

  // SPSC ring: the producer writes slot [tail], the consumer reads [head].
  std::unique_ptr<float[]> ring_;
  std::atomic<uint32_t> ring_head_{0};
  std::atomic<uint32_t> ring_tail_{0};
  // Frames allowed past the semaphore's depth (two device bursts minus
  // apu_max_queued_frames, set on every stream open), how many are queued,
  // and per slot whether its frame already gave its semaphore slot back.
  std::atomic<uint32_t> credit_target_{0};
  std::atomic<uint32_t> credit_queued_{0};
  bool ring_credit_[kRingSlots] = {};

  // Consumer state: one converted stereo frame and the read position in it.
  float stage_[kChannelSamples * 2] = {};
  uint32_t stage_pos_ = kChannelSamples;
  uint32_t consumed_in_render_ = 0;
  // Linear resampler (only when the device is not at the guest rate).
  double phase_ = 0.0;
  float prev_[2] = {};
  float next_[2] = {};
  bool resampler_primed_ = false;
  // Last output pair, faded out on underrun instead of a hard cut.
  float hold_[2] = {};
  // Underrun concealment (xdroid_audio_conceal): the last real output pairs;
  // on an underrun the newest ~10 ms loop (crossfaded at the seam), faded in
  // from |hold_|, held for 40 ms, then released, and crossfaded back into the
  // real signal when it returns.
  void ConcealNext(float& left, float& right);
  static constexpr int32_t kHistoryPairs = 2048;
  static constexpr int32_t kConcealFade = 64;
  float history_[kHistoryPairs * 2] = {};
  int32_t history_pos_ = 0;
  int32_t history_filled_ = 0;
  bool concealing_ = false;
  int32_t conceal_base_ = 0;
  int32_t conceal_loop_ = 0;
  int32_t conceal_pos_ = 0;
  int32_t conceal_pairs_ = 0;

  // Output level windows (consumer side): 1 s, or 250 ms for 2 s after a
  // resume, a reconnect or a producer stall. Logged in those 2 s, when loud
  // (at most every 5 s) and every 30 s as a baseline.
  // Consumer-side logging is bounded: at most 8 watch lines per resume,
  // reconnect or stall, 1 loud line per 5 s and 1 baseline line per 30 s.
  std::atomic<bool> watch_request_{false};
  int64_t watch_until_ms_ = 0;
  bool producer_stalled_ = false;
  int64_t level_window_ms_ = 0;
  int64_t level_baseline_ms_ = 0;
  int64_t level_loud_ms_ = 0;
  // Peak before the limiter; sum of squares of the samples sent out.
  float level_peak_ = 0.0f;
  double level_sum_sq_ = 0.0;
  uint64_t level_samples_ = 0;
  uint32_t level_limited_ = 0;
  uint32_t level_nonfinite_ = 0;
  uint32_t level_underruns_ = 0;
  uint32_t level_loud_skipped_ = 0;

  // Counters, logged on shutdown and every 4096 drops; underruns (each one an
  // audible fade-out) at most once a second, from the render thread only.
  std::atomic<uint64_t> underruns_{0};
  uint64_t underruns_logged_ = 0;
  int64_t underrun_log_ms_ = 0;
  int64_t underrun_gap_max_ms_ = 0;
  // Steady-clock ms of the producer's last SubmitFrame.
  std::atomic<int64_t> last_submit_ms_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> limited_{0};
  std::atomic<uint64_t> nonfinite_{0};
};

}  // namespace xdroid

#endif  // XDROID_AUDIO_DRIVER_H_
