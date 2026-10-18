/**
 * XDroid360: tested defaults written into the user's config once.
 */

#ifndef XDROID_DEFAULTS_H_
#define XDROID_DEFAULTS_H_

namespace xdroid {

// After config::SetupConfig: if the config predates the current tested
// defaults, sets them and saves the file. Fresh installs and upgrades get
// the values the target device was validated with; a change the user makes
// afterwards sticks.
void ApplyTestedDefaults();

// Forces the diagnostic cvars on or off per xdroid_diagnostics, every boot,
// without writing them to the config.
void ApplyDiagnostics();

}  // namespace xdroid

#endif  // XDROID_DEFAULTS_H_
