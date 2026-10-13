// XDroid360 audio system: AAudio output per guest client, or a silent sink
// when no stream can be opened, so titles still boot.
#ifndef XDROID_AUDIO_H_
#define XDROID_AUDIO_H_

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "xdroid_audio_driver.h"
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xdroid {

// Discards frames at the real-time rate. It has to give every slot back:
// a title that waits on its audio callback would otherwise freeze, and
// returning slots immediately would let the APU pump spin.
class SilentAudioDriver : public xe::apu::AudioDriver {
 public:
  explicit SilentAudioDriver(xe::threading::Semaphore* semaphore)
      : semaphore_(semaphore) {}
  bool Initialize() override { return true; }
  void Shutdown() override {}
  void SubmitFrame(float* /*frame*/) override {
    using Clock = std::chrono::steady_clock;
    constexpr auto kPeriod = std::chrono::microseconds(
        1000000ull * kChannelSamplesDefault / kFrameFrequencyDefault);
    const auto now = Clock::now();
    if (next_ < now - 4 * kPeriod) {
      next_ = now;
    }
    if (next_ > now) {
      xe::threading::NanoSleepPrecise(
          std::chrono::nanoseconds(next_ - now).count());
    }
    next_ += kPeriod;
    if (semaphore_) {
      semaphore_->Release(1, nullptr);
    }
  }
  void Pause() override {}
  void Resume() override {}
  void SetVolume(float /*volume*/) override {}

 private:
  xe::threading::Semaphore* semaphore_;
  std::chrono::steady_clock::time_point next_{};
};

class XDroidAudioSystem : public xe::apu::nop::NopAudioSystem {
 public:
  explicit XDroidAudioSystem(xe::cpu::Processor* processor)
      : NopAudioSystem(processor) {}

  std::string name() const override { return "AAudio"; }

  xe::X_STATUS CreateDriver(size_t /*index*/,
                            xe::threading::Semaphore* semaphore,
                            xe::apu::AudioDriver** out_driver) override {
    auto driver = std::make_unique<XDroidAAudioDriver>(semaphore);
    if (driver->Initialize()) {
      *out_driver = driver.release();
    } else {
      driver->Shutdown();
      XELOGW("XDroid360: no AAudio stream, audio is silent");
      *out_driver = new SilentAudioDriver(semaphore);
    }
    std::lock_guard<std::mutex> lock(drivers_mutex_);
    if (output_paused_) {
      (*out_driver)->Pause();
    }
    drivers_.push_back(*out_driver);
    return xe::X_STATUS(0);
  }

  void DestroyDriver(xe::apu::AudioDriver* driver) override {
    {
      std::lock_guard<std::mutex> lock(drivers_mutex_);
      drivers_.erase(std::remove(drivers_.begin(), drivers_.end(), driver),
                     drivers_.end());
    }
    driver->Shutdown();
    delete driver;
  }

  // Stops or restarts every output stream, so a backgrounded game neither
  // plays nor runs the consumer-paced guest callback. AudioSystem::Pause is
  // not used: it waits for the worker to reach a pause point, forever while
  // the worker is blocked inside a guest callback.
  void SetOutputPaused(bool paused) {
    std::lock_guard<std::mutex> lock(drivers_mutex_);
    output_paused_ = paused;
    for (xe::apu::AudioDriver* driver : drivers_) {
      if (paused) {
        driver->Pause();
      } else {
        driver->Resume();
      }
    }
  }

 private:
  std::mutex drivers_mutex_;
  std::vector<xe::apu::AudioDriver*> drivers_;
  bool output_paused_ = false;
};

}  // namespace xdroid

#endif  // XDROID_AUDIO_H_
