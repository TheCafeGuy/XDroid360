// MIRROR-1 PC viewer: the adb commands that point the app at the viewer.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace xv {

class Adb {
 public:
  // path: --adb, else adb.exe on PATH, else the Android SDK's. serial:
  // --serial, empty for adb's default device.
  bool Init(const std::wstring& path, const std::wstring& serial);
  bool Reverse(uint16_t port);
  void RemoveReverse(uint16_t port);
  // Rewrites the app's live.toml: the xdroid_mirror* keys become these
  // values, every other line stays.
  bool Publish(uint16_t port, const std::string& token_hex, bool on);
  // What the public APK's "PC mirror (experimental)" switch wrote: Tokens
  // in Hex() form; empty while the switch is off.
  std::string MirrorToken();
  // For a closing console: every call, the running one too, ends within
  // 1.5 s, so the switch-off fits the 5 s Windows leaves the process.
  void Hurry() { timeout_us_ = 1500000; }

 private:
  // One adb command line, no shell; true when adb exits with 0.
  bool Run(const std::wstring& args, const std::string& input,
           std::string* output);

  std::wstring path_;
  std::wstring serial_;
  std::mutex mutex_;  // Re-publishes run on the accept thread.
  std::atomic<int64_t> timeout_us_{15000000};
};

}  // namespace xv
