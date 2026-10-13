/**
 * XDroid360: live cvar changes for on-device A/B runs (PES-1).
 */

#include "xdroid_live.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "xdroid_gamepad.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

namespace xdroid {
namespace {

// Cvars that are read on every use (or whose user notices the change and
// rebuilds, like the presenter's swapchain), so changing them mid-run takes
// effect without a restart. Anything else would only half-apply.
constexpr std::string_view kLiveCvars[] = {
    "present_prerotate",
    "present_prerotate_flip",
    "vulkan_allow_present_mode_immediate",
    "vulkan_allow_present_mode_mailbox",
    "vulkan_allow_present_mode_fifo_relaxed",
    "timer_console_tick",
    "guest_spin_park",
    "guest_spin_park_hints",
    "guest_spin_park_timeout_us",
    "xdroid_stall_inject_every_s",
    "xdroid_stall_inject_ms",
    "xdroid_glitch_recorder",
    "vulkan_direct_resolve_transient_dest",
    "vulkan_direct_resolve_post_barrier",
    "vulkan_direct_resolve_transient_depth",
    "xdroid_depth_read_repeat",
    "xdroid_depth_read_skip_stencil",
    "ac6_storage_submit_order_fix",
    "ac6_job_start_order",
    "ac6_job_start_wait_us",
    "xdroid_cp_wait_jobs",
    "xdroid_cp_wait_jobs_frame_us",
    "xdroid_vdswap_wait_cp",
    "xdroid_vdswap_wait_cp_max_lag",
    "xdroid_vdswap_wait_cp_us",
    "xdroid_cp_defer_to_swap",
    "xdroid_cp_defer_to_swap_us",
    "xdroid_vfetch_reupload_max_kb",
    "xdroid_draw_blackbox",
    "xdroid_job_tracer",
    "xdroid_audio_conceal",
    "xdroid_audio_worker_nice",
    "xdroid_audio_producer_boost",
    "xdroid_time_coupling",
    "readback_resolve",
    "xdroid_time_coupling_slow_pct",
    "xdroid_time_coupling_due_pct",
    "xdroid_time_coupling_max_ms",
};

bool IsLiveCvar(std::string_view name) {
  for (std::string_view live : kLiveCvars) {
    if (live == name) {
      return true;
    }
  }
  return false;
}

// Virtual pad for unattended runs (IS-2): `pad = "A+DPAD_DOWN:150"` holds the
// buttons (and `LX=-1`-style axis values) for the given milliseconds, then
// releases them, on its own thread. The host changes another key along with
// it (a sequence number) so that repeating a press still changes the file.
struct PadName {
  std::string_view name;
  uint16_t mask;
};
constexpr PadName kPadButtons[] = {
    {"DPAD_UP", 0x0001}, {"DPAD_DOWN", 0x0002}, {"DPAD_LEFT", 0x0004},
    {"DPAD_RIGHT", 0x0008}, {"START", 0x0010},  {"BACK", 0x0020},
    {"LTHUMB", 0x0040},  {"RTHUMB", 0x0080},     {"LB", 0x0100},
    {"RB", 0x0200},      {"A", 0x1000},          {"B", 0x2000},
    {"X", 0x4000},       {"Y", 0x8000},
};
struct PadAxisName {
  std::string_view name;
  GamepadAxis axis;
};
constexpr PadAxisName kPadAxes[] = {
    {"LX", GamepadAxis::kLeftX},       {"LY", GamepadAxis::kLeftY},
    {"RX", GamepadAxis::kRightX},      {"RY", GamepadAxis::kRightY},
    {"LT", GamepadAxis::kLeftTrigger}, {"RT", GamepadAxis::kRightTrigger},
};

void ApplyPad(const std::string& text) {
  std::string_view spec(text);
  uint32_t hold_ms = 120;
  if (size_t colon = spec.find(':'); colon != std::string_view::npos) {
    hold_ms = uint32_t(std::strtoul(std::string(spec.substr(colon + 1)).c_str(),
                                    nullptr, 10));
    spec = spec.substr(0, colon);
  }
  uint16_t mask = 0;
  std::vector<std::pair<GamepadAxis, float>> axes;
  while (!spec.empty()) {
    size_t plus = spec.find('+');
    std::string_view token = spec.substr(0, plus);
    spec = plus == std::string_view::npos ? std::string_view()
                                          : spec.substr(plus + 1);
    size_t equals = token.find('=');
    bool known = false;
    if (equals != std::string_view::npos) {
      for (const PadAxisName& axis : kPadAxes) {
        if (axis.name == token.substr(0, equals)) {
          axes.emplace_back(axis.axis,
                            std::strtof(std::string(token.substr(equals + 1))
                                            .c_str(),
                                        nullptr));
          known = true;
        }
      }
    } else {
      for (const PadName& button : kPadButtons) {
        if (button.name == token) {
          mask |= button.mask;
          known = true;
        }
      }
    }
    if (!known) {
      XELOGW("XDroid360: live pad: unknown input '{}'", token);
    }
  }
  XELOGI("XDroid360: live pad {} for {} ms", text, hold_ms);
  std::thread([mask, axes, hold_ms] {
    for (const auto& [axis, value] : axes) {
      XDroidGamepadDriver::SetAxis(axis, value);
    }
    if (mask) {
      XDroidGamepadDriver::SetButton(mask, true);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    if (mask) {
      XDroidGamepadDriver::SetButton(mask, false);
    }
    for (const auto& [axis, value] : axes) {
      XDroidGamepadDriver::SetAxis(axis, 0.0f);
    }
  }).detach();
}

// By node type: value<bool>() also converts integers (2 printed as "true").
std::string NodeText(const toml::node& node) {
  if (const auto* value = node.as_boolean()) {
    return value->get() ? "true" : "false";
  }
  if (const auto* value = node.as_integer()) {
    return std::to_string(value->get());
  }
  if (const auto* value = node.as_floating_point()) {
    return std::to_string(value->get());
  }
  if (const auto* value = node.as_string()) {
    return value->get();
  }
  return "?";
}

void ApplyTable(const toml::table& table) {
  for (auto&& [key, node] : table) {
    if (const toml::table* subtable = node.as_table()) {
      // Sections are allowed, as in the config file.
      ApplyTable(*subtable);
      continue;
    }
    const std::string name(key.str());
    if (name == "pad") {
      ApplyPad(NodeText(node));
      continue;
    }
    cvar::IConfigVar* config_var = nullptr;
    if (cvar::ConfigVars && IsLiveCvar(name)) {
      auto it = cvar::ConfigVars->find(name);
      if (it != cvar::ConfigVars->end()) {
        config_var = it->second;
      }
    }
    if (config_var) {
      // Plain stores to word-sized cvars that other threads read without
      // synchronization: they see either value, which is all a switch needs.
      config_var->LoadConfigValue(&node);
      XELOGI("XDroid360: live cvar {} = {}", name, NodeText(node));
    } else {
      XELOGI("XDroid360: live marker {} = {}", name, NodeText(node));
    }
  }
}

void PollLoop(std::filesystem::path path) {
  std::filesystem::file_time_type last_write{};
  uintmax_t last_size = 0;
  bool seen = false;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::error_code error;
    const auto write_time = std::filesystem::last_write_time(path, error);
    if (error) {
      seen = false;
      continue;
    }
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (error || (seen && write_time == last_write && size == last_size)) {
      continue;
    }
    seen = true;
    last_write = write_time;
    last_size = size;
    try {
      toml::table table = toml::parse_file(path.string());
      ApplyTable(table);
    } catch (const toml::parse_error& e) {
      // Possibly caught mid-write; the next change of size or time retries.
      XELOGW("XDroid360: live config not parsed: {}", e.what());
      seen = false;
    }
  }
}

}  // namespace

void StartLiveConfig(const std::filesystem::path& path) {
  static std::once_flag started;
  std::call_once(started, [&path] {
    std::error_code error;
    std::filesystem::remove(path, error);
    // Detached: it only sleeps and touches cvars, and lives as long as the
    // process.
    std::thread(PollLoop, path).detach();
  });
}

}  // namespace xdroid
