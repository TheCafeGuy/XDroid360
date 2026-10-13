package xdroid360.core;

import android.app.Activity;
import android.content.res.AssetManager;
import android.view.Surface;

/**
 * JNI boundary to libxdroid360.so. The natives are registered in JNI_OnLoad
 * (xdroid_jni.cc), so a missing library fails on first use of this class.
 *
 * <p>Threads: boot, surface, layout and destroy on the UI thread; pause,
 * resume, trim, the title ID and the boot error from any thread.
 */
public final class Bridge {
  static {
    System.loadLibrary("xdroid360");
  }

  private Bridge() {}

  public static String version() {
    return nativeVersion();
  }

  /** Boots the emulator. UI thread only. Returns 0 on success. */
  public static int boot(
      Activity activity,
      AssetManager assetManager,
      String storageRoot,
      String cacheRoot,
      String gamePath,
      String driverPath) {
    return nativeBoot(activity, assetManager, storageRoot, cacheRoot, gamePath, driverPath);
  }

  public static void surfaceChanged(Surface surface) {
    nativeSurfaceChanged(surface);
  }

  public static void surfaceLayout(int left, int top, int right, int bottom) {
    nativeSurfaceLayout(left, top, right, bottom);
  }

  /** Stops the emulator and joins its threads. UI thread only; idempotent. */
  public static void destroy() {
    nativeDestroy();
  }

  /** Gamepad button by XInput mask. */
  public static void gamepadButton(int mask, boolean down) {
    nativeGamepadButton(mask, down);
  }

  /** Axis 0-5 (LX, LY, RX, RY, LT, RT): sticks -1..1, triggers 0..1. */
  public static void gamepadAxis(int axis, float value) {
    nativeGamepadAxis(axis, value);
  }

  /** Guest frames presented since boot. */
  public static long frameNumber() {
    return nativeFrameNumber();
  }

  /** Suspends the emulation (activity stopped). Applied once running. */
  public static void pause() {
    nativePause();
  }

  public static void resume() {
    nativeResume();
  }

  /** Drops reconstructible GPU caches under memory pressure. */
  public static void trim() {
    nativeTrim();
  }

  /** Uncapped guest vblanks. */
  public static void setFastForward(boolean on) {
    nativeSetFastForward(on);
  }

  /**
   * Installs one Xbox 360 content package (DLC, title update) into the content
   * folder. Returns "OK|type|titleId|name" or "ERR|reason". Blocking.
   */
  public static String installContent(String storageRoot, String packagePath) {
    return nativeInstallContent(storageRoot, packagePath);
  }

  /** AMOLED highlight compression, 0 (off) to 1, applied live. */
  public static void setToneStrength(float strength) {
    nativeSetToneStrength(strength);
  }

  /** Title ID of the running game, 0 until known. */
  public static long titleId() {
    return nativeTitleId();
  }

  /** Title ID from a game file's XEX header without booting, 0 if unknown. */
  public static long probeTitleId(String gamePath) {
    return nativeProbeTitleId(gamePath);
  }

  /**
   * Why the boot failed after {@link #boot} returned 0 (the boot finishes on a
   * native thread), or null while booting or once the game runs.
   */
  public static String bootError() {
    return nativeBootError();
  }

  private static native String nativeVersion();

  private static native int nativeBoot(
      Activity activity,
      AssetManager assetManager,
      String storageRoot,
      String cacheRoot,
      String gamePath,
      String driverPath);

  private static native void nativeSurfaceChanged(Surface surface);

  private static native void nativeSurfaceLayout(int left, int top, int right, int bottom);

  private static native void nativeDestroy();

  private static native void nativeGamepadButton(int mask, boolean down);

  private static native void nativeGamepadAxis(int axis, float value);

  private static native long nativeFrameNumber();

  private static native void nativePause();

  private static native void nativeResume();

  private static native void nativeTrim();

  private static native void nativeSetFastForward(boolean on);

  private static native String nativeInstallContent(String storageRoot, String packagePath);

  private static native void nativeSetToneStrength(float strength);

  private static native long nativeTitleId();

  private static native String nativeBootError();

  private static native long nativeProbeTitleId(String gamePath);
}
