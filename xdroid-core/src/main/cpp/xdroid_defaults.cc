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

namespace xdroid {

namespace {

// Bump when the set changes: every config below it gets the new set once.
constexpr int32_t kDefaultsVersion = 1;

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
  };
}

}  // namespace

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
