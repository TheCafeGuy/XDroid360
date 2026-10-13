// XDroid360 JNI entry points, registered on xdroid360.core.Bridge.
//
// Threads: boot, surface, layout and destroy come from the UI thread (the
// app context needs its looper). Pause, resume, trim and the title id may
// come from any thread; they and destroy share app_mutex so none of them
// can use the app while it is being torn down.
#include <jni.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include "xdroid_app.h"
#include "xdroid_content.h"
#include "xdroid_gamepad.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/ui/windowed_app_context_android.h"

DECLARE_path(log_file);

namespace {

std::mutex app_mutex;
xe::ui::AndroidWindowedAppContext* app_context = nullptr;

std::string ToString(JNIEnv* env, jstring value) {
  if (!value) {
    return {};
  }
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (!chars) {
    return {};
  }
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

// activity.getApplicationInfo().nativeLibraryDir
std::string NativeLibraryDir(JNIEnv* env, jobject activity) {
  jclass activity_class = env->GetObjectClass(activity);
  jmethodID get_info =
      env->GetMethodID(activity_class, "getApplicationInfo",
                       "()Landroid/content/pm/ApplicationInfo;");
  jobject info = env->CallObjectMethod(activity, get_info);
  jclass info_class = env->GetObjectClass(info);
  jfieldID dir_field =
      env->GetFieldID(info_class, "nativeLibraryDir", "Ljava/lang/String;");
  auto dir = static_cast<jstring>(env->GetObjectField(info, dir_field));
  std::string result = ToString(env, dir);
  env->DeleteLocalRef(dir);
  env->DeleteLocalRef(info_class);
  env->DeleteLocalRef(info);
  env->DeleteLocalRef(activity_class);
  return result;
}

void DestroyAppLocked() {
  if (app_context) {
    // Destroys the app; the context deletes itself afterwards.
    app_context->JniActivityOnDestroy();
    app_context = nullptr;
  }
}

jstring Version(JNIEnv* env, jclass) {
  const std::string version =
      std::string("XDroid360 ") + xdroid::BuildId() + " / xenia-edge 77f2cca80";
  return env->NewStringUTF(version.c_str());
}

jint Boot(JNIEnv* env, jclass, jobject activity, jobject asset_manager,
          jstring storage_root, jstring cache_root, jstring game_path,
          jstring driver_path) {
  std::lock_guard<std::mutex> lock(app_mutex);
  DestroyAppLocked();
  xdroid::BootParams params;
  params.storage_root = ToString(env, storage_root);
  params.cache_root = ToString(env, cache_root);
  params.game_path = ToString(env, game_path);
  params.driver_path = ToString(env, driver_path);
  params.native_lib_dir = NativeLibraryDir(env, activity);
  if (params.storage_root.empty() || params.cache_root.empty()) {
    return -2;
  }
  // Before logging starts (inside the context creation below): the file
  // keeps what logcat drops and becomes the crash report of a dead session.
  cvars::log_file = std::filesystem::path(params.storage_root) / "xdroid.log";
  xdroid::SetBootParams(std::move(params));
  jstring app_name = env->NewStringUTF("xdroid");
  if (!app_name) {
    return -3;
  }
  app_context =
      xe::ui::AndroidWindowedAppContext::JniActivityInitializeWindowedAppOnCreate(
          env, activity, app_name, asset_manager);
  env->DeleteLocalRef(app_name);
  return app_context ? 0 : -4;
}

void SurfaceChanged(JNIEnv*, jclass, jobject surface) {
  if (app_context) {
    app_context->JniActivityOnWindowSurfaceChanged(surface);
  }
  if (auto* app = xdroid::XDroidApp::Get()) {
    app->RebindPresenterIfSurfaceChanged();
  }
}

void SurfaceLayout(JNIEnv*, jclass, jint left, jint top, jint right,
                   jint bottom) {
  if (app_context) {
    app_context->JniActivityOnWindowSurfaceLayoutChange(left, top, right,
                                                        bottom);
  }
  if (auto* app = xdroid::XDroidApp::Get()) {
    app->RebindPresenterIfSurfaceChanged();
  }
}

void Destroy(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lock(app_mutex);
  DestroyAppLocked();
}

void GamepadButton(JNIEnv*, jclass, jint mask, jboolean down) {
  xdroid::XDroidGamepadDriver::SetButton(uint16_t(mask), down == JNI_TRUE);
}

void GamepadAxis(JNIEnv*, jclass, jint axis, jfloat value) {
  if (axis >= 0 && axis <= int(xdroid::GamepadAxis::kRightTrigger)) {
    xdroid::XDroidGamepadDriver::SetAxis(xdroid::GamepadAxis(axis), value);
  }
}

// Guest frames presented so far; the activity derives FPS and stalls.
jlong FrameNumber(JNIEnv*, jclass) {
  return jlong(xe::logging::GetFrameNumber());
}

void Pause(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lock(app_mutex);
  if (auto* app = xdroid::XDroidApp::Get()) {
    app->SetPaused(true);
  }
}

void Resume(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lock(app_mutex);
  if (auto* app = xdroid::XDroidApp::Get()) {
    app->SetPaused(false);
  }
}

void Trim(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lock(app_mutex);
  if (auto* app = xdroid::XDroidApp::Get()) {
    app->TrimMemory();
  }
}

void FastForward(JNIEnv*, jclass, jboolean on) {
  SetFastForward(on == JNI_TRUE);
  XELOGI("XDroid360: fast forward {}", on ? "on" : "off");
}

jstring InstallContent(JNIEnv* env, jclass, jstring storage_root,
                       jstring package_path) {
  const std::string result = xdroid::InstallContentPackage(
      ToString(env, storage_root), ToString(env, package_path));
  return env->NewStringUTF(result.c_str());
}

void ToneStrength(JNIEnv*, jclass, jfloat strength) {
  SetDisplayToneStrength(strength);
}

jlong TitleId(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lock(app_mutex);
  auto* app = xdroid::XDroidApp::Get();
  return app ? jlong(app->current_title_id()) : 0;
}

// Null unless the last boot failed after Boot() returned (boot is async).
jstring BootError(JNIEnv* env, jclass) {
  const std::string error = xdroid::BootError();
  return error.empty() ? nullptr : env->NewStringUTF(error.c_str());
}

jlong ProbeTitleId(JNIEnv* env, jclass, jstring path) {
  return jlong(xdroid::ProbeTitleId(ToString(env, path)));
}

template <typename F>
JNINativeMethod Method(const char* name, const char* signature, F function) {
  return {name, signature, reinterpret_cast<void*>(function)};
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  jclass bridge = env->FindClass("xdroid360/core/Bridge");
  if (!bridge) {
    return JNI_ERR;
  }
  const JNINativeMethod methods[] = {
      Method("nativeVersion", "()Ljava/lang/String;", &Version),
      Method("nativeBoot",
             "(Landroid/app/Activity;Landroid/content/res/AssetManager;"
             "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
             "Ljava/lang/String;)I",
             &Boot),
      Method("nativeSurfaceChanged", "(Landroid/view/Surface;)V",
             &SurfaceChanged),
      Method("nativeSurfaceLayout", "(IIII)V", &SurfaceLayout),
      Method("nativeDestroy", "()V", &Destroy),
      Method("nativeGamepadButton", "(IZ)V", &GamepadButton),
      Method("nativeGamepadAxis", "(IF)V", &GamepadAxis),
      Method("nativeFrameNumber", "()J", &FrameNumber),
      Method("nativePause", "()V", &Pause),
      Method("nativeResume", "()V", &Resume),
      Method("nativeTrim", "()V", &Trim),
      Method("nativeSetFastForward", "(Z)V", &FastForward),
      Method("nativeInstallContent",
             "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
             &InstallContent),
      Method("nativeSetToneStrength", "(F)V", &ToneStrength),
      Method("nativeTitleId", "()J", &TitleId),
      Method("nativeProbeTitleId", "(Ljava/lang/String;)J", &ProbeTitleId),
      Method("nativeBootError", "()Ljava/lang/String;", &BootError),
  };
  if (env->RegisterNatives(bridge, methods,
                           sizeof(methods) / sizeof(methods[0])) != JNI_OK) {
    return JNI_ERR;
  }
  return JNI_VERSION_1_6;
}
