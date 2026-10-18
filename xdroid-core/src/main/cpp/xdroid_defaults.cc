#include "xdroid_defaults.h"

#include <cstdint>
#include <string>

#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"

DEFINE_int32(xdroid_defaults_version, 0,
             "XDroid360 tested defaults already written to this file (set by "
             "the app).",
             "XDroid360");

DEFINE_bool(xdroid_diagnostics, true,
            "Forensics and diagnostics (glitch recorder, black box, probes, "
            "detailed logs, forensic zip). Off forces every diagnostic off for "
            "maximum performance; the GPU watchdog and crash reports stay on. "
            "Set by the app's Diagnostics switch.",
            "XDroid360");

namespace xdroid {

namespace {

// Bump when the set changes: every config below it gets the new set once.
constexpr int32_t kDefaultsVersion = 11;

// v1 (2026-10-14): the config of the validated test app (Ace Combat 6 on
// OnePlus 12, SD8 Gen 3 + Turnip, HUD-1 runs of 2026-10-13). A fresh install
// got the upstream values instead and showed pixelated brown/black clouds.
toml::table TestedDefaults() {
  return toml::table{
      // Resolves straight into the destination texture (no EDRAM round
      // trip): render-to-texture content such as AC6's clouds.
      {"vulkan_direct_resolve_transient_dest", true},
      // Exact occlusion query results (sun, flares, effect visibility).
      {"occlusion_query", "strict"},
      {"use_dedicated_xma_thread", true},
      {"wfe_precise_sleep", true},
      {"license_mask", -1},
      // v2 (2026-10-14): STAB-S1 fix; configs saved before it hold false.
      {"vulkan_direct_resolve_post_barrier", true},
      // v3 (2026-10-15): GE-4 pass splice for texture loads and uploads.
      // GPU-BENCH flight trace: -2.3 ms GPU per frame, identical frames.
      {"xdroid_pass_splice", 3},
      // v4 (2026-10-16): GE-1 tight render area. GPU-BENCH flight -1.1 ms with
      // identical frames; hangar hexagon clean on the bench and in the game.
      {"vulkan_tight_render_area", true},
      // v5 (2026-10-16): SHD-2. Configs saved while its default was false
      // pinned it off.
      {"xdroid_tfetch_implicit_lod", true},
      // v6 (2026-10-16): the maintainer's play config. Defer to swap fixes the M7
      // first-load cutscene fragment (suspect of MTN-1, still open); the
      // 16 ms textured wait hides the SHD-1c first-variant black bar.
      {"xdroid_cp_defer_to_swap", true},
      {"xdroid_async_textured_wait_ms", 16},
      // v7 (2026-10-16): TILE-1 merged predicated tiles (bench flight -4 ms,
      // in-mission 300/300 frames merged, audio clean) and UPS-1 swapchain at
      // 50% (paint 1.24 -> 0.35 ms, the display upscales).
      {"xdroid_tile_merge", true},
      {"present_resolution_scale_percent", 50},
      // v8 (2026-10-17): RES-3 direct resolve for MSAA-averaging 8888 copies.
      // GPU-BENCH flight -0.45 ms, flight and cutscene frames bit-identical.
      {"xdroid_direct_resolve_average", true},
      // v9 (2026-10-17): TEX-1a skips untiling CPU reloads whose bytes match
      // the texture already. GPU-BENCH flight -0.4 ms, cutscene -0.16 ms,
      // frames bit-identical.
      {"xdroid_tex_skip_same", true},
      // v11 (2026-10-17; 10 was the withdrawn guest_spin_park=4): TEX-1b
      // resolve-to-texture link and RES-4 direct 1x reads of multisampled
      // tiles. GPU-BENCH flight 23.35 -> 22.10 ms, flight, cutscene and
      // hangar frames bit-identical.
      {"xdroid_tex_link", true},
      {"xdroid_direct_resolve_read_as_1x", true},
  };
}

// Diagnostics that cost CPU, GPU or log I/O. Off forces all of them off;
// on turns on the cheap always-useful ones and leaves the costly experiment
// switches (breadcrumbs, draw black box, pass profiler, IB hashing) to the
// config. Not diagnostics: the GPU stall watchdog and the session crash
// reports (the crash net) and every behaviour-changing test arm.
toml::table DiagnosticsOff() {
  return toml::table{
      {"xdroid_glitch_recorder", false},
      {"ac6_resource_miss_log", false},
      {"xdroid_job_tracer", false},
      {"vulkan_log_dispatch_groups", 0},
      {"xdroid_shader_stall_log", false},
      {"ac6_unlock_probe", false},
      {"xdroid_tex_probe", false},
      {"vulkan_gpu_breadcrumbs", false},
      {"xdroid_draw_blackbox", false},
      {"xdroid_pass_profiler", 0},
      {"xdroid_cp_ib_kick_hash", false},
      {"xdroid_gpu_sanity", false},
      {"xdroid_auto_mark", false},
  };
}

toml::table DiagnosticsOn() {
  return toml::table{
      {"xdroid_glitch_recorder", true},
  };
}

// Applied as game-config overrides: they win over the global file but
// SaveConfig never writes them, so the user's config keeps its values.
void Override(const toml::table& values) {
  for (auto&& [key, node] : values) {
    std::string name(key.str());
    auto it = cvar::ConfigVars->find(name);
    if (it == cvar::ConfigVars->end()) {
      XELOGW("XDroid360: diagnostics cvar {} has no cvar", name);
      continue;
    }
    it->second->LoadGameConfigValue(&node);
  }
}

}  // namespace

void ApplyDiagnostics() {
  if (!cvar::ConfigVars) {
    return;
  }
  Override(cvars::xdroid_diagnostics ? DiagnosticsOn() : DiagnosticsOff());
  XELOGI("XDroid360: diagnostics {}", cvars::xdroid_diagnostics ? "on" : "off");
}

void ApplyTestedDefaults() {
  if (cvars::xdroid_defaults_version >= kDefaultsVersion ||
      !cvar::ConfigVars) {
    return;
  }
  toml::table defaults = TestedDefaults();
  defaults.insert_or_assign("xdroid_defaults_version", kDefaultsVersion);
  for (auto&& [key, node] : defaults) {
    std::string name(key.str());
    auto it = cvar::ConfigVars->find(name);
    if (it == cvar::ConfigVars->end()) {
      XELOGW("XDroid360: tested default {} has no cvar", name);
      continue;
    }
    it->second->LoadConfigValue(&node);
  }
  config::SaveConfig();
  XELOGI("XDroid360: tested defaults v{} written to the config",
         kDefaultsVersion);
}

}  // namespace xdroid
