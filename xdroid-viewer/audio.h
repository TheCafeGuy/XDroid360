// MIRROR-1 PC viewer: audio output (WASAPI shared mode, adaptive jitter
// buffer, own resampler that holds the buffer at its target).
#pragma once

#include <cstdint>
#include <memory>

namespace xv {

class AudioOut {
 public:
  struct Stats {
    uint32_t buffered_us;  // Queued in the jitter buffer + device buffer.
    uint32_t target_us;    // Current adaptive target.
    uint32_t underruns;
    uint32_t overflows;
    double ratio;  // Resampler ratio, 1.0 = no correction.
  };

  AudioOut();
  ~AudioOut();
  // rate/channels of the incoming stream (48000 Hz, 2 ch s16). null_sink:
  // no device, a timer consumes at real-time pace (for --selftest).
  bool Start(uint32_t rate, uint32_t channels, bool null_sink);
  // Net thread, once per kAudio packet. arrival_us: NowUs() at receipt.
  // loss: the packet had kFlagLoss (samples lost before it).
  void Push(const int16_t* samples, uint32_t frames, int64_t arrival_us,
            bool loss);
  void Stop();
  Stats GetStats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Jitter buffer mode, live. Off (default): waits for the latest packet of
// the last minute, up to 250 ms, so the phone's audio bursts play whole. On:
// follows the p99 packet, up to 60 ms; every later packet is a chop.
void SetAudioLowLatency(bool on);
bool AudioLowLatency();

// --probe: prints the default render device's mix format and WASAPI periods
// (IAudioClient3 minimum / default). Never starts a stream or plays sound.
void ProbeAudio();

}  // namespace xv
