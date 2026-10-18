// XDroid360 WindowedApp. Follows the desktop EmulatorApp order (Setup with
// the subsystem factories, MountStandardDrives, SetupSubsystems, LaunchPath)
// without the desktop UI: AAudio output, Vulkan, Android controller input.
#include "xdroid_app.h"

#include <android/native_window.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "xdroid_ac6_fixes.h"
#include "xdroid_defaults.h"
#include "xdroid_adpf.h"
#include "xdroid_audio.h"
#include "xdroid_build_id.h"
#include "xdroid_content.h"
#include "xdroid_driver.h"
#include "xdroid_gamepad.h"
#include "xdroid_gpu_bench.h"
#include "xdroid_live.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/game_info_database.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window_android.h"
#include "xenia/ui/windowed_app_context_android.h"

DECLARE_int32(vulkan_pipeline_creation_threads);
DECLARE_string(xdroid_gpu_bench);

namespace xdroid {
namespace {

constexpr size_t kInputZOrder = 0;
constexpr size_t kImGuiZOrder = 1;

BootParams g_boot_params;

std::mutex g_boot_error_mutex;
std::string g_boot_error;

void SetBootError(std::string error) {
  XELOGE("XDroid360: {}", error);
  std::lock_guard<std::mutex> lock(g_boot_error_mutex);
  g_boot_error = std::move(error);
}

// Device recreation exists only on desktop. Save the log and leave: the
// activity sees the process die mid-session and files a crash report.
// ShutdownLogging would free the logger while guest, APU and GPU threads still
// write to it; give the write thread time to drain instead (it naps 50 ms when
// idle) and flush the sinks.
void OnHostGpuLost() {
  XELOGE("XDroid360: host GPU lost, ending the session");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  xe::FlushLog();
  std::quick_exit(EXIT_FAILURE);
}

// Titles save only with a signed-in profile: create one on first boot, then
// sign in to the first account every boot.
void SignInLocalProfile(xe::Emulator* emulator) {
  auto* profiles = emulator->kernel_state()->xam_state()->profile_manager();
  if (profiles->IsAnyProfileSignedIn()) {
    return;
  }
  if (!profiles->GetAccounts()->empty()) {
    profiles->Login(profiles->GetAccounts()->begin()->first);
    return;
  }
  if (!profiles->CreateProfile("XDroid", true)) {
    XELOGE("XDroid360: could not create the local profile");
  }
}

}  // namespace

void SetBootParams(BootParams params) {
  g_boot_params = std::move(params);
  std::lock_guard<std::mutex> lock(g_boot_error_mutex);
  g_boot_error.clear();
}

std::string BootError() {
  std::lock_guard<std::mutex> lock(g_boot_error_mutex);
  return g_boot_error;
}

const char* BuildId() { return XDROID_BUILD_ID; }

XDroidApp* XDroidApp::instance_ = nullptr;

XDroidApp::XDroidApp(xe::ui::WindowedAppContext& app_context)
    : xe::ui::WindowedApp(app_context, "xdroid", "[Path to .iso/.xex]"),
      params_(std::move(g_boot_params)) {}

XDroidApp::~XDroidApp() {
  instance_ = nullptr;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    quit_ = true;
    running_ = false;
  }
  pause_cv_.notify_all();
  if (emulator_thread_.joinable()) {
    // During boot the emulator thread may be waiting in
    // CallInUIThreadSynchronous for this (UI) thread, so keep running UI
    // tasks until it exits.
    while (!emulator_thread_exited_.load(std::memory_order_acquire)) {
      app_context().ExecutePendingFunctionsFromUIThread();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    emulator_thread_.join();
  }
  // The destructor runs on the UI thread, which owns the presenter binding.
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  graphics_.store(nullptr, std::memory_order_release);
  // Kernel objects: release them while the kernel exists (the emulator
  // thread, their owner, has exited).
  paused_threads_.clear();
  if (emulator_) {
    AdpfSetEmulator(nullptr);
    emulator_->TerminateTitle();
    emulator_->Shutdown();
    emulator_.reset();
  }
  drawer_.reset();
  window_.reset();
}

std::unique_ptr<xe::ui::WindowedApp> XDroidApp::Create(
    xe::ui::WindowedAppContext& app_context) {
  return std::make_unique<XDroidApp>(app_context);
}

bool XDroidApp::OnInitialize() {
  XELOGI("XDroid360: build {}", XDROID_BUILD_ID);
  const std::filesystem::path storage_root(params_.storage_root);
  const std::filesystem::path content_root = storage_root / "content";
  const std::filesystem::path cache_root(params_.cache_root);
  std::error_code error;
  std::filesystem::create_directories(content_root, error);
  std::filesystem::create_directories(cache_root, error);

  config::SetupConfig(storage_root);
  ApplyTestedDefaults();
  StartLiveConfig(storage_root / "live.toml");
  SelectVulkanDriver(params_.driver_path, params_.native_lib_dir,
                     (storage_root / "tu_debug.txt").string());
  xe::gpu::GraphicsSystem::SetHostGpuLossHandler(&OnHostGpuLost);

  emulator_ = std::make_unique<xe::Emulator>("", storage_root, content_root,
                                            cache_root);
  InstallAc6Fixes(emulator_.get());
  StartAdpf();
  AdpfSetEmulator(emulator_.get());
  // Library cover fallback: the title's own icon from its XDBF.
  emulator_->on_launch.AddListener(
      [this, root = params_.storage_root](uint32_t title_id,
                                          std::string_view) {
        title_id_.store(title_id, std::memory_order_relaxed);
        auto* database = emulator_->game_info_database();
        if (database && database->IsValid()) {
          const std::vector<uint8_t> icon = database->GetIcon();
          SaveTitleIcon(root, title_id, icon.data(), icon.size());
        }
      });
  emulator_->on_terminate.AddListener(
      [this] { title_id_.store(0, std::memory_order_relaxed); });

  uint32_t width = 1280;
  uint32_t height = 720;
  if (auto* context = dynamic_cast<xe::ui::AndroidWindowedAppContext*>(
          &app_context())) {
    const int32_t layout_width = context->window_surface_layout_right() -
                                 context->window_surface_layout_left();
    const int32_t layout_height = context->window_surface_layout_bottom() -
                                  context->window_surface_layout_top();
    if (layout_width > 0 && layout_height > 0) {
      width = uint32_t(layout_width);
      height = uint32_t(layout_height);
    }
  }
  window_ = std::make_unique<xe::ui::AndroidWindow>(app_context(), "XDroid360",
                                                   width, height);
  if (!window_->Open()) {
    XELOGE("XDroid360: could not open the window");
    return false;
  }
  drawer_ = std::make_unique<xe::ui::ImGuiDrawer>(window_.get(), kImGuiZOrder);

  instance_ = this;
  emulator_thread_ = std::thread([this] {
    EmulatorThreadMain();
    emulator_thread_exited_.store(true, std::memory_order_release);
  });
  return true;
}

uint32_t XDroidApp::current_title_id() const {
  return title_id_.load(std::memory_order_relaxed);
}

void XDroidApp::EmulatorThreadMain() {
  if (!Boot()) {
    return;
  }
  // Guest work runs on its own threads; this one applies pause requests and
  // waits for teardown. Pause waits for the GPU thread to park, which never
  // happens when the GPU is hung, so no lock is held across it: the caller of
  // SetPaused (and app_mutex behind it) cannot wedge Destroy or TitleId.
  std::unique_lock<std::mutex> lock(lifecycle_mutex_);
  running_ = !quit_;
  while (true) {
    pause_cv_.wait(lock, [this] { return quit_ || pause_wanted_ != paused_; });
    if (quit_) {
      return;
    }
    const bool pause = pause_wanted_;
    lock.unlock();
    const size_t threads = pause ? PauseEmulation() : ResumeEmulation();
    XELOGI("XDroid360: emulation {} ({} guest threads)",
           pause ? "paused" : "resumed", threads);
    lock.lock();
    paused_ = pause;
  }
}

// Not Emulator::Pause, which never returned on device: it suspends the GPU
// thread, then waits for the audio worker to reach a pause point, which it
// cannot while it is inside a guest callback waiting on guest threads that
// wait on the GPU; it would then host-suspend threads at arbitrary points
// (holding the log or allocator locks, say) and call Suspend on the null host
// thread of every guest-scheduler fiber, and its Resume only resumes threads
// that are not running. Here nothing is waited on: guest threads are
// suspended the way the guest suspends them (a fiber parks at the scheduler's
// next dispatch), the list is kept so resume undoes exactly that, and the
// audio output stops. The GPU thread drains the ring and idles on its own.
size_t XDroidApp::PauseEmulation() {
  if (auto* audio =
          dynamic_cast<XDroidAudioSystem*>(emulator_->audio_system())) {
    audio->SetOutputPaused(true);
  }
  auto threads =
      emulator_->kernel_state()
          ->object_table()
          ->GetObjectsByType<xe::kernel::XThread>(
              xe::kernel::XObject::Type::Thread);
  for (auto& thread : threads) {
    // Host-routine threads (audio and XMA workers, vsync) run host loops;
    // without output and guest work they idle.
    if (thread->is_host_thread()) {
      continue;
    }
    thread->Suspend(nullptr);
    paused_threads_.push_back(std::move(thread));
  }
  if (auto* scheduler = emulator_->kernel_state()->guest_scheduler()) {
    scheduler->SetHostPaused(true);
  }
  return paused_threads_.size();
}

size_t XDroidApp::ResumeEmulation() {
  const size_t count = paused_threads_.size();
  if (auto* scheduler = emulator_->kernel_state()->guest_scheduler()) {
    scheduler->SetHostPaused(false);
  }
  for (auto& thread : paused_threads_) {
    thread->Resume(nullptr);
  }
  paused_threads_.clear();
  if (auto* audio =
          dynamic_cast<XDroidAudioSystem*>(emulator_->audio_system())) {
    audio->SetOutputPaused(false);
  }
  return count;
}

bool XDroidApp::Boot() {
  // Pipeline compiles contend on driver locks; the upstream default of 75%
  // of the cores (6 here) takes big cores from the JIT and the GPU thread.
  // Only when the config leaves the count on auto.
  if (cvars::vulkan_pipeline_creation_threads < 0) {
    cvars::vulkan_pipeline_creation_threads = 3;
  }

  xe::X_STATUS result = emulator_->Setup(
      window_.get(), drawer_.get(), /*require_cpu_backend=*/true,
      [](xe::cpu::Processor* processor) {
        return std::make_unique<XDroidAudioSystem>(processor);
      },
      [] { return std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>(); },
      [](xe::ui::Window* window) {
        std::vector<std::unique_ptr<xe::hid::InputDriver>> drivers;
        drivers.push_back(XDroidGamepadDriver::Create(window, kInputZOrder));
        drivers.push_back(xe::hid::nop::Create(window, kInputZOrder));
        return drivers;
      });
  if (XFAILED(result)) {
    SetBootError(fmt::format("Emulator setup failed (error {:08X})", result));
    return false;
  }
  emulator_->MountStandardDrives();
  if (params_.game_path.empty()) {
    SetBootError("No game selected");
    return false;
  }

  std::error_code path_error;
  const std::filesystem::path game_path =
      std::filesystem::absolute(params_.game_path, path_error);
  if (path_error) {
    SetBootError("Invalid game path: " + params_.game_path);
    return false;
  }
  if (auto xam = emulator_->kernel_state()
                     ->GetKernelModule<xe::kernel::xam::XamModule>("xam.xex")) {
    xam->loader_data().host_path = xe::path_to_utf8(game_path);
  }
  SignInLocalProfile(emulator_.get());
  config::LoadGameConfigForFile(game_path);
  // After the title config: loading it clears every game-config override,
  // which is where the Diagnostics switch puts its values.
  ApplyDiagnostics();

  result = emulator_->SetupSubsystems();
  if (XFAILED(result)) {
    SetBootError(
        fmt::format("Emulator subsystem setup failed (error {:08X})", result));
    return false;
  }
  // The presenter exists now; bind it to whatever surface is current.
  graphics_.store(emulator_->graphics_system(), std::memory_order_release);
  app_context().CallInUIThreadSynchronous(
      [this] { RebindPresenterIfSurfaceChanged(); });
  SetLiveGraphicsSystem(emulator_->graphics_system());
  if (StartGpuBench(emulator_->graphics_system(),
                    std::filesystem::path(params_.storage_root))) {
    XELOGI("XDroid360: GPU-BENCH replaying {} instead of the title",
           cvars::xdroid_gpu_bench);
    return true;
  }
  result = emulator_->LaunchPath(game_path);
  if (XFAILED(result)) {
    SetBootError(fmt::format("Could not load the game (error {:08X})", result));
    return false;
  }
  XELOGI("XDroid360: title launched");
  return true;
}

void XDroidApp::SetPaused(bool paused) {
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    pause_wanted_ = paused;
  }
  pause_cv_.notify_all();
}

void XDroidApp::TrimMemory() {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (running_ && emulator_->graphics_system()) {
    XELOGI("XDroid360: memory trim, clearing GPU caches");
    emulator_->graphics_system()->ClearCaches();
  }
}

void XDroidApp::RebindPresenterIfSurfaceChanged() {
  if (!window_) {
    return;
  }
  auto* context =
      dynamic_cast<xe::ui::AndroidWindowedAppContext*>(&app_context());
  ANativeWindow* surface = context ? context->GetWindowSurface() : nullptr;
  const int32_t width = surface ? ANativeWindow_getWidth(surface) : 0;
  const int32_t height = surface ? ANativeWindow_getHeight(surface) : 0;
  xe::gpu::GraphicsSystem* graphics = graphics_.load(std::memory_order_acquire);
  xe::ui::Presenter* presenter = graphics ? graphics->presenter() : nullptr;
  if (surface == bound_surface_ && width == bound_width_ &&
      height == bound_height_ && presenter == bound_presenter_) {
    return;
  }
  // SetPresenter ignores the same pointer, and the swapchain must be rebuilt
  // for a new surface: detach first.
  window_->SetPresenter(nullptr);
  window_->SetPresenter(presenter);
  bound_surface_ = surface;
  bound_width_ = width;
  bound_height_ = height;
  bound_presenter_ = presenter;
  XELOGI("XDroid360: presenter {} on a {}x{} surface",
         presenter ? "bound" : "detached", width, height);
}

}  // namespace xdroid

XE_DEFINE_WINDOWED_APP(xdroid, xdroid::XDroidApp::Create)
