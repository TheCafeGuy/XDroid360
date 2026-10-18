package xdroid360.app

import android.content.Context
import java.io.File
import java.nio.file.Files

/**
 * The app's single Diagnostics switch: native cvar xdroid_diagnostics in the
 * global config (xdroid_defaults.cc forces every diagnostic cvar from it at
 * boot). On, the session also gets the black box ([Forensics]) and the
 * emulator log goes to the app's external files dir, which adb reads on a
 * non-debuggable release APK (/sdcard/Android/data/<id>/files/xdroid.log).
 * One build for testers and players: max performance is this switch off.
 */
object Diagnostics {
  const val KEY = "xdroid_diagnostics"
  const val SECTION = "XDroid360"

  /** Default on, like the cvar. */
  fun enabled(storage: File): Boolean =
    ConfigStore(storage).let { it.read(it.globalFile, KEY) } != "false"

  fun set(storage: File, on: Boolean) {
    ConfigStore(storage).let { it.write(it.globalFile, SECTION, KEY, on.toString()) }
  }

  /** Before boot: storage/xdroid.log becomes a link into the external dir when on. */
  fun linkLog(context: Context, storage: File, on: Boolean) {
    val log = File(storage, "xdroid.log").toPath()
    try {
      if (Files.isSymbolicLink(log)) Files.delete(log)
      if (!on) return
      val dir = context.getExternalFilesDir(null) ?: return
      Files.deleteIfExists(log)
      Files.createSymbolicLink(log, File(dir, "xdroid.log").toPath())
    } catch (e: Exception) {
      android.util.Log.w("XDroid360", "diagnostics log link: $e")
    }
  }
}
