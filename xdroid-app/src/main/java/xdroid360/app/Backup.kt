package xdroid360.app

import android.content.SharedPreferences
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream
import org.json.JSONArray
import org.json.JSONObject

/**
 * Save and settings backups as one zip, written and read through the
 * Storage Access Framework (the user picks the file).
 *
 * Layout: the manifest first (so an import knows what it replaces before any
 * file arrives), then content/<xuid>/<title>/00000001/..., the matching
 * Headers/00000001/... and each profile (<xuid>/FFFE07D1/...: saves belong
 * to the profile's XUID), then settings/xenia-edge.config.toml,
 * settings/config/<TITLE>.config.toml and settings/prefs.json.
 */
object Backup {
  private const val MANIFEST = "xdroid360-backup.json"
  private const val SAVE = "00000001"
  private const val PROFILE = "FFFE07D1"
  // Preferences tied to this device's paths stay out.
  private val LOCAL_PREFS = listOf("title_", "last_")
  private val HEX8 = Regex("[0-9A-Fa-f]{8}")
  private val HEX16 = Regex("[0-9A-Fa-f]{16}")
  private val CONFIG_NAME = Regex("[0-9A-Fa-f]{8}\\.config\\.toml")

  data class Contents(val titles: List<String>, val settings: Boolean)

  /** Title ids with at least one save, across all profiles. */
  fun savedTitles(root: File): List<String> =
    profileDirs(root).flatMap { xuid ->
      xuid.listFiles()?.filter { it.isDirectory && it.name != PROFILE && File(it, SAVE).isDirectory }
        ?.map { it.name.uppercase() }.orEmpty()
    }.distinct().sorted()

  fun export(root: File, prefs: SharedPreferences, out: OutputStream, titles: Set<String>, settings: Boolean) {
    ZipOutputStream(out.buffered()).use { zip ->
      val manifest = JSONObject()
        .put("version", 1)
        .put("titles", JSONArray(titles.sorted()))
        .put("settings", settings)
      zip.putNextEntry(ZipEntry(MANIFEST))
      zip.write(manifest.toString(2).toByteArray())
      zip.closeEntry()
      if (titles.isNotEmpty()) {
        for (xuid in profileDirs(root)) {
          addTree(zip, File(xuid, PROFILE), "content/${xuid.name}/$PROFILE")
          xuid.listFiles()?.filter { it.isDirectory && it.name.uppercase() in titles }?.forEach { t ->
            addTree(zip, File(t, SAVE), "content/${xuid.name}/${t.name}/$SAVE")
            addTree(zip, File(t, "Headers/$SAVE"), "content/${xuid.name}/${t.name}/Headers/$SAVE")
          }
        }
      }
      if (settings) {
        addFile(zip, File(root, "xenia-edge.config.toml"), "settings/xenia-edge.config.toml")
        File(root, "config").listFiles()?.filter { CONFIG_NAME.matches(it.name) }?.forEach {
          addFile(zip, it, "settings/config/${it.name}")
        }
        zip.putNextEntry(ZipEntry("settings/prefs.json"))
        zip.write(prefsJson(prefs).toString(2).toByteArray())
        zip.closeEntry()
      }
    }
  }

  /** Reads only the manifest; null when the file is not one of our backups. */
  fun inspect(input: InputStream): Contents? =
    ZipInputStream(input.buffered()).use { zip ->
      val entry = zip.nextEntry ?: return null
      if (entry.name != MANIFEST) return null
      val m = try {
        JSONObject(zip.readBytes().decodeToString())
      } catch (e: org.json.JSONException) {
        return null
      }
      val arr = m.optJSONArray("titles") ?: JSONArray()
      Contents((0 until arr.length()).map { arr.getString(it).uppercase() }, m.optBoolean("settings"))
    }

  /**
   * Replace deletes the saves of the titles in the backup first; merge only
   * overwrites the files the backup carries. Returns the files written.
   */
  fun import(root: File, prefs: SharedPreferences, input: InputStream, replace: Boolean): Int {
    var written = 0
    ZipInputStream(input.buffered()).use { zip ->
      val first = zip.nextEntry ?: return 0
      if (first.name != MANIFEST) return 0
      val m = JSONObject(zip.readBytes().decodeToString())
      val arr = m.optJSONArray("titles") ?: JSONArray()
      val titles = (0 until arr.length()).map { arr.getString(it).uppercase() }.toSet()
      if (replace) {
        for (xuid in profileDirs(root)) {
          xuid.listFiles()?.filter { it.name.uppercase() in titles }?.forEach { t ->
            File(t, SAVE).deleteRecursively()
            File(t, "Headers/$SAVE").deleteRecursively()
          }
        }
      }
      while (true) {
        val e = zip.nextEntry ?: break
        if (e.isDirectory) continue
        val name = e.name
        if (name == "settings/prefs.json") {
          applyPrefs(prefs, JSONObject(zip.readBytes().decodeToString()))
          written++
          continue
        }
        val target = targetFor(root, name) ?: continue
        target.parentFile?.mkdirs()
        val tmp = File(target.parentFile, ".${target.name}.import")
        tmp.outputStream().use { zip.copyTo(it) }
        if (!tmp.renameTo(target)) {
          target.delete()
          tmp.renameTo(target)
        }
        written++
      }
    }
    return written
  }

  // Only the paths export writes; anything else (or "..") is skipped.
  private fun targetFor(root: File, name: String): File? {
    val p = name.split('/')
    if (p.any { it.isEmpty() || it == "." || it == ".." }) return null
    return when {
      p.size == 2 && p[0] == "settings" && p[1] == "xenia-edge.config.toml" -> File(root, p[1])
      p.size == 3 && p[0] == "settings" && p[1] == "config" && CONFIG_NAME.matches(p[2]) ->
        File(root, "config/${p[2]}")
      p.size >= 5 && p[0] == "content" && HEX16.matches(p[1]) && p[2] == PROFILE -> File(root, name)
      p.size >= 5 && p[0] == "content" && HEX16.matches(p[1]) && HEX8.matches(p[2]) &&
        (p[3] == SAVE || (p.size >= 6 && p[3] == "Headers" && p[4] == SAVE)) -> File(root, name)
      else -> null
    }
  }

  private fun profileDirs(root: File): List<File> =
    File(root, "content").listFiles()?.filter { it.isDirectory && HEX16.matches(it.name) }.orEmpty()

  private fun addTree(zip: ZipOutputStream, dir: File, prefix: String) {
    dir.listFiles()?.forEach { f ->
      if (f.isDirectory) addTree(zip, f, "$prefix/${f.name}") else addFile(zip, f, "$prefix/${f.name}")
    }
  }

  private fun addFile(zip: ZipOutputStream, f: File, name: String) {
    if (!f.isFile) return
    zip.putNextEntry(ZipEntry(name).apply { time = f.lastModified() })
    f.inputStream().use { it.copyTo(zip) }
    zip.closeEntry()
  }

  private fun prefsJson(prefs: SharedPreferences): JSONObject {
    val out = JSONObject()
    for ((k, v) in prefs.all) {
      if (LOCAL_PREFS.any { k.startsWith(it) }) continue
      val typed = when (v) {
        is Boolean -> JSONObject().put("t", "b").put("v", v)
        is Float -> JSONObject().put("t", "f").put("v", v.toDouble())
        is Int -> JSONObject().put("t", "i").put("v", v)
        is Long -> JSONObject().put("t", "l").put("v", v)
        is String -> JSONObject().put("t", "s").put("v", v)
        else -> continue
      }
      out.put(k, typed)
    }
    return out
  }

  private fun applyPrefs(prefs: SharedPreferences, json: JSONObject) {
    val edit = prefs.edit()
    for (k in json.keys()) {
      if (LOCAL_PREFS.any { k.startsWith(it) }) continue
      val o = json.optJSONObject(k) ?: continue
      when (o.optString("t")) {
        "b" -> edit.putBoolean(k, o.optBoolean("v"))
        "f" -> edit.putFloat(k, o.optDouble("v").toFloat())
        "i" -> edit.putInt(k, o.optInt("v"))
        "l" -> edit.putLong(k, o.optLong("v"))
        "s" -> edit.putString(k, o.optString("v"))
      }
    }
    edit.apply()
  }
}
