package xdroid360.app

import java.io.File

/**
 * The app side of files/xdroid/live.toml (phone.sh live / liveab): the native
 * side applies live-safe cvars, this one the app's own switches
 * (`xdroid_frame_rate_vote`, `xdroid_show_stats`), so a whole A/B runs in one
 * flight without the tester touching a menu.
 */
class LiveFile(private val file: File) {
  private var stamp = 0L

  /** `key = value` pairs (values unquoted) if the file changed, else null. */
  fun poll(): Map<String, String>? {
    val now = file.lastModified()
    if (now == 0L || now == stamp) return null
    stamp = now
    val lines = runCatching { file.readLines() }.getOrNull() ?: return null
    return lines
      .mapNotNull { line ->
        val key = line.substringBefore('=', "").trim()
        if (key.isEmpty() || key.startsWith("#") || key.startsWith("[")) {
          null
        } else {
          key to line.substringAfter('=').trim().trim('"')
        }
      }
      .toMap()
  }
}
