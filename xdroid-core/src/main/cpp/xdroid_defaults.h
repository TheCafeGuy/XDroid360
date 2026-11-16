/**
 * XDroid360: tested defaults written into the user's config once.
 */

#ifndef XDROID_DEFAULTS_H_
#define XDROID_DEFAULTS_H_

namespace xdroid {

// After config::SetupConfig: if the config predates the current tested
// defaults, sets the ones changed since its version and saves the file.
// Fresh installs get every value the target device was validated with;
// upgrades keep every other value the user changed.
void ApplyTestedDefaults();

// Forces the diagnostic cvars on or off per xdroid_diagnostics, every boot,
// without writing them to the config.
void ApplyDiagnostics();

}  // namespace xdroid

#endif  // XDROID_DEFAULTS_H_
