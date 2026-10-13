// XDroid360 content package installer. See xdroid_content.h.

#include "xdroid_content.h"

#include <atomic>
#include <cstdio>
#include <vector>
#include <filesystem>
#include <mutex>
#include <system_error>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/xbox.h"

namespace xdroid {

std::string InstallContentPackage(const std::string& storage_root,
                                  const std::string& package_path) {
  // One install at a time: they share the staging folder.
  static std::mutex install_mutex;
  std::lock_guard<std::mutex> install_lock(install_mutex);

  const std::filesystem::path path = xe::to_path(package_path);
  const std::filesystem::path content_root =
      xe::to_path(storage_root) / "content";

  // Same header checks and install layout as
  // Emulator::ProcessContentPackageHeader / InstallContentPackage, minus the
  // signed-in profile (only saved games are re-targeted to it there).
  const auto header = xe::vfs::XContentContainerDevice::ReadContainerHeader(path);
  if (!header || !header->content_header.is_magic_valid()) {
    return "ERR|not an Xbox 360 content package (STFS/SVOD)";
  }
  const uint64_t xuid = header->content_metadata.profile_id;
  const uint32_t title_id =
      header->content_metadata.execution_info.title_id.get();
  const uint32_t content_type =
      static_cast<uint32_t>(header->content_metadata.content_type.get());
  const std::string file_name = xe::path_to_utf8(path.filename());
  const std::filesystem::path data_path =
      content_root / fmt::format("{:016X}/{:08X}/{:08X}/{}", xuid, title_id,
                                 content_type, file_name);
  const std::filesystem::path header_path =
      content_root / fmt::format("{:016X}/{:08X}/Headers/{:08X}/{}", xuid,
                                 title_id, content_type, file_name);
  std::string name = xe::to_utf8(
      header->content_metadata.display_name(xe::XLanguage::kEnglish));
  if (name.empty()) {
    name = file_name;
  }
  std::string type_name = fmt::format("{:08X}", content_type);
  auto type_it =
      xe::XContentTypeMap.find(static_cast<xe::XContentType>(content_type));
  if (type_it != xe::XContentTypeMap.end()) {
    type_name = type_it->second;
  }

  std::error_code ec;
  std::filesystem::create_directories(content_root, ec);
  if (ec) {
    return "ERR|cannot create " + xe::path_to_utf8(content_root) + ": " +
           ec.message();
  }
  // Staging and a previous one cut short (the app was killed).
  const std::filesystem::path staging = content_root / ".staging";
  const std::filesystem::path old_copy = content_root / ".old";
  std::filesystem::remove_all(staging, ec);
  std::filesystem::remove_all(old_copy, ec);
  const bool reinstall = std::filesystem::exists(data_path, ec);
  const auto space = std::filesystem::space(content_root, ec);
  if (!ec && space.available <
                 uint64_t(header->content_metadata.content_size * 1.1)) {
    return reinstall ? "ERR|not enough free space (a reinstall needs room "
                       "for both copies until it finishes)"
                     : "ERR|not enough free space";
  }

  auto device = xe::vfs::XContentContainerDevice::CreateContentDevice("", path);
  if (!device || !device->Initialize()) {
    return "ERR|cannot open the package";
  }

  // Extract into a staging folder at the content root, which the guest never
  // lists (it only looks under <xuid>/<title>/<type>) and the app skips, and
  // swap it in only on success: a failed install or re-install leaves the
  // installed copy, if any, untouched. The replaced copy waits in ".old",
  // also at the root, until the swap is done.
  std::filesystem::create_directories(staging, ec);
  if (ec) {
    return "ERR|cannot create " + xe::path_to_utf8(staging) + ": " +
           ec.message();
  }
  std::atomic<uint64_t> progress{0};
  const xe::X_STATUS status = xe::vfs::VirtualFileSystem::ExtractContentFiles(
      device.get(), staging, progress);
  if (status != xe::X_STATUS(0)) {
    std::filesystem::remove_all(staging, ec);
    return fmt::format("ERR|extraction failed (0x{:08X})", status);
  }

  std::filesystem::create_directories(data_path.parent_path(), ec);
  if (reinstall) {
    std::filesystem::rename(data_path, old_copy, ec);
    if (ec) {
      std::filesystem::remove_all(staging, ec);
      return "ERR|cannot replace the installed copy: " + ec.message();
    }
  }
  std::filesystem::rename(staging, data_path, ec);
  if (ec) {
    const std::string error = ec.message();
    if (reinstall) {
      std::filesystem::rename(old_copy, data_path, ec);
    }
    std::filesystem::remove_all(staging, ec);
    return "ERR|cannot move the package into place: " + error;
  }
  std::filesystem::remove_all(old_copy, ec);
  xe::vfs::VirtualFileSystem::ExtractContentHeader(device.get(), header_path);
  XELOGI("XDroid360: installed content '{}' ({}) for title {:08X} from {}",
         name, type_name, title_id, package_path);
  return fmt::format("OK|{}|{:08X}|{}", type_name, title_id, name);
}

namespace {

uint32_t ReadBe32(const std::vector<uint8_t>& data, size_t offset) {
  if (offset + 4 > data.size()) {
    return 0;
  }
  return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
         (uint32_t(data[offset + 2]) << 8) | uint32_t(data[offset + 3]);
}

// XEX2: "XEX2", header count at 0x14, (key, value) pairs from 0x18; the
// execution info (key 0x00040006) holds the title ID at +0xC.
uint32_t TitleIdFromXexHeader(const std::vector<uint8_t>& data) {
  if (data.size() < 0x18 || ReadBe32(data, 0) != 0x58455832) {
    return 0;
  }
  const uint32_t count = std::min<uint32_t>(ReadBe32(data, 0x14), 64);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = 0x18 + i * 8;
    if (ReadBe32(data, at) == 0x00040006) {
      // size_t: a uint32 sum would wrap past the bounds check in ReadBe32.
      return ReadBe32(data, size_t(ReadBe32(data, at + 4)) + 0xC);
    }
  }
  return 0;
}

}  // namespace

uint32_t ProbeTitleId(const std::string& game_path) {
  constexpr size_t kHeaderBytes = 64 * 1024;
  std::vector<uint8_t> data(kHeaderBytes);
  const std::filesystem::path path = xe::to_path(game_path);
  std::string ext = xe::path_to_utf8(path.extension());
  for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (ext == ".xex") {
    FILE* f = std::fopen(game_path.c_str(), "rb");
    if (!f) {
      return 0;
    }
    data.resize(std::fread(data.data(), 1, data.size(), f));
    std::fclose(f);
    return TitleIdFromXexHeader(data);
  }
  if (ext != ".iso") {
    return 0;
  }
  xe::vfs::DiscImageDevice device("\\Device\\Probe", path);
  if (!device.Initialize()) {
    return 0;
  }
  xe::vfs::Entry* entry = device.ResolvePath("default.xex");
  if (!entry) {
    return 0;
  }
  xe::vfs::File* file = nullptr;
  if (entry->Open(xe::vfs::FileAccess::kGenericRead, &file) != 0 || !file) {
    return 0;
  }
  size_t read = 0;
  file->ReadSync(std::span<uint8_t>(data.data(), data.size()), 0, &read);
  file->Destroy();
  data.resize(read);
  return TitleIdFromXexHeader(data);
}

void SaveTitleIcon(const std::string& storage_root, uint32_t title_id,
                   const uint8_t* png, size_t size) {
  if (!title_id || !png || !size) {
    return;
  }
  const std::filesystem::path dir = xe::to_path(storage_root) / "covers";
  const std::filesystem::path out = dir / fmt::format("{:08X}.png", title_id);
  std::error_code ec;
  if (std::filesystem::exists(out, ec)) {
    return;
  }
  std::filesystem::create_directories(dir, ec);
  // Write then rename: a truncated PNG would never be replaced, because an
  // existing file short-circuits the next save.
  std::filesystem::path tmp = out;
  tmp += ".tmp";
  FILE* f = std::fopen(xe::path_to_utf8(tmp).c_str(), "wb");
  if (!f) {
    return;
  }
  const bool written = std::fwrite(png, 1, size, f) == size;
  if (std::fclose(f) == 0 && written) {
    std::filesystem::rename(tmp, out, ec);
  }
  std::filesystem::remove(tmp, ec);
}

}  // namespace xdroid
