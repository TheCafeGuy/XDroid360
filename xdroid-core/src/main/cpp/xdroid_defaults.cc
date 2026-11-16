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

// Bump when the set changes, and tag each changed key with the new version:
// configs below it get those keys once.
constexpr int32_t kDefaultsVersion = 24;

// Each key is tagged with the defaults version that last changed it. An
// upgrade from `stored` sets only the keys changed after it, so the user's
// other values stay; a fresh config (stored 0) gets every key.
// v1 (2026-10-14): the config of the validated test app (Ace Combat 6 on
// OnePlus 12, SD8 Gen 3 + Turnip, HUD-1 runs of 2026-10-13). A fresh install
// got the upstream values instead and showed pixelated brown/black clouds.
toml::table TestedDefaults(int32_t stored) {
  toml::table values;
  auto set = [&](int32_t version, const char* key, auto value) {
    if (version > stored) {
      values.insert_or_assign(key, value);
    }
  };
  // Resolves straight into the destination texture (no EDRAM round
  // trip): render-to-texture content such as AC6's clouds.
  set(1, "vulkan_direct_resolve_transient_dest", true);
  // Exact occlusion query results (sun, flares, effect visibility).
  set(1, "occlusion_query", "strict");
  set(1, "use_dedicated_xma_thread", true);
  set(1, "wfe_precise_sleep", true);
  set(1, "license_mask", -1);
  // v2 (2026-10-14): STAB-S1 fix; configs saved before it hold false.
  set(2, "vulkan_direct_resolve_post_barrier", true);
  // v3 (2026-10-15): GE-4 pass splice for texture loads and uploads.
  // GPU-BENCH flight trace: -2.3 ms GPU per frame, identical frames.
  set(3, "xdroid_pass_splice", 3);
  // v4 (2026-10-16): GE-1 tight render area. GPU-BENCH flight -1.1 ms with
  // identical frames; hangar hexagon clean on the bench and in the game.
  set(4, "vulkan_tight_render_area", true);
  // v5 (2026-10-16): SHD-2. Configs saved while its default was false
  // pinned it off.
  set(5, "xdroid_tfetch_implicit_lod", true);
  // v6 (2026-10-16): the maintainer's play config. Defer to swap fixes the M7
  // first-load cutscene fragment (suspect of MTN-1, still open); the
  // 16 ms textured wait hides the SHD-1c first-variant black bar.
  set(6, "xdroid_cp_defer_to_swap", true);
  set(6, "xdroid_async_textured_wait_ms", 16);
  // v7 (2026-10-16): TILE-1 merged predicated tiles (bench flight -4 ms,
  // in-mission 300/300 frames merged, audio clean) and UPS-1 swapchain at
  // 50% (paint 1.24 -> 0.35 ms, the display upscales).
  set(7, "xdroid_tile_merge", true);
  set(7, "present_resolution_scale_percent", 50);
  // v8 (2026-10-17): RES-3 direct resolve for MSAA-averaging 8888 copies.
  // GPU-BENCH flight -0.45 ms, flight and cutscene frames bit-identical.
  // (v19 turns it off again.)
  // v9 (2026-10-17): TEX-1a skips untiling CPU reloads whose bytes match
  // the texture already. GPU-BENCH flight -0.4 ms, cutscene -0.16 ms,
  // frames bit-identical.
  set(9, "xdroid_tex_skip_same", true);
  // v11 (2026-10-17; 10 was the withdrawn guest_spin_park=4): TEX-1b
  // resolve-to-texture link and RES-4 direct 1x reads of multisampled
  // tiles. GPU-BENCH flight 23.35 -> 22.10 ms, flight, cutscene and
  // hangar frames bit-identical.
  set(11, "xdroid_tex_link", true);
  set(11, "xdroid_direct_resolve_read_as_1x", true);
  // v12 (2026-10-18): A2 cheaper 21-bit transcendental rounding and A5
  // coalesced 32bpb untiling (ideas: Xendroid+). GPU-BENCH flight,
  // plugged in, 2 rounds: 21.95 -> 21.23 ms; flight and cutscene frames
  // bit-identical.
  set(12, "xdroid_fast_precision_round", true);
  set(12, "xdroid_texload_coalesced", true);
  // v13 (2026-10-18): A8 quad lists as CPU-built triangle lists instead
  // of the quad geometry shader (~380 quad draws per flight frame).
  // GPU-BENCH flight 21.95 -> 21.29 ms; not bit-exact (diagonal split,
  // max 41/255 on a few pixels), no visible difference (the maintainer).
  set(13, "force_convert_quad_lists_to_triangle_lists", true);
  // v14 (2026-10-18): A9 simple 4x MSAA draws drawn into the 1x surface
  // of their samples (idea: Xendroid+). Flight, unplugged: transfers 84
  // -> 35 a frame, 21.88 -> 21.09 ms; cutscene 2.97 -> 1.89 ms and
  // bit-identical. Flight differs on <0.8% of pixels by <= 8/255, no
  // visible difference (the maintainer).
  set(14, "xdroid_samples_as_pixels", true);
  // v15 (2026-10-18): relaxed multiplications (no SM3 "0 * x = 0"
  // emulation) in vertex and pixel shaders (idea: Xendroid+). Flight
  // 20.99 -> 19.79 ms, cutscene 1.90 -> 1.78 ms, both bit-identical.
  set(15, "xdroid_relaxed_mul", 3);
  // v16 (2026-10-19): SHD-2 integer-scale specialization of texture
  // fetches. Flight 19.76 -> 19.38 ms, main pixel shader 974 -> 882 ir3
  // instructions, frames identical.
  set(16, "xdroid_shader_iscale_spec", true);
  // v17 (2026-10-19): SHD-2 S1 structured forward jumps instead of the
  // PC switch (guarded form where the jumps nest). Flight 19.34 -> 18.97
  // ms, cutscene 1.75 ms both, frames identical.
  set(17, "xdroid_structured_jumps", true);
  // v18 (2026-10-22): GSR2-1 Snapdragon GSR 2 temporal upscale 720p ->
  // 1080p at swap, HUD mask. Flight bench -0.11 ms (CI -0.98..+0.76),
  // no ghosting, HUD crisp (bench frames).
  // v19 (2026-10-22): back off. In flight the HUD blends with the world
  // history (ghosted translucent panels, doubled moving tags, sky streaks);
  // zero glitches first, GSR2 stays a Settings opt-in.
  set(19, "gsr2", false);
  // v19 (2026-10-22): RES-3 off. Every R5302-family GPU hang in play
  // (3 on 2026-10-19, 1 on 2026-10-22 with the link already off) was an
  // averaged direct resolve, kgsl page fault at address 0 (UCHE BR_SP),
  // then a preemption fault and a SystemUI death; bench repro noavg 0/4.
  // Costs +0.45 ms flight until the null read is found.
  set(19, "xdroid_direct_resolve_average", false);
  // v20 (2026-11-11): G5-3 signals wake multi-waiters after unlocking and a
  // contended multi-wait sleeps on the lock (the audio worker spun on
  // try_lock and starved the mixer thread it waited for, ~100 ms stalls).
  // Flight g53: FPS 30, 0 underruns, mW within noise.
  set(20, "xdroid_wait_block_contended", 1);
  // v21 (2026-11-11): 16x anisotropic filtering (G4, the maintainer's OK). GPU-BENCH
  // flight +0.20 ms; little to see at altitude (0.04-0.07% of pixels differ
  // by more than 8/255), more on the ground and in the hangar.
  set(21, "anisotropic_override", 5);
  // v22 (2026-11-11): relaxed multiplications only where the non-finite
  // analysis proves both operands finite (bit 4); the Shader Model 3
  // "0 * x = 0" select elsewhere, so no product gives NaN where the console
  // gives 0 (BLACK-H1). GPU-BENCH flight: +0.28 ms vs mode 3 (full SM3:
  // +1.09 ms), frames identical. the maintainer's OK.
  set(22, "xdroid_relaxed_mul", 7);
  // v23 (2026-11-11): RES-3 back for 2x MSAA sources only (the main scene
  // resolve, dump key 0B01): tens of thousands of runs per session with no
  // fault, every R5302 fault was the four-sample average, which stays off.
  // GPU-BENCH flight: -0.18 ms (CI -0.26..-0.09), frames identical.
  set(23, "xdroid_direct_resolve_average_2x", true);
  // v24 (2026-11-16): the config behind release 0.3.0 for every install.
  // STAB-S1 / R5302: a full barrier before every color dump (bit 8) and
  // before every direct resolve. The hangs of 2026-11-14 and 2026-11-15 were
  // the two render target reads left without that drain; flights on 13 / 2
  // since then: no hang, no device loss, 30 FPS at 23.1-23.3 ms GPU.
  set(24, "vulkan_dump_barrier", 13);
  set(24, "vulkan_direct_resolve_pre_barrier", 2);
  // Keys whose default changed after a build had already saved the old one,
  // so that config kept it: LAT-3 (577cb7753; a 3000 us margin cost FPS),
  // TEX-1c (e1db94174) and the MIRROR-1 encoder input (90d63fea8).
  set(24, "xdroid_jit_frame", 2);
  set(24, "xdroid_jit_frame_margin_us", 6000);
  set(24, "xdroid_tex_link_mips", 5);
  set(24, "xdroid_mirror_input", 1);
  set(24, "xdroid_mirror_keys", "vendor.qti-ext-enc-low-latency.enable=1");
  return values;
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
      {"xdroid_tex_census", false},
      {"xdroid_xfer_census", false},
      {"xdroid_shader_ir_dump", ""},
      {"vulkan_gpu_breadcrumbs", false},
      {"xdroid_draw_blackbox", false},
      {"xdroid_pass_profiler", 0},
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
  const int32_t stored = cvars::xdroid_defaults_version;
  if (stored >= kDefaultsVersion || !cvar::ConfigVars) {
    return;
  }
  toml::table defaults = TestedDefaults(stored);
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
  XELOGI(
      "XDroid360: tested defaults v{} written to the config, {} keys "
      "from v{}",
      kDefaultsVersion, defaults.size() - 1, stored);
}

}  // namespace xdroid
