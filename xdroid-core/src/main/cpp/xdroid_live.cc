/**
 * XDroid360: live cvar changes for on-device A/B runs (PES-1).
 */

#include "xdroid_live.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "xdroid_audio_driver.h"
#include "xdroid_events.h"
#include "xdroid_latency.h"
#include "xdroid_mirror.h"
#include "xdroid_pad_script.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/xdroid_gpu_hooks.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/memory.h"

DECLARE_bool(xdroid_diagnostics);
DECLARE_path(trace_gpu_prefix);
DEFINE_uint32(xdroid_gpu_trace_frames, 0,
              "GPU-BENCH capture: set (live) to stream about this many frames "
              "into <storage>/traces/; resets to 0 when done.",
              "GPU");

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
    "guest_spin_park_gpu_pct",
    "guest_spin_park_hints",
    "guest_spin_park_timeout_us",
    "guest_spin_wfe",
    "xdroid_stall_inject_every_s",
    "xdroid_stall_inject_ms",
    "xdroid_glitch_recorder",
    "xdroid_gpu_sanity",
    "ac6_resource_miss_log",
    "vulkan_direct_resolve_transient_dest",
    "vulkan_direct_resolve_post_barrier",
    "xdroid_direct_resolve_average",
    "xdroid_direct_resolve_average_2x",
    "xdroid_direct_resolve_read_as_1x",
    "xdroid_tex_link",
    "xdroid_samples_as_pixels",
    "force_convert_quad_lists_to_triangle_lists",
    "vulkan_direct_resolve_pre_barrier",
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
    "xdroid_lat_trace",
    "xdroid_lat_meter",
    "xdroid_jit_frame",
    "xdroid_jit_frame_margin_us",
    "xdroid_jit_frame_max_us",
    "xdroid_vfetch_reupload_max_kb",
    "xdroid_draw_blackbox",
    "xdroid_write_probe_first",
    "xdroid_write_probe_last",
    "ac6_hud_slot_join",
    "ac6_hud_slot_join_us",
    "xdroid_cp_defer_yield_hud",
    "xdroid_job_tracer",
    "apu_queue_bursts",
    "apu_queue_max_frames",
    "xdroid_audio_conceal",
    "xdroid_audio_loudness_guard",
    "xdroid_audio_worker_nice",
    "xdroid_wait_block_contended",
    "xdroid_audio_producer_nice",
    "xdroid_audio_drc_ppm",
    "xdroid_audio_route_watch",
    "xdroid_audio_producer_boost",
    "xdroid_pulse_multi_waiters",
    // Only its per-use readers follow live (AUDIO-H4 audio forensics, event
    // auto marks, clip requests); the boot-time DiagnosticsOn/Off override
    // and the stream-open clip ring stay as set at boot.
    "xdroid_diagnostics",
    "xdroid_adpf",
    "xdroid_adpf_target_pct",
    "xdroid_adpf_power_efficiency",
    "xdroid_adpf_audio",
    "xdroid_adpf_audio_guest_tid",
    "xdroid_adpf_audio_period_us",
    "xdroid_adpf_audio_target_pct",
    "xdroid_adpf_exclude",
    "xdroid_time_coupling",
    "readback_resolve",
    "xdroid_time_coupling_slow_pct",
    "xdroid_time_coupling_due_pct",
    "xdroid_time_coupling_max_ms",
    "ac6_unlock_probe",
    "ac6_slowdown_guard",
    "ac6_min_sim_fps",
    "ac6_dt_snap",
    "xdroid_pass_profiler",
    "xdroid_pass_profiler_top",
    "xdroid_clear_pass_mode",
    "xdroid_pass_splice",
    "vulkan_tight_render_area",
    "gpu_force_msaa_1x",
    "xdroid_tex_probe",
    "xdroid_tex_unloaded_null",
    "xdroid_async_textured_wait_ms",
    "xdroid_async_textured_skip",
    "xdroid_gpu_trace_frames",
    "xdroid_mirror",
    "xdroid_mirror_port",
    "xdroid_mirror_height",
    "xdroid_mirror_kbps",
    "xdroid_mirror_codec",
    "xdroid_mirror_bitrate_mode",
    "xdroid_mirror_intra_refresh",
    "xdroid_mirror_key_interval",
    "xdroid_mirror_input",
    "xdroid_mirror_audio",
};

bool IsLiveCvar(std::string_view name) {
  for (std::string_view live : kLiveCvars) {
    if (live == name) {
      return true;
    }
  }
  return false;
}

// Set once by StartLiveConfig, before any live key is applied.
std::filesystem::path g_storage_root;

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

// Forensic: guest_dump = "calls:ADDR" lists the call sites of a function;
// guest_dump = "ADDR:BYTES" (hex address) logs the title's code as
// big-endian words, to read a hot guest function the sampler names.
void DumpGuestCode(const std::string& spec) {
  unsigned address = 0, bytes = 0;
  auto* kernel_state = xe::kernel::KernelState::shared();
  auto module = kernel_state ? kernel_state->GetExecutableModule() : nullptr;
  const xe::cpu::XexModule* xex = module ? module->xex_module() : nullptr;
  // "calls:ADDR" lists the bl instructions that call ADDR instead.
  unsigned target = 0;
  if (xex && std::sscanf(spec.c_str(), "calls:%x", &target) == 1) {
    const uint8_t* membase = kernel_state->memory()->virtual_membase();
    std::string sites;
    unsigned count = 0;
    for (unsigned site = xex->code_low_address();
         site < xex->code_high_address(); site += 4) {
      if (xe::load_and_swap<uint32_t>(membase + site) ==
          (0x48000001u | ((target - site) & 0x03FFFFFCu))) {
        char word[10];
        std::snprintf(word, sizeof(word), " %08X", site);
        sites += word;
        if (!(++count % 12)) {
          XELOGI("XDROID-GCODE calls {:08X}:{}", target, sites);
          sites.clear();
        }
      }
    }
    XELOGI("XDROID-GCODE calls {:08X}:{} ({} sites)", target, sites, count);
    return;
  }
  if (!xex || std::sscanf(spec.c_str(), "%x:%u", &address, &bytes) != 2 ||
      address < xex->code_low_address() ||
      address >= xex->code_high_address()) {
    XELOGW("XDroid360: guest_dump {}: not in the title's code", spec);
    return;
  }
  address &= ~3u;
  bytes = std::min(bytes, std::min(8192u, xex->code_high_address() - address));
  const uint8_t* membase = kernel_state->memory()->virtual_membase();
  for (unsigned offset = 0; offset < bytes; offset += 32) {
    std::string line;
    for (unsigned i = offset; i < std::min(offset + 32, bytes); i += 4) {
      char word[10];
      std::snprintf(word, sizeof(word), " %08X",
                    xe::load_and_swap<uint32_t>(membase + address + i));
      line += word;
    }
    XELOGI("XDROID-GCODE {:08X}:{}", address + offset, line);
  }
}

void ApplyTable(const toml::table& table) {
  for (auto&& [key, node] : table) {
    if (const toml::table* subtable = node.as_table()) {
      // Sections are allowed, as in the config file.
      ApplyTable(*subtable);
      continue;
    }
    const std::string name(key.str());
    if (ApplyPadKey(g_storage_root, name, NodeText(node))) {
      continue;
    }
    if (name == "xdroid_mirror_token") {  // Secret: never logged.
      MirrorSetToken(NodeText(node));
      continue;
    }
    if (MirrorSetString(name, NodeText(node))) {
      XELOGI("XDroid360: live {} = {}", name, NodeText(node));
      continue;
    }
    if (name == "guest_dump") {
      DumpGuestCode(NodeText(node));
      continue;
    }
    cvar::IConfigVar* config_var = nullptr;
    if (cvar::ConfigVars) {
      auto it = cvar::ConfigVars->find(name);
      if (it != cvar::ConfigVars->end()) {
        config_var = it->second;
      }
    }
    if (config_var && IsLiveCvar(name)) {
      // Plain stores to word-sized cvars that other threads read without
      // synchronization: they see either value, which is all a switch needs.
      // Into the game-config layer, which SaveConfig never writes: a test
      // toggle ends with the session instead of staying in the user's config
      // (an averaging A/B left xdroid_direct_resolve_average on for two days
      // and hung the GPU twice in mission 6, 2026-11-14).
      config_var->LoadGameConfigValue(&node);
      XELOGI("XDroid360: live cvar {} = {}", name, NodeText(node));
    } else if (config_var) {
      // A real cvar outside kLiveCvars only changes after a restart: say so
      // loudly, or an A/B on it compares two identical arms (gsr2-sab).
      XELOGW("XDroid360: live key {} = {} IGNORED: restart-only cvar", name,
             NodeText(node));
    } else {
      XELOGI("XDroid360: live marker {} = {}", name, NodeText(node));
    }
  }
}

// MC-1 mark clip: the app writes clip.req next to live.toml on a user mark.
// The glitch recorder saves the frames (clip_request_frame); this thread
// saves the audio, both about 1 s after the mark so the clip also holds what
// followed it.
void PollClipRequest(const std::filesystem::path& storage_root,
                     uint64_t& pending,
                     std::chrono::steady_clock::time_point& due) {
  std::error_code error;
  const std::filesystem::path request = storage_root / "clip.req";
  // EVT-0: an automatic mark from a detector event, saved like a user mark
  // plus event.txt naming the event.
  static std::string event_text;
  if (std::filesystem::exists(request, error)) {
    std::filesystem::remove(request, error);
    if (!pending && cvars::xdroid_diagnostics) {
      pending = std::max<uint64_t>(xe::logging::GetFrameNumber(), 1);
      event_text.clear();
      xe::gpu::GetXdroidGpuHooks().clip_request_frame.store(
          pending, std::memory_order_release);
      due = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      XELOGI("XDROID-CLIP: mark at frame {}", pending);
    }
  }
  uint64_t auto_frame;
  std::string auto_text;
  if (TakeAutoMark(auto_frame, auto_text)) {
    if (pending) {
      XELOGI("XDROID-CLIP: auto mark at frame {} merged into the pending one",
             auto_frame);
    } else {
      pending = auto_frame;
      event_text = std::move(auto_text);
      xe::gpu::GetXdroidGpuHooks().clip_request_frame.store(
          pending, std::memory_order_release);
      due = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      XELOGI("XDROID-CLIP: auto mark at frame {}: {}", pending, event_text);
    }
  }
  if (!pending || std::chrono::steady_clock::now() < due) {
    return;
  }
  const std::filesystem::path dir =
      storage_root / "clips" / fmt::format("clip-f{:07}", pending);
  std::filesystem::create_directories(dir, error);
  if (!WriteAudioClip(dir / "audio.wav", dir / "audio.csv")) {
    XELOGW("XDROID-CLIP: frame {}: no audio to save", pending);
  }
  if (!event_text.empty()) {
    if (FILE* file = std::fopen((dir / "event.txt").string().c_str(), "w")) {
      std::fprintf(file, "%s\n", event_text.c_str());
      std::fclose(file);
    }
    event_text.clear();
  }
  pending = 0;
}

std::atomic<xe::gpu::GraphicsSystem*> g_graphics_system{nullptr};

// GPU-BENCH capture state machine, polled every 200 ms (so the trace holds a
// few frames more than asked).
void PollTraceCapture(const std::filesystem::path& storage_root) {
  static uint64_t end_frame = 0;
  xe::gpu::GraphicsSystem* graphics_system =
      g_graphics_system.load(std::memory_order_acquire);
  if (!graphics_system) {
    return;
  }
  const uint64_t frame = xe::logging::GetFrameNumber();
  if (end_frame) {
    if (frame >= end_frame) {
      graphics_system->RequestEndTracing();
      XELOGI("XDROID-BENCH: capture ends at frame {}", frame);
      end_frame = 0;
      cvars::xdroid_gpu_trace_frames = 0;
    }
    return;
  }
  if (!cvars::xdroid_gpu_trace_frames) {
    return;
  }
#if XE_ENABLE_TRACE_WRITER_INSTRUMENTATION
  std::error_code error;
  const std::filesystem::path dir = storage_root / "traces";
  std::filesystem::create_directories(dir, error);
  cvars::trace_gpu_prefix = dir;
  graphics_system->BeginTracing();
  end_frame = frame + cvars::xdroid_gpu_trace_frames;
  XELOGI("XDROID-BENCH: capturing frames {}-{} into {}", frame, end_frame,
         dir.string());
#else
  XELOGW(
      "XDROID-BENCH: this build has no trace writer "
      "(XENIA_ENABLE_GPU_TRACE off); capture ignored");
  cvars::xdroid_gpu_trace_frames = 0;
#endif
}

void PollLoop(std::filesystem::path path) {
  std::filesystem::file_time_type last_write{};
  uintmax_t last_size = 0;
  bool seen = false;
  uint64_t clip_pending = 0;
  std::chrono::steady_clock::time_point clip_due;
  auto summary_due = std::chrono::steady_clock::now() + std::chrono::minutes(1);
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    PollClipRequest(path.parent_path(), clip_pending, clip_due);
    PollTraceCapture(path.parent_path());
    if (std::chrono::steady_clock::now() >= summary_due) {
      summary_due += std::chrono::minutes(1);
      LogEventSummary();
    }
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

void ApplyLiveText(const std::string& text) {
  try {
    ApplyTable(toml::parse(text));
  } catch (const toml::parse_error& e) {
    XELOGW("XDroid360: live text not parsed: {}", e.what());
  }
}

void SetLiveGraphicsSystem(xe::gpu::GraphicsSystem* graphics_system) {
  g_graphics_system.store(graphics_system, std::memory_order_release);
}

xe::gpu::GraphicsSystem* GetLiveGraphicsSystem() {
  return g_graphics_system.load(std::memory_order_acquire);
}

void StartLiveConfig(const std::filesystem::path& path) {
  static std::once_flag started;
  std::call_once(started, [&path] {
    g_storage_root = path.parent_path();
    std::error_code error;
    std::filesystem::remove(path, error);
    InstallEventBus();
    InstallLatencyMeter();
    InstallMirror();
    // Detached: it only sleeps and touches cvars, and lives as long as the
    // process.
    std::thread(PollLoop, path).detach();
  });
}

}  // namespace xdroid
