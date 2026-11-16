#include "adb.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <utility>

#include "common.h"

namespace xv {
namespace {

constexpr wchar_t kRunAs[] = L"run-as io.github.thecafeguy.xdroid360 ";
// run-as and cat errors print no marker, so a failed read can never be
// written back as live.toml.
constexpr std::string_view kMarker = "\nXDROID_EOF\n";

bool Exists(const std::wstring& path) {
  DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Environment(const wchar_t* name) {
  std::wstring value(GetEnvironmentVariableW(name, nullptr, 0), L'\0');
  value.resize(GetEnvironmentVariableW(name, value.data(), value.size()));
  return value;
}

// Appends what the pipe holds without waiting for more.
void Drain(HANDLE pipe, std::string* out) {
  char buffer[4096];
  DWORD available = 0;
  DWORD got = 0;
  while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) &&
         available && ReadFile(pipe, buffer, sizeof buffer, &got, nullptr) &&
         got) {
    out->append(buffer, got);
  }
}

}  // namespace

bool Adb::Init(const std::wstring& path, const std::wstring& serial) {
  serial_ = serial;
  path_ = path;
  if (path_.empty()) {
    std::wstring dirs = Environment(L"PATH");
    for (size_t begin = 0; begin <= dirs.size() && path_.empty();) {
      size_t end = std::min(dirs.find(L';', begin), dirs.size());
      std::wstring candidate = dirs.substr(begin, end - begin) + L"\\adb.exe";
      if (end > begin && Exists(candidate)) {
        path_ = candidate;
      }
      begin = end + 1;
    }
  }
  if (path_.empty()) {
    path_ = Environment(L"LOCALAPPDATA") +
            L"\\Android\\Sdk\\platform-tools\\adb.exe";
  }
  if (!Exists(path_)) {
    std::fprintf(stderr, "adb: %ls not found (use --adb PATH or --no-adb)\n",
                 path_.c_str());
    return false;
  }
  return true;
}

bool Adb::Reverse(uint16_t port) {
  std::wstring tcp = L"tcp:" + std::to_wstring(port);
  return Run(L"reverse " + tcp + L" " + tcp, {}, nullptr);
}

void Adb::RemoveReverse(uint16_t port) {
  Run(L"reverse --remove tcp:" + std::to_wstring(port), {}, nullptr);
}

bool Adb::Publish(uint16_t port, const std::string& token_hex, bool on) {
  std::string current;
  if (!Run(L"exec-out \"" + std::wstring(kRunAs) +
               L"sh -c 'cat files/xdroid/live.toml 2>/dev/null; echo; "
               L"echo XDROID_EOF'\"",
           {}, &current) ||
      !current.ends_with(kMarker)) {
    std::fprintf(stderr,
                 "adb: cannot read live.toml (run-as needs a debug build), "
                 "not publishing\n");
    return false;
  }
  current.resize(current.size() - kMarker.size());
  // At the top, so the keys stay in the root table whatever follows.
  std::string toml = "xdroid_mirror_port = " + std::to_string(port) +
                     "\nxdroid_mirror_token = \"" + token_hex +
                     "\"\nxdroid_mirror = " + (on ? "true" : "false") + "\n";
  for (size_t begin = 0; begin < current.size();) {
    size_t end = std::min(current.find('\n', begin), current.size());
    std::string_view line(current.data() + begin, end - begin);
    // Drop only the keys written above: A/B arms such as xdroid_mirror_kbps
    // stay.
    std::string_view key =
        line.substr(std::min(line.find_first_not_of(" \t"), line.size()));
    key = key.substr(0, key.find_first_of(" \t="));
    if (key != "xdroid_mirror" && key != "xdroid_mirror_port" &&
        key != "xdroid_mirror_token") {
      toml.append(line);
      toml += '\n';
    }
    begin = end + 1;
  }
  // The idle re-publish every 3 s mostly finds the file as it should be.
  if (toml == current) {
    return true;
  }
  // Write then rename: the app never reads a half-written file.
  return Run(L"shell \"" + std::wstring(kRunAs) +
                 L"sh -c 'cat > files/xdroid/live.toml.tmp && mv "
                 L"files/xdroid/live.toml.tmp files/xdroid/live.toml'\"",
             toml, nullptr);
}

std::string Adb::MirrorToken() {
  std::string token;
  // `; true`: a missing file (switch off) is no adb failure to print.
  Run(L"shell \"cat /sdcard/Android/data/io.github.thecafeguy.xdroid360/"
      L"files/xdroid/mirror_token 2>/dev/null; true\"",
      {}, &token);
  return token;
}

bool Adb::Run(const std::wstring& args, const std::string& input,
              std::string* output) {
  std::lock_guard lock(mutex_);
  std::wstring command = L"\"" + path_ + L"\"";
  if (!serial_.empty()) {
    command += L" -s " + serial_;
  }
  command += L" " + args;

  // Only the child's ends are inheritable; separate pipes keep adb's own
  // messages out of the file content.
  SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
  HANDLE in_read, in_write, out_read, out_write, err_read, err_write;
  if (!CreatePipe(&in_read, &in_write, &inherit, 1 << 20)) {
    return false;
  }
  if (!CreatePipe(&out_read, &out_write, &inherit, 1 << 20)) {
    CloseHandle(in_read);
    CloseHandle(in_write);
    return false;
  }
  if (!CreatePipe(&err_read, &err_write, &inherit, 1 << 20)) {
    for (HANDLE handle : {in_read, in_write, out_read, out_write}) {
      CloseHandle(handle);
    }
    return false;
  }
  for (HANDLE handle : {in_write, out_read, err_read}) {
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = in_read;
  startup.hStdOutput = out_write;
  startup.hStdError = err_write;
  PROCESS_INFORMATION process{};
  bool started =
      CreateProcessW(path_.c_str(), command.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  for (HANDLE handle : {in_read, out_write, err_write}) {
    CloseHandle(handle);
  }
  std::string out;
  std::string err;
  DWORD code = 1;
  if (started) {
    // Fits the pipe buffer, so the write never waits for adb.
    DWORD written;
    WriteFile(in_write, input.data(), DWORD(input.size()), &written, nullptr);
    CloseHandle(in_write);
    in_write = nullptr;
    // A started adb server inherits the pipes and keeps them open, so wait
    // for adb itself to exit, never for end of file.
    int64_t begin = NowUs();
    for (bool exited = false; !exited;) {
      exited = WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0;
      Drain(out_read, &out);
      Drain(err_read, &err);
      if (!exited && NowUs() - begin > timeout_us_) {
        TerminateProcess(process.hProcess, 1);
        std::fprintf(stderr, "adb: timed out\n");
        break;
      }
    }
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  } else {
    std::fprintf(stderr, "adb: cannot start (%lu)\n", GetLastError());
  }
  for (HANDLE handle : {in_write, out_read, err_read}) {
    if (handle) {
      CloseHandle(handle);
    }
  }
  if (code || !err.empty()) {
    // Failures only: the idle re-publish runs adb every 3 s.
    std::fprintf(stderr, "adb %ls: exit %lu %s\n", args.c_str(), code,
                 err.c_str());
  }
  if (output) {
    *output = std::move(out);
  }
  return code == 0;
}

}  // namespace xv
