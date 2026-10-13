package xdroid360.app

import android.content.Context
import android.os.Build
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Session reports in files/xdroid/logs. A marker exists while a game session
 * runs. Finding it later means the session died without ending (crash, kill),
 * so its log is kept as a crash report instead of being overwritten by the
 * next boot.
 */
object SessionReports {
  private const val KEEP_PER_KIND = 10

  private fun root(context: Context) = File(context.filesDir, "xdroid")

  private fun marker(context: Context) = File(root(context), "session.marker")

  fun logsDir(context: Context) = File(root(context), "logs").apply { mkdirs() }

  /** Files a crash report for a session that died. Returns a status line. */
  fun recoverDeadSession(context: Context): String? {
    val marker = marker(context)
    if (!marker.exists()) return null
    val previous = runCatching { marker.readText() }.getOrDefault("")
    val background = previous.contains("state=background")
    write(context, "crash", if (background) "died in background" else "died", previous, "")
    marker.delete()
    return if (background) {
      "Previous session died in the background — report saved"
    } else {
      "Previous session crashed — report saved"
    }
  }

  fun begin(context: Context, gamePath: String) {
    runCatching {
      marker(context).writeText("game=$gamePath\nstarted=${Date()}\nstate=foreground\n")
    }
  }

  /** Foreground or background: tells a crash from a background kill. */
  fun setState(context: Context, state: String) {
    val marker = marker(context)
    if (!marker.exists()) return
    runCatching {
      val lines = marker.readLines().filterNot { it.startsWith("state=") }
      marker.writeText((lines + "state=$state").joinToString("\n", postfix = "\n"))
    }
  }

  /** Ends the session. Crash reports are what the About tab counts. */
  fun end(context: Context, reason: String, crash: Boolean, details: String = "") {
    val marker = marker(context)
    if (!marker.exists()) return
    val previous = runCatching { marker.readText() }.getOrDefault("")
    write(context, if (crash) "crash" else "session", reason, previous, details)
    marker.delete()
  }

  /** Files a crash report outside a session (its marker is already gone). */
  fun fileCrash(context: Context, reason: String) = write(context, "crash", reason, "", "")

  private fun write(
    context: Context,
    kind: String,
    reason: String,
    previous: String,
    details: String,
  ) {
    runCatching {
      val dir = logsDir(context)
      val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
      val log = File(root(context), "xdroid.log")
      if (log.exists()) log.copyTo(File(dir, "$kind-$stamp.log"), overwrite = true)
      val driver =
        context.getSharedPreferences("xdroid", Context.MODE_PRIVATE)
          .getString("driver_name", "").orEmpty().ifEmpty { "system" }
      File(dir, "$kind-$stamp.txt").writeText(
        "XDroid360 $kind report ($reason)\n" +
          "date=${Date()}\n" +
          "device=${Build.MANUFACTURER} ${Build.MODEL} " +
          "android=${Build.VERSION.RELEASE} soc=${Build.HARDWARE}\n" +
          "driver=$driver\n" +
          (if (details.isNotEmpty()) "$details\n" else "") +
          previous,
      )
      prune(dir, kind)
    }
  }

  private fun prune(dir: File, kind: String) {
    val sessions =
      dir.listFiles { f -> f.name.startsWith("$kind-") }
        ?.groupBy { it.nameWithoutExtension }
        ?.toSortedMap() ?: return
    sessions.values.toList().dropLast(KEEP_PER_KIND).flatten().forEach { it.delete() }
  }
}
