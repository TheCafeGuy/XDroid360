package xdroid360.app

import android.content.Context
import android.util.Log
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties

/** One component of assets/licenses/NOTICES.txt (see generateLicenseNotices). */
private class Notice(val title: String, val body: String)

/** The preamble, then one Notice per "==== name" section. */
private fun readNotices(context: Context): Pair<String, List<Notice>> {
  val text =
    try {
      context.assets.open("licenses/NOTICES.txt").bufferedReader().use { it.readText() }
    } catch (e: Exception) {
      Log.w("XDroid360", "cannot read the license notices", e)
      return "The license notices are missing from this build." to emptyList()
    }
  val parts = text.split("\n==== ")
  val notices =
    parts.drop(1).map { section ->
      val title = section.substringBefore('\n')
      Notice(title, section.substringAfter('\n').trim())
    }
  return parts.first().trim() to notices
}

/** Full-screen list of the bundled open-source notices; tap a name to expand it. */
@Composable
fun LicensesDialog(onDismiss: () -> Unit) {
  val context = LocalContext.current
  val (preamble, notices) = remember { readNotices(context) }
  var expanded by remember { mutableStateOf<String?>(null) }
  Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false)) {
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
      LazyColumn(contentPadding = PaddingValues(16.dp)) {
        item {
          Text("Open-source licenses", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
          Text(preamble, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(vertical = 8.dp))
          TextButton(onClick = onDismiss) { Text("Close") }
        }
        items(notices, key = { it.title }) { notice ->
          Column(
            Modifier.clickable { expanded = if (expanded == notice.title) null else notice.title }
              .padding(vertical = 8.dp)
          ) {
            Text(notice.title, fontWeight = FontWeight.Bold)
            if (expanded == notice.title) {
              Text(notice.body, fontFamily = FontFamily.Monospace, fontSize = 11.sp, modifier = Modifier.padding(top = 4.dp))
            } else {
              Text(
                notice.body.lineSequence().take(3).joinToString(" · "),
                style = MaterialTheme.typography.bodySmall,
                color = Color.Gray,
                maxLines = 1,
              )
            }
          }
        }
      }
    }
  }
}
