// XDroid360 controller input. See xdroid_gamepad.h.
#include "xdroid_gamepad.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace xdroid {

using xe::X_RESULT;

namespace {

// Lock-free pad state: JNI writers and guest readers never block each
// other. The packet number changes whenever any field changes, which is
// what XInput titles use to detect new input.
struct PadState {
  std::atomic<uint16_t> buttons{0};
  std::atomic<int16_t> thumbs[4] = {};
  std::atomic<uint8_t> triggers[2] = {};
  std::atomic<uint32_t> packet{1};
};
PadState g_pad;

// Rest jitter of common sticks stays below this.
constexpr float kStickDeadzone = 0.06f;

int16_t StickValue(float value) {
  if (std::fabs(value) < kStickDeadzone) {
    return 0;
  }
  return int16_t(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
}

uint8_t TriggerValue(float value) {
  return uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

}  // namespace

X_RESULT XDroidGamepadDriver::GetCapabilities(
    uint32_t user_index, uint32_t /*flags*/,
    xe::hid::X_INPUT_CAPABILITIES* out_caps) {
  if (user_index != 0 || !out_caps) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  *out_caps = {};
  out_caps->type = xe::hid::XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = xe::hid::XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = int16_t(-1);
  out_caps->gamepad.thumb_ly = int16_t(-1);
  out_caps->gamepad.thumb_rx = int16_t(-1);
  out_caps->gamepad.thumb_ry = int16_t(-1);
  out_caps->vibration.left_motor_speed = 0xFFFF;
  out_caps->vibration.right_motor_speed = 0xFFFF;
  return X_ERROR_SUCCESS;
}

X_RESULT XDroidGamepadDriver::GetState(uint32_t user_index,
                                       xe::hid::X_INPUT_STATE* out_state) {
  if (user_index != 0 || !out_state) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  out_state->packet_number = g_pad.packet.load(std::memory_order_acquire);
  auto& pad = out_state->gamepad;
  pad.buttons = g_pad.buttons.load(std::memory_order_relaxed);
  pad.left_trigger = g_pad.triggers[0].load(std::memory_order_relaxed);
  pad.right_trigger = g_pad.triggers[1].load(std::memory_order_relaxed);
  pad.thumb_lx = g_pad.thumbs[0].load(std::memory_order_relaxed);
  pad.thumb_ly = g_pad.thumbs[1].load(std::memory_order_relaxed);
  pad.thumb_rx = g_pad.thumbs[2].load(std::memory_order_relaxed);
  pad.thumb_ry = g_pad.thumbs[3].load(std::memory_order_relaxed);
  return X_ERROR_SUCCESS;
}

X_RESULT XDroidGamepadDriver::SetState(
    uint32_t user_index, xe::hid::X_INPUT_VIBRATION* /*vibration*/) {
  // No rumble output yet; the request is accepted.
  return user_index == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT XDroidGamepadDriver::GetKeystroke(
    uint32_t user_index, uint32_t /*flags*/,
    xe::hid::X_INPUT_KEYSTROKE* /*out_keystroke*/) {
  return user_index == 0 ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::vector<xe::hid::InputDeviceInfo> XDroidGamepadDriver::EnumerateDevices() {
  xe::hid::InputDeviceInfo info;
  info.driver_slot = 0;
  info.stable_id = "xdroid-pad-0";
  info.display_name = "Android controller";
  info.subtype = xe::hid::XINPUT_DEVSUBTYPE_GAMEPAD;
  info.preferred_slot = 0;
  info.auto_bind = true;
  return {info};
}

void XDroidGamepadDriver::SetButton(uint16_t mask, bool down) {
  const uint16_t before =
      down ? g_pad.buttons.fetch_or(mask, std::memory_order_relaxed)
           : g_pad.buttons.fetch_and(uint16_t(~mask), std::memory_order_relaxed);
  const uint16_t after = down ? (before | mask) : (before & uint16_t(~mask));
  if (after != before) {
    g_pad.packet.fetch_add(1, std::memory_order_release);
  }
}

void XDroidGamepadDriver::SetAxis(GamepadAxis axis, float value) {
  bool changed = false;
  switch (axis) {
    case GamepadAxis::kLeftX:
    case GamepadAxis::kRightX:
    case GamepadAxis::kLeftY:
    case GamepadAxis::kRightY: {
      const bool is_y =
          axis == GamepadAxis::kLeftY || axis == GamepadAxis::kRightY;
      // Android Y grows downwards, XInput Y upwards.
      const int16_t stick = StickValue(is_y ? -value : value);
      changed = g_pad.thumbs[int(axis)].exchange(
                    stick, std::memory_order_relaxed) != stick;
      break;
    }
    case GamepadAxis::kLeftTrigger:
    case GamepadAxis::kRightTrigger: {
      const uint8_t trigger = TriggerValue(value);
      changed = g_pad.triggers[int(axis) - int(GamepadAxis::kLeftTrigger)]
                    .exchange(trigger, std::memory_order_relaxed) != trigger;
      break;
    }
  }
  if (changed) {
    g_pad.packet.fetch_add(1, std::memory_order_release);
  }
}

}  // namespace xdroid
