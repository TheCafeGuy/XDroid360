// XDroid360 controller input: one XInput pad in user slot 0, fed from
// Android key and motion events through JNI. The pad always reports as
// connected (zeros when idle) so titles never stop at a "connect a
// controller" screen.
#ifndef XDROID_GAMEPAD_H_
#define XDROID_GAMEPAD_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "xenia/hid/input_driver.h"
#include "xenia/xbox.h"

namespace xdroid {

// Axis ids shared with Bridge.java.
enum class GamepadAxis : int {
  kLeftX = 0,
  kLeftY = 1,
  kRightX = 2,
  kRightY = 3,
  kLeftTrigger = 4,
  kRightTrigger = 5,
};

class XDroidGamepadDriver : public xe::hid::InputDriver {
 public:
  XDroidGamepadDriver(xe::ui::Window* window, size_t window_z_order)
      : InputDriver(window, window_z_order) {}

  static std::unique_ptr<xe::hid::InputDriver> Create(xe::ui::Window* window,
                                                      size_t window_z_order) {
    return std::make_unique<XDroidGamepadDriver>(window, window_z_order);
  }

  xe::X_STATUS Setup() override { return xe::X_STATUS(0); }
  xe::X_RESULT GetCapabilities(
      uint32_t user_index, uint32_t flags,
      xe::hid::X_INPUT_CAPABILITIES* out_caps) override;
  xe::X_RESULT GetState(uint32_t user_index,
                        xe::hid::X_INPUT_STATE* out_state) override;
  xe::X_RESULT SetState(uint32_t user_index,
                        xe::hid::X_INPUT_VIBRATION* vibration) override;
  xe::X_RESULT GetKeystroke(
      uint32_t user_index, uint32_t flags,
      xe::hid::X_INPUT_KEYSTROKE* out_keystroke) override;
  xe::hid::InputType GetInputType() const override {
    return xe::hid::InputType::Controller;
  }
  std::vector<xe::hid::InputDeviceInfo> EnumerateDevices() override;

  // Any thread. The state lives outside the driver instance, so input that
  // arrives before the emulator is up is not lost.
  static void SetButton(uint16_t mask, bool down);
  // Raw Android axis value: sticks -1..1 (Y down), triggers 0..1.
  static void SetAxis(GamepadAxis axis, float value);
};

}  // namespace xdroid

#endif  // XDROID_GAMEPAD_H_
