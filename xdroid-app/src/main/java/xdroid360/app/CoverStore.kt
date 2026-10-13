package xdroid360.app

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Library cover art per title ID: <TITLEID>.jpg (box art downloaded from
 * Xbox's legacy marketplace image host, optional) preferred over
 * <TITLEID>.png (the game's own icon, saved by native code at launch).
 */
class CoverStore(private val dir: File) {
  private val cache = HashMap<String, Bitmap?>()

  fun boxArt(titleId: String) = File(dir, "${titleId.uppercase()}.jpg")

  private fun icon(titleId: String) = File(dir, "${titleId.uppercase()}.png")

  fun hasBoxArt(titleId: String) = boxArt(titleId).length() > 0

  @Synchronized
  fun bitmap(titleId: String): Bitmap? {
    val key = titleId.uppercase()
    if (cache.containsKey(key) && cache[key] != null) return cache[key]
    val file = boxArt(key).takeIf { it.length() > 0 } ?: icon(key).takeIf { it.length() > 0 }
    val bmp =
      file?.let {
        val opts = BitmapFactory.Options().apply { inSampleSize = 1 }
        BitmapFactory.decodeFile(it.absolutePath, opts)
      }
    cache[key] = bmp
    return bmp
  }

  @Synchronized
  fun invalidate(titleId: String) {
    cache.remove(titleId.uppercase())
  }

  /**
   * Blocking download; true if box art is now on disk, false if the server has
   * none, null on a network error (worth retrying soon). Call off the UI thread.
   */
  fun downloadBoxArt(titleId: String): Boolean? {
    val id = titleId.lowercase()
    val url =
      URL("http://download.xbox.com/content/images/66acd000-77fe-1000-9115-d802$id/1033/boxartlg.jpg")
    return try {
      val conn = url.openConnection() as HttpURLConnection
      conn.connectTimeout = 8000
      conn.readTimeout = 15000
      conn.instanceFollowRedirects = true
      if (conn.responseCode != 200 || conn.contentType?.startsWith("image") != true) {
        conn.disconnect()
        return false
      }
      dir.mkdirs()
      val tmp = File(dir, "$id.part")
      conn.inputStream.use { inp -> tmp.outputStream().use { inp.copyTo(it) } }
      conn.disconnect()
      val ok = tmp.length() > 1024 && tmp.renameTo(boxArt(titleId))
      if (!ok) tmp.delete()
      invalidate(titleId)
      ok
    } catch (_: Exception) {
      null
    }
  }
}
