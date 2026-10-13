// XDroid360 content package (DLC, title update, ...) installer.

#ifndef XDROID_CONTENT_H_
#define XDROID_CONTENT_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace xdroid {

// Installs one STFS/SVOD content package into <storage_root>/content, the
// same layout Emulator::InstallContentPackage uses, without a running
// emulator. Returns "OK|<type>|<title id>|<display name>" or "ERR|<reason>".
std::string InstallContentPackage(const std::string& storage_root,
                                  const std::string& package_path);

// Title ID of a game (ISO or loose .xex) read from its XEX execution info
// header, without booting it. 0 if unknown.
uint32_t ProbeTitleId(const std::string& game_path);

// Saves the running game's icon (PNG from its XDBF) as
// <storage_root>/covers/<TITLEID>.png, once.
void SaveTitleIcon(const std::string& storage_root, uint32_t title_id,
                   const uint8_t* png, size_t size);

}  // namespace xdroid

#endif  // XDROID_CONTENT_H_
