/**
 * XDroid360: virtual pad (IS-2) and pad script player and recorder (AUTO-1).
 * See docs/design/auto1-autonomous-bench.md section 1.
 */

#include "xdroid_pad_script.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "xdroid_ac6_jobs.h"
#include "xdroid_app.h"
#include "xdroid_gamepad.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/xdroid_gpu_hooks.h"

namespace xdroid {
namespace {

using SteadyClock = std::chrono::steady_clock;

struct PadName {
  std::string_view name;
  uint16_t mask;
};
constexpr PadName kPadButtons[] = {
    {"DPAD_UP", 0x0001},    {"DPAD_DOWN", 0x0002}, {"DPAD_LEFT", 0x0004},
    {"DPAD_RIGHT", 0x0008}, {"START", 0x0010},     {"BACK", 0x0020},
    {"LTHUMB", 0x0040},     {"RTHUMB", 0x0080},    {"LB", 0x0100},
    {"RB", 0x0200},         {"A", 0x1000},         {"B", 0x2000},
    {"X", 0x4000},          {"Y", 0x8000},
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

// Buttons and axis values named by a pad spec ("A+DPAD_DOWN+LY=-1"). Axes use
// the SetAxis convention (Y down).
struct PadInput {
  uint16_t buttons = 0;
  std::vector<std::pair<GamepadAxis, float>> axes;
};

// Adds every known token of |spec| to |out|; returns the first unknown token,
// empty when all are known.
std::string_view ParsePadSpec(std::string_view spec, PadInput& out) {
  std::string_view unknown;
  while (!spec.empty()) {
    const size_t plus = spec.find('+');
    const std::string_view token = spec.substr(0, plus);
    spec = plus == std::string_view::npos ? std::string_view()
                                          : spec.substr(plus + 1);
    const size_t equals = token.find('=');
    bool known = false;
    if (equals != std::string_view::npos) {
      for (const PadAxisName& axis : kPadAxes) {
        if (axis.name == token.substr(0, equals)) {
          out.axes.emplace_back(
              axis.axis,
              std::strtof(std::string(token.substr(equals + 1)).c_str(),
                          nullptr));
          known = true;
        }
      }
    } else {
      for (const PadName& button : kPadButtons) {
        if (button.name == token) {
          out.buttons |= button.mask;
          known = true;
        }
      }
    }
    if (!known && unknown.empty()) {
      unknown = token;
    }
  }
  return unknown;
}

// Last "Frame stats" window: a new value per window, passes x100 in the low
// half.
uint64_t FrameStats() {
  return xe::gpu::GetXdroidGpuHooks().frame_stats.load(
      std::memory_order_relaxed);
}

void Nap() { std::this_thread::sleep_for(std::chrono::milliseconds(4)); }

uint32_t TitleId() {
  XDroidApp* app = XDroidApp::Get();
  return app ? app->current_title_id() : 0;
}

// Ticks since launch (both counters start at 0 in each process): guest swaps
// when the AC6 hooks run, else log frames. Chosen at the first Now() so a
// script started before launch picks it after its `wait title`, as the
// recorder does.
struct Clock {
  const char* who;
  int swap = -1;
  uint64_t Now() {
    if (swap < 0) {
      swap = xe::gpu::GetXdroidGpuHooks().on_guest_swap.load(
                 std::memory_order_acquire) != nullptr;
      XELOGI("XDROID-PAD clock={} for={}", swap ? "swap" : "frame", who);
    }
    return swap ? Ac6GuestSwaps() : xe::logging::GetFrameNumber();
  }
};

// What the player set, so an end, a stop or teardown releases exactly that.
struct ScriptPad {
  uint16_t buttons = 0;
  uint8_t axes = 0;  // bit per GamepadAxis
  void Press(const PadInput& input) {
    for (const auto& [axis, value] : input.axes) {
      XDroidGamepadDriver::SetAxis(axis, value);
      axes |= 1 << int(axis);
    }
    XDroidGamepadDriver::SetButton(input.buttons, true);
    buttons |= input.buttons;
  }
  void Release(const PadInput& input) {
    XDroidGamepadDriver::SetButton(input.buttons, false);
    buttons &= ~input.buttons;
    for (const auto& [axis, value] : input.axes) {
      XDroidGamepadDriver::SetAxis(axis, 0.0f);
      axes &= ~(1 << int(axis));
    }
  }
  void Set(uint16_t new_buttons, const float values[6]) {
    XDroidGamepadDriver::SetButton(buttons & ~new_buttons, false);
    XDroidGamepadDriver::SetButton(new_buttons, true);
    buttons = new_buttons;
    for (int i = 0; i < 6; ++i) {
      XDroidGamepadDriver::SetAxis(GamepadAxis(i), values[i]);
    }
    axes = 0x3F;
  }
  void ReleaseAll() {
    XDroidGamepadDriver::SetButton(buttons, false);
    for (int i = 0; i < 6; ++i) {
      if (axes & (1 << i)) {
        XDroidGamepadDriver::SetAxis(GamepadAxis(i), 0.0f);
      }
    }
    buttons = 0;
    axes = 0;
  }
};

// IS-2: `pad = "A+DPAD_DOWN:150"` holds the buttons (and `LX=-1`-style axis
// values) for the given milliseconds, then releases them, on its own thread.
// The host changes another key along with it (a sequence number) so that
// repeating a press still changes the file.
void ApplyPad(const std::string& text) {
  std::string_view spec(text);
  uint32_t hold_ms = 120;
  if (size_t colon = spec.find(':'); colon != std::string_view::npos) {
    hold_ms = uint32_t(
        std::strtoul(std::string(spec.substr(colon + 1)).c_str(), nullptr, 10));
    spec = spec.substr(0, colon);
  }
  PadInput input;
  if (std::string_view unknown = ParsePadSpec(spec, input); !unknown.empty()) {
    XELOGW("XDroid360: live pad: unknown input '{}'", unknown);
  }
  XELOGI("XDroid360: live pad {} for {} ms", text, hold_ms);
  std::thread([input = std::move(input), hold_ms] {
    ScriptPad pad;
    pad.Press(input);
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    pad.Release(input);
  }).detach();
}

enum class Op : uint8_t {
  kSet,
  kPress,
  kHold,
  kRelease,
  kReleaseAll,
  kIdle,
  kWaitTitle,
  kWaitPassesAtLeast,
  kWaitPassesBelow,
  kWaitFlight,
  kLoop,
  kEndLoop,
  kMark,
  kEnd,
};

struct Command {
  Op op = Op::kEnd;
  int line = 0;
  std::string text;
  PadInput input;
  float values[6] = {};
  // Ticks (kSet, kPress, kIdle), ms (kPress with `ms`), title id, passes x100,
  // loop count (kLoop) or the index of the matching kLoop (kEndLoop).
  uint64_t n = 0;
  bool ms = false;
  uint32_t timeout_s = 120;
};

bool ParseUint(std::string_view text, uint64_t& out, int base = 10) {
  const std::string copy(text);
  char* end = nullptr;
  out = std::strtoull(copy.c_str(), &end, base);
  return !copy.empty() && copy[0] != '-' && *end == '\0';
}

bool ParseFloat(std::string_view text, float& out) {
  const std::string copy(text);
  char* end = nullptr;
  out = std::strtof(copy.c_str(), &end);
  return !copy.empty() && *end == '\0';
}

bool ParseSpec(std::string_view spec, PadInput& out) {
  return ParsePadSpec(spec, out).empty() && (out.buttons || !out.axes.empty());
}

bool ParseCommand(const std::vector<std::string_view>& t, Command& c) {
  const std::string_view op = t[0];
  const size_t n = t.size();
  if (op == "S") {
    uint64_t buttons;
    if (n != 9 || !ParseUint(t[1], c.n) || !ParseUint(t[2], buttons, 16)) {
      return false;
    }
    c.op = Op::kSet;
    c.input.buttons = uint16_t(buttons);
    for (int i = 0; i < 6; ++i) {
      if (!ParseFloat(t[3 + i], c.values[i])) {
        return false;
      }
    }
    return true;
  }
  if (op == "release" && n == 2 && t[1] == "all") {
    c.op = Op::kReleaseAll;
    return true;
  }
  if (op == "hold" || op == "release") {
    c.op = op == "hold" ? Op::kHold : Op::kRelease;
    return n == 2 && ParseSpec(t[1], c.input);
  }
  if (op == "idle" || op == "loop") {
    c.op = op == "idle" ? Op::kIdle : Op::kLoop;
    return n == 2 && ParseUint(t[1], c.n) && (c.op == Op::kIdle || c.n);
  }
  if (op == "endloop" || op == "end") {
    c.op = op == "end" ? Op::kEnd : Op::kEndLoop;
    return n == 1;
  }
  if (op == "mark") {
    c.op = Op::kMark;
    return n == 2;
  }
  if (op == "wait") {
    if (n < 2) {
      return false;
    }
    size_t timeout_at = 2;
    constexpr std::string_view kAtLeast = "passes>=", kBelow = "passes<";
    if (t[1] == "title") {
      c.op = Op::kWaitTitle;
      timeout_at = 3;
      if (n < 3 || !ParseUint(t[2], c.n, 16)) {
        return false;
      }
    } else if (t[1] == "flight") {
      c.op = Op::kWaitFlight;
    } else {
      const bool at_least = t[1].substr(0, kAtLeast.size()) == kAtLeast;
      const std::string_view prefix = at_least ? kAtLeast : kBelow;
      if (t[1].substr(0, prefix.size()) != prefix ||
          !ParseUint(t[1].substr(prefix.size()), c.n)) {
        return false;
      }
      c.op = at_least ? Op::kWaitPassesAtLeast : Op::kWaitPassesBelow;
      c.n *= 100;
    }
    uint64_t timeout = 120;
    if (n > timeout_at + 1 ||
        (n == timeout_at + 1 && !ParseUint(t[timeout_at], timeout))) {
      return false;
    }
    c.timeout_s = uint32_t(timeout);
    c.text = t[1];  // what= in the wait-done and abort lines
    return true;
  }
  // SPEC[:N] or SPEC[:Nms].
  if (n != 1) {
    return false;
  }
  c.op = Op::kPress;
  c.n = 4;
  std::string_view spec = op;
  if (size_t colon = spec.find(':'); colon != std::string_view::npos) {
    std::string_view hold = spec.substr(colon + 1);
    spec = spec.substr(0, colon);
    if (hold.size() > 2 && hold.substr(hold.size() - 2) == "ms") {
      c.ms = true;
      hold.remove_suffix(2);
    }
    if (!ParseUint(hold, c.n)) {
      return false;
    }
  }
  return ParseSpec(spec, c.input);
}

// False with |bad| set to the failing line (0: the file did not open).
bool LoadScript(const std::filesystem::path& path,
                std::vector<Command>& commands, Command& bad) {
  std::ifstream file(path);
  if (!file) {
    bad.text = "open";
    return false;
  }
  std::vector<size_t> loops;
  std::string raw;
  for (int line = 1; std::getline(file, raw); ++line) {
    std::string_view text(raw);
    if (line == 1 && text.substr(0, 3) == "\xEF\xBB\xBF") {
      text.remove_prefix(3);  // UTF-8 BOM from a Windows editor
    }
    text = text.substr(0, text.find('#'));
    std::vector<std::string_view> tokens;
    while (!text.empty()) {
      const size_t start = text.find_first_not_of(" \t\r");
      if (start == std::string_view::npos) {
        break;
      }
      text.remove_prefix(start);
      const size_t end = std::min(text.find_first_of(" \t\r"), text.size());
      tokens.push_back(text.substr(0, end));
      text.remove_prefix(end);
    }
    if (tokens.empty()) {
      continue;
    }
    Command c;
    c.line = line;
    c.text = std::string(tokens[0].data(),
                         tokens.back().data() + tokens.back().size());
    bool ok = ParseCommand(tokens, c);
    if (ok && c.op == Op::kLoop) {
      ok = loops.size() < 4;
      loops.push_back(commands.size());
    } else if (ok && c.op == Op::kEndLoop) {
      ok = !loops.empty();
      if (ok) {
        c.n = loops.back();
        loops.pop_back();
      }
    }
    if (!ok) {
      bad = std::move(c);
      return false;
    }
    commands.push_back(std::move(c));
  }
  if (!loops.empty()) {
    bad = commands[loops.back()];
    return false;
  }
  return true;
}

// Start and stop only, from the live-config poll thread and teardown; never
// taken on the input path or inside a watchdog region.
std::mutex g_control;
std::thread g_player;
std::thread g_recorder;
std::string g_last_spec;
std::atomic<bool> g_player_stop{false};
std::atomic<bool> g_recorder_stop{false};
// Teardown clears instance_ before PadScriptShutdown, so no thread starts
// between it and the next instance. Boot (no instance yet) may start one.
bool g_shut_down = false;

bool Refused() {
  if (XDroidApp::Get()) {
    g_shut_down = false;
  }
  return g_shut_down;
}

void StopThread(std::thread& thread, std::atomic<bool>& stop) {
  if (thread.joinable()) {
    stop.store(true, std::memory_order_relaxed);
    thread.join();
  }
}

void RunScript(std::vector<Command> commands) {
  Clock clock{"play"};
  ScriptPad pad;
  constexpr auto kForever = std::chrono::hours(24);
  // Polls |done| every 4 ms; returns why it gave up, or null once done.
  auto wait = [](auto&& done, SteadyClock::duration timeout) -> const char* {
    const auto end = SteadyClock::now() + timeout;
    while (!done()) {
      if (g_player_stop.load(std::memory_order_relaxed)) {
        return "stopped";
      }
      if (SteadyClock::now() >= end) {
        return "timeout";
      }
      Nap();
    }
    return nullptr;
  };
  // Tick of the previous step: step durations add up without drift.
  uint64_t base = 0;
  auto until_tick = [&](uint64_t ticks) {
    base += ticks;
    return wait([&] { return clock.Now() >= base; }, kForever);
  };
  std::vector<uint64_t> loops;
  const char* reason = nullptr;
  size_t steps = 0;
  size_t pc = 0;
  for (; pc < commands.size(); ++pc) {
    const Command& c = commands[pc];
    // Zero-tick commands never reach wait(): check stop here too.
    if (g_player_stop.load(std::memory_order_relaxed)) {
      reason = "stopped";
      break;
    }
    ++steps;
    switch (c.op) {
      case Op::kSet:
        if (!(reason = until_tick(c.n))) {
          pad.Set(c.input.buttons, c.values);
        }
        break;
      case Op::kIdle:
        reason = until_tick(c.n);
        break;
      case Op::kPress:
        pad.Press(c.input);
        if (c.ms) {
          const auto end = SteadyClock::now() + std::chrono::milliseconds(c.n);
          reason = wait([&] { return SteadyClock::now() >= end; }, kForever);
          base = clock.Now();
        } else {
          base = std::max(base, clock.Now());  // held N ticks when behind
          reason = until_tick(c.n);
        }
        pad.Release(c.input);
        break;
      case Op::kHold:
        pad.Press(c.input);
        break;
      case Op::kRelease:
        pad.Release(c.input);
        break;
      case Op::kReleaseAll:
        pad.ReleaseAll();
        break;
      case Op::kWaitTitle:
      case Op::kWaitPassesAtLeast:
      case Op::kWaitPassesBelow:
      case Op::kWaitFlight: {
        const auto start = SteadyClock::now();
        // Window conditions need a window newer than the wait.
        uint64_t stats = FrameStats();
        int flight_windows = 0;
        reason = wait(
            [&] {
              if (c.op == Op::kWaitTitle) {
                return TitleId() == c.n;
              }
              const uint64_t now = FrameStats();
              if (now == stats) {
                return false;
              }
              stats = now;
              const uint32_t passes = uint32_t(now);
              if (c.op == Op::kWaitPassesAtLeast) {
                return passes >= c.n;
              }
              if (c.op == Op::kWaitPassesBelow) {
                return passes < c.n;
              }
              flight_windows = passes >= 6000 ? flight_windows + 1 : 0;
              return flight_windows >= 2;
            },
            std::chrono::seconds(c.timeout_s));
        if (!reason) {
          // `wait title` keeps tick 0 at launch, as the recording does.
          const uint64_t now = clock.Now();
          if (c.op != Op::kWaitTitle) {
            base = now;
          }
          XELOGI(
              "XDROID-PAD wait-done line={} what={} tick={} frame={} "
              "waited_ms={}",
              c.line, c.text, now, xe::logging::GetFrameNumber(),
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  SteadyClock::now() - start)
                  .count());
        }
        break;
      }
      case Op::kLoop:
        loops.push_back(c.n);
        break;
      case Op::kEndLoop:
        if (--loops.back()) {
          pc = c.n;  // the loop line; the body follows
        } else {
          loops.pop_back();
        }
        break;
      case Op::kMark:
        XELOGI("XDROID-PAD mark {} tick={} frame={}",
               c.text.substr(c.text.find_last_of(" \t") + 1), clock.Now(),
               xe::logging::GetFrameNumber());
        break;
      case Op::kEnd:
        pc = commands.size() - 1;
        break;
    }
    if (reason) {
      break;
    }
  }
  pad.ReleaseAll();
  if (reason) {
    XELOGW("XDROID-PAD abort line={} reason={} what={}", commands[pc].line,
           reason, commands[pc].text);
  } else {
    XELOGI("XDROID-PAD done steps={} ticks={} frame={}", steps, clock.Now(),
           xe::logging::GetFrameNumber());
  }
}

void RunRecorder(std::filesystem::path dir) {
  uint32_t title;
  while (!(title = TitleId())) {
    if (g_recorder_stop.load(std::memory_order_relaxed)) {
      XELOGW("XDROID-PAD rec stopped before the title ran");
      return;
    }
    Nap();
  }
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  const std::filesystem::path path =
      dir / fmt::format("rec-{}.pad", int64_t(std::time(nullptr)));
  FILE* file = std::fopen(path.string().c_str(), "w");
  if (!file) {
    XELOGE("XDROID-PAD rec failed path={}", path.string());
    return;
  }
  XELOGI("XDROID-PAD rec start path={}", path.string());
  std::fprintf(file, "# XDroid360 pad recording: title %08X, build %s\n", title,
               BuildId());
  std::fprintf(file, "wait title %08X 180\n", title);
  size_t lines = 2;
  Clock clock{"rec"};
  uint64_t last_tick = 0;
  uint16_t last_buttons = 0;
  long last_cents[6] = {};
  uint64_t stats = FrameStats();
  uint64_t window_ticks[2] = {};  // ticks of the last two windows seen
  int regime = -1;
  int high_windows = 0;
  auto flush_due = SteadyClock::now() + std::chrono::seconds(1);
  while (!g_recorder_stop.load(std::memory_order_relaxed)) {
    Nap();
    uint16_t buttons;
    float axes[6];
    XDroidGamepadDriver::Snapshot(buttons, axes);
    long cents[6];
    bool changed = buttons != last_buttons;
    for (int i = 0; i < 6; ++i) {
      cents[i] = std::lround(axes[i] * 100.0f);
      changed |= std::labs(cents[i] - last_cents[i]) >= 2;
    }
    if (changed) {
      // last_tick may lead now by a tick after a same-tick release.
      const uint64_t now = clock.Now();
      uint64_t d = now - std::min(now, last_tick);
      if (!d && (last_buttons & ~buttons)) {
        d = 1;  // a release in the press tick would replay microseconds later
      }
      std::fprintf(file, "S %llu %04X", (unsigned long long)d, buttons);
      for (int i = 0; i < 6; ++i) {
        std::fprintf(file, " %.2f", cents[i] / 100.0);
        last_cents[i] = cents[i];
      }
      std::fputc('\n', file);
      ++lines;
      last_tick += d;
      last_buttons = buttons;
    }
    // Regime changes (20 and 60 passes/frame) help placing anchors by hand.
    if (const uint64_t now_stats = FrameStats(); now_stats != stats) {
      stats = now_stats;
      const uint32_t passes = uint32_t(stats);
      const int next = passes < 2000 ? 0 : passes < 6000 ? 1 : 2;
      high_windows = next == 2 ? high_windows + 1 : 0;
      const uint64_t now = clock.Now();
      // >= 60 counts on the second window, where `wait flight` returns. A wait
      // needs windows newer than its start, so the idle before the comment
      // ends after the window before (two before for >= 60): an anchor that
      // replaces the idle after the comment keeps the timing.
      if (next != regime && (next != 2 || high_windows >= 2)) {
        regime = next;
        const uint64_t start = std::max(last_tick, window_ticks[next == 2]);
        std::fprintf(file, "idle %llu\n# passes %u tick %llu\nidle %llu\n",
                     (unsigned long long)(start - last_tick), passes / 100,
                     (unsigned long long)now,
                     (unsigned long long)(now - std::min(now, start)));
        lines += 3;
        last_tick = std::max(start, now);
      }
      window_ticks[1] = window_ticks[0];
      window_ticks[0] = now;
    }
    if (SteadyClock::now() >= flush_due) {
      flush_due += std::chrono::seconds(1);
      std::fflush(file);
    }
  }
  std::fclose(file);
  XELOGI("XDROID-PAD rec saved path={} lines={} ticks={}", path.string(), lines,
         clock.Now());
}

// |spec| is a path relative to |storage_root| (`pad/NAME.pad`) or `stop`.
// Starting a script stops the running one; the spec started last is ignored
// until `stop`, so rewriting live.toml with the same key does not restart the
// flight.
void PadScriptStart(const std::filesystem::path& storage_root,
                    std::string_view spec) {
  std::lock_guard<std::mutex> lock(g_control);
  if (spec == g_last_spec || Refused()) {
    return;
  }
  g_last_spec = spec;
  StopThread(g_player, g_player_stop);
  if (spec == "stop") {
    return;
  }
  std::vector<Command> commands;
  Command bad;
  if (!LoadScript(storage_root / std::string(spec), commands, bad)) {
    XELOGW("XDROID-PAD abort line={} reason=parse what={} file={}", bad.line,
           bad.text, spec);
    return;
  }
  XELOGI("XDROID-PAD start file={} lines={}", spec, commands.size());
  g_player_stop.store(false, std::memory_order_relaxed);
  g_player = std::thread(RunScript, std::move(commands));
}

// Idempotent. Writes <storage_root>/pad/rec-<epoch>.pad.
void PadRecordSet(const std::filesystem::path& storage_root, bool on) {
  std::lock_guard<std::mutex> lock(g_control);
  if (!on) {
    StopThread(g_recorder, g_recorder_stop);
    return;
  }
  if (g_recorder.joinable() || Refused()) {
    return;
  }
  g_recorder_stop.store(false, std::memory_order_relaxed);
  g_recorder = std::thread(RunRecorder, storage_root / "pad");
}

}  // namespace

bool ApplyPadKey(const std::filesystem::path& storage_root,
                 std::string_view name, const std::string& value) {
  if (name == "pad") {
    ApplyPad(value);
  } else if (name == "pad_script") {
    PadScriptStart(storage_root, value);
  } else if (name == "pad_record") {
    PadRecordSet(storage_root, value == "1" || value == "true");
  } else {
    return false;
  }
  return true;
}

void PadScriptShutdown() {
  std::lock_guard<std::mutex> lock(g_control);
  StopThread(g_player, g_player_stop);
  StopThread(g_recorder, g_recorder_stop);
  g_last_spec.clear();
  g_shut_down = true;
}

}  // namespace xdroid
