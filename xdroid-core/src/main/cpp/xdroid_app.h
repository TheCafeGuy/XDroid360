// XDroid360 WindowedApp: the Android counterpart of the desktop EmulatorApp.
// Registered as "xdroid" so AndroidWindowedAppContext can create it.
#ifndef XDROID_APP_H_
#define XDROID_APP_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/kernel/xobject.h"
#include "xenia/ui/windowed_app.h"

struct ANativeWindow;

namespace xe {
class Emulator;
namespace gpu {
class GraphicsSystem;
}  // namespace gpu
namespace kernel {
class XThread;
}  // namespace kernel
namespace ui {
class AndroidWindow;
class ImGuiDrawer;
class Presenter;
}  // namespace ui
}  // namespace xe

namespace xdroid {

// What the activity hands over at boot. One boot per app instance.
struct BootParams {
  std::string storage_root;
  std::string cache_root;
  std::string game_path;
  // Custom Vulkan driver .so; empty = system driver.
  std::string driver_path;
  // The app's native library dir (adrenotools hook libraries).
  std::string native_lib_dir;
};
// Must be called before the app is created. Clears the last boot error.
void SetBootParams(BootParams params);

// Why the last boot failed, for the user; empty while booting or once the
// title runs. Any thread.
std::string BootError();

// The build stamp (commit, +dirty) of this library.
const char* BuildId();

class XDroidApp : public xe::ui::WindowedApp {
 public:
  explicit XDroidApp(xe::ui::WindowedAppContext& app_context);
  ~XDroidApp() override;

  static std::unique_ptr<xe::ui::WindowedApp> Create(
      xe::ui::WindowedAppContext& app_context);

  // The live instance (UI thread creates and destroys it), or null.
  static XDroidApp* Get() { return instance_; }

  bool OnInitialize() override;

  // 0 until a title is running.
  uint32_t current_title_id() const;

  // UI thread. Binds the presenter to the current surface when the surface,
  // its size, or the presenter changed since the last bind.
  void RebindPresenterIfSurfaceChanged();

  // Any thread. Returns at once; the emulator thread applies the latest
  // request, and one made while the title is still booting as soon as it runs.
  void SetPaused(bool paused);

  // Drops GPU caches that can be rebuilt from guest memory.
  void TrimMemory();

 private:
  void EmulatorThreadMain();
  bool Boot();
  // Emulator thread only. Return the number of guest threads suspended or
  // resumed.
  size_t PauseEmulation();
  size_t ResumeEmulation();

  static XDroidApp* instance_;

  BootParams params_;
  std::unique_ptr<xe::ui::AndroidWindow> window_;
  std::unique_ptr<xe::ui::ImGuiDrawer> drawer_;
  std::unique_ptr<xe::Emulator> emulator_;
  std::thread emulator_thread_;
  std::atomic<bool> emulator_thread_exited_{false};
  // Copies of emulator state the emulator thread writes unlocked, for readers
  // on other threads: the running title, and the graphics system once its
  // presenter exists.
  std::atomic<uint32_t> title_id_{0};
  std::atomic<xe::gpu::GraphicsSystem*> graphics_{nullptr};

  std::mutex lifecycle_mutex_;
  // Wakes the emulator thread for quit_ or a pause_wanted_ change.
  std::condition_variable pause_cv_;
  bool quit_ = false;
  bool running_ = false;
  bool pause_wanted_ = false;
  bool paused_ = false;
  // The guest threads PauseEmulation suspended, for ResumeEmulation to resume
  // exactly those (emulator thread only).
  std::vector<xe::kernel::object_ref<xe::kernel::XThread>> paused_threads_;

  // Last bind (UI thread only). A bind to a zero-sized surface gives no
  // swapchain, so the size is part of the key.
  ANativeWindow* bound_surface_ = nullptr;
  int32_t bound_width_ = 0;
  int32_t bound_height_ = 0;
  xe::ui::Presenter* bound_presenter_ = nullptr;
};

}  // namespace xdroid

#endif  // XDROID_APP_H_
