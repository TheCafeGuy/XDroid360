// MIRROR-1 PC viewer: shows the XDroid360 game screen and plays its sound,
// streamed by the app over USB (adb reverse).
#include <objbase.h>
#include <shellapi.h>
#include <windows.h>
#include <winsock2.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>

#include "adb.h"
#include "audio.h"
#include "decode.h"
#include "net.h"
#include "render.h"

namespace xv {
int RunProbe();
int RunSelftest();
}  // namespace xv

namespace {

namespace mp = xdroid::mirror;

struct Options {
  bool probe = false;
  bool selftest = false;
  bool no_adb = false;
  // Each turns one change off for this run; none is kept.
  bool unpaced = false;
  bool audio_low_latency = false;
  bool no_autotune = false;
  bool interp = true;
  std::wstring adb;
  std::wstring serial;
  uint16_t port = mp::kDefaultPort;
};

struct App {
  xv::Renderer renderer;
  xv::Server server;
  bool fullscreen = false;
  WINDOWPLACEMENT placement{};
  int64_t kbps = 16000;  // The app's default xdroid_mirror_kbps.
};

HWND g_window;
HANDLE g_done;
xv::Adb* g_adb;

// GUI subsystem: print where the caller reads, a pipe or else its console.
void AttachOutput() {
  if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN &&
      AttachConsole(ATTACH_PARENT_PROCESS)) {
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
}

void ToggleFullscreen(HWND window, App* app) {
  if (!app->fullscreen) {
    // Borderless over the whole current monitor: the flip-model swapchain
    // can then go to independent flip, as fast as exclusive fullscreen.
    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    app->placement.length = sizeof app->placement;
    GetWindowPlacement(window, &app->placement);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                    &monitor);
    const RECT& r = monitor.rcMonitor;
    SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(window, HWND_TOP, r.left, r.top, r.right - r.left,
                 r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  } else {
    SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPlacement(window, &app->placement);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
  }
  app->fullscreen = !app->fullscreen;
  app->renderer.SetFullscreen(app->fullscreen);
}

void OnKey(HWND window, App* app, WPARAM key) {
  switch (key) {
    case VK_F11:
      ToggleFullscreen(window, app);
      break;
    case VK_ESCAPE:
      if (app->fullscreen) {
        ToggleFullscreen(window, app);
      }
      break;
    case 'K':
      app->server.SendControl(mp::Control::kRequestKey, 0);
      break;
    case VK_PRIOR:
    case VK_NEXT:
      app->kbps = std::clamp<int64_t>(
          app->kbps + (key == VK_PRIOR ? 2000 : -2000), 2000, 60000);
      app->server.SendControl(mp::Control::kSetKbps, app->kbps);
      std::fprintf(stderr, "xv: video bitrate %lld kbps\n", app->kbps);
      break;
    default:
      app->renderer.OnKey(UINT(key));
      break;
  }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(
        window, GWLP_USERDATA,
        LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
  }
  if (!app) {
    return DefWindowProcW(window, message, w, l);
  }
  switch (message) {
    case WM_SIZE:
      app->renderer.Resize();
      return 0;
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(l);
      if (!app->fullscreen) {
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left,
                     r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      }
      app->renderer.SetDpi(HIWORD(w));
      return 0;
    }
    case WM_LBUTTONDBLCLK:
      ToggleFullscreen(window, app);
      return 0;
    case WM_SYSKEYDOWN:
      if (w == VK_RETURN && !(l & (1 << 30))) {
        ToggleFullscreen(window, app);
        return 0;
      }
      break;
    case WM_SYSCHAR:
      if (w == VK_RETURN) {
        return 0;  // No beep for Alt+Enter.
      }
      break;
    case WM_KEYDOWN:
      // Held keys repeat only the bitrate steps.
      if (!(l & (1 << 30)) || w == VK_PRIOR || w == VK_NEXT) {
        OnKey(window, app, w);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      ValidateRect(window, nullptr);
      app->renderer.Redraw();
      return 0;
    case WM_DESTROY:
      // The render thread presents to this window: stop it while it lives.
      app->renderer.Stop();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, w, l);
}

BOOL WINAPI OnConsoleControl(DWORD) {
  // Windows ends the process when this returns (console closed), 5 s at
  // most: first let the viewer switch the phone back off, in short adb calls.
  g_adb->Hurry();
  PostMessageW(g_window, WM_CLOSE, 0, 0);
  WaitForSingleObject(g_done, 5000);
  return TRUE;
}

int RunViewer(HINSTANCE instance, const Options& options) {
  // Two viewers would keep re-publishing their own tokens over each other.
  CreateMutexW(nullptr, FALSE, L"XDroid360Mirror");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    std::fprintf(stderr, "xv: another viewer is running\n");
    return 1;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  xv::ComPtr<ID3D11Device> device = xv::CreateDevice();
  xv::Adb adb;
  if (!device || (!options.no_adb && !adb.Init(options.adb, options.serial))) {
    return 1;
  }
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof window_class;
  window_class.style = CS_DBLCLKS;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"XDroid360Mirror";
  RegisterClassExW(&window_class);
  App app;
  HWND window = CreateWindowExW(
      0, window_class.lpszClassName, L"XDroid360 mirror", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr,
      nullptr, instance, &app);
  if (!window || !app.renderer.Init(device.Get(), window)) {
    return 1;
  }
  // 1280x720 DIP of client area on the monitor the window opened on.
  UINT dpi = GetDpiForWindow(window);
  RECT frame{0, 0, MulDiv(1280, dpi, 96), MulDiv(720, dpi, 96)};
  AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left,
               frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER);
  app.renderer.SetDpi(dpi);

  xv::Decoder decoder;
  xv::AudioOut audio;
  xv::Server::Callbacks callbacks =
      xv::Route(device.Get(), &decoder, &audio, false);
  // The tunnel first, every time: once the app sees the tokens, the port
  // answers, also after a replug or an adb server restart. A debug APK gets
  // the viewer's own tokens through run-as. A public one has no run-as: its
  // "PC mirror (experimental)" switch writes its own tokens, read again
  // every second while no stream runs; without them only `own` (never
  // published) is accepted.
  bool public_apk = false;
  xv::Tokens own{};
  callbacks.on_tokens = [&](xv::Tokens& tokens) {
    if (!public_apk) {
      own = tokens;
    }
    if (options.no_adb || !adb.Reverse(options.port) ||
        (!public_apk && adb.Publish(options.port, tokens.Hex(), true))) {
      return false;
    }
    public_apk = true;
    bool valid = tokens.FromHex(adb.MirrorToken());
    if (!valid) {
      tokens = own;
    }
    const char* status =
        valid ? nullptr
              : "XDroid360 mirror: turn on \"PC mirror (experimental)\" in "
                "the game's menu";
    if (status != app.server.status()) {
      std::fprintf(
          stderr, "xv: %s\n",
          valid ? "token read from the app's PC mirror switch" : status);
      app.server.SetStatus(status);
    }
    return true;
  };
  bool ok = app.server.Start(options.port, callbacks);
  if (ok) {
    g_window = window;
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_adb = &adb;
    SetConsoleCtrlHandler(OnConsoleControl, TRUE);
    app.renderer.Start(&decoder, &app.server, &audio, !options.unpaced,
                       !options.no_autotune, options.interp);
    ShowWindow(window, SW_SHOWNORMAL);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  } else {
    DestroyWindow(window);
  }
  // No re-publish may run after the switch-off below.
  app.server.Stop();
  // A viewer that never listened never published: the phone may belong to
  // another viewer.
  if (ok && !options.no_adb) {
    if (!public_apk) {
      adb.Publish(options.port, "", false);
    }
    adb.RemoveReverse(options.port);
  }
  audio.Stop();
  if (g_done) {
    SetEvent(g_done);
  }
  return ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  AttachOutput();
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  Options options;
  bool usage = false;
  for (int i = 1; i < argc; ++i) {
    std::wstring arg = argv[i];
    bool value = i + 1 < argc;
    if (arg == L"--probe") {
      options.probe = true;
    } else if (arg == L"--selftest") {
      options.selftest = true;
    } else if (arg == L"--no-adb") {
      options.no_adb = true;
    } else if (arg == L"--unpaced") {
      options.unpaced = true;
    } else if (arg == L"--audio-low-latency") {
      options.audio_low_latency = true;
    } else if (arg == L"--no-autotune") {
      options.no_autotune = true;
    } else if (arg == L"--no-interp") {
      options.interp = false;
    } else if (arg == L"--interp") {
      // The default since v0.3.0: kept so that old shortcuts still start.
    } else if (arg == L"--adb" && value) {
      options.adb = argv[++i];
    } else if (arg == L"--serial" && value) {
      options.serial = argv[++i];
    } else if (arg == L"--port" && value) {
      unsigned long port = std::wcstoul(argv[++i], nullptr, 10);
      options.port = port <= 65535 ? uint16_t(port) : 0;
    } else {
      usage = true;
    }
  }
  LocalFree(argv);
  if (usage || !options.port) {
    std::fprintf(stderr,
                 "usage: xdroid-viewer [--probe | --selftest] [--no-adb] "
                 "[--adb PATH] [--serial S] [--port P] [--unpaced] "
                 "[--audio-low-latency] [--no-autotune] [--no-interp]\n");
    return 2;
  }
  xv::SetAudioLowLatency(options.audio_low_latency);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (options.probe) {
    return xv::RunProbe();
  }
  if (options.selftest) {
    return xv::RunSelftest();
  }
  return RunViewer(instance, options);
}
