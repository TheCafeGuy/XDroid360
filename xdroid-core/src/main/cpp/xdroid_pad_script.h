/**
 * XDroid360: virtual pad for unattended runs: the live `pad` key (IS-2) and
 * the pad script player and recorder (AUTO-1,
 * docs/design/auto1-autonomous-bench.md section 1).
 */

#ifndef XDROID_PAD_SCRIPT_H_
#define XDROID_PAD_SCRIPT_H_

#include <filesystem>
#include <string>
#include <string_view>

namespace xdroid {

// Live-config poll thread: applies the `pad`, `pad_script` or `pad_record`
// key; false for any other key. Scripts live under |storage_root|.
bool ApplyPadKey(const std::filesystem::path& storage_root,
                 std::string_view name, const std::string& value);
// Emulator teardown: stops and joins both threads, releases the script input.
void PadScriptShutdown();

}  // namespace xdroid

#endif  // XDROID_PAD_SCRIPT_H_
