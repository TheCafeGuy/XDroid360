package xdroid360.app

import java.io.File

/**
 * Reads and edits the emulator's TOML config files in place: the global
 * xenia-edge.config.toml and per-game config/<TITLEID>.config.toml. Only
 * `key = value` lines inside `[Section]` blocks are touched, so comments and
 * unknown keys survive. The emulator reads these files at boot.
 */
class ConfigStore(private val storageRoot: File) {
  val globalFile: File get() = File(storageRoot, "xenia-edge.config.toml")

  fun gameFile(titleId: String): File =
    File(storageRoot, "config/${titleId.uppercase()}.config.toml")

  /** Raw value text (strings keep their quotes) or null if absent. */
  fun read(file: File, key: String): String? {
    if (!file.exists()) return null
    val prefix = Regex("^\\s*" + Regex.escape(key) + "\\s*=")
    for (line in file.readLines()) {
      if (prefix.containsMatchIn(line)) return valueOf(line)
    }
    return null
  }

  /** Sets key = rawValue inside [section], creating either if needed. */
  fun write(file: File, section: String, key: String, rawValue: String) {
    val lines = if (file.exists()) file.readLines().toMutableList() else mutableListOf()
    val prefix = Regex("^\\s*" + Regex.escape(key) + "\\s*=")
    val existing = lines.indexOfFirst { prefix.containsMatchIn(it) }
    if (existing >= 0) {
      val comment = lines[existing].substringAfter('#', "")
      lines[existing] =
        "$key = $rawValue" + if (comment.isNotEmpty()) "\t#$comment" else ""
    } else {
      val header = "[$section]"
      val at = lines.indexOfFirst { it.trim() == header }
      if (at < 0) {
        if (lines.isNotEmpty() && lines.last().isNotBlank()) lines.add("")
        lines.add(header)
        lines.add("$key = $rawValue")
      } else {
        var insert = at + 1
        while (insert < lines.size && !lines[insert].trim().startsWith("[")) insert++
        while (insert > at + 1 && lines[insert - 1].isBlank()) insert--
        lines.add(insert, "$key = $rawValue")
      }
    }
    file.parentFile?.mkdirs()
    file.writeText(lines.joinToString("\n") + "\n")
  }

  /** Removes key (per-game override back to the global value). */
  fun remove(file: File, key: String) {
    if (!file.exists()) return
    val prefix = Regex("^\\s*" + Regex.escape(key) + "\\s*=")
    val lines = file.readLines().filterNot { prefix.containsMatchIn(it) }
    file.writeText(lines.joinToString("\n") + "\n")
  }

  private fun valueOf(line: String): String {
    val raw = line.substringAfter('=').trim()
    if (raw.startsWith("\"")) {
      val end = raw.indexOf('"', 1)
      return if (end > 0) raw.substring(0, end + 1) else raw
    }
    return raw.substringBefore('#').trim()
  }

  companion object {
    fun unquote(raw: String): String =
      if (raw.length >= 2 && raw.startsWith("\"") && raw.endsWith("\"")) {
        raw.substring(1, raw.length - 1)
      } else {
        raw
      }
  }
}
