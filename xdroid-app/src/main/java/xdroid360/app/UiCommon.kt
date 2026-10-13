package xdroid360.app

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import java.io.File

/** AMOLED-black dark scheme with a violet accent (ARMSX2-like). */
val XdColors: ColorScheme =
  darkColorScheme(
    primary = Color(0xFF8C7BFF),
    onPrimary = Color.Black,
    primaryContainer = Color(0xFF2A2350),
    onPrimaryContainer = Color(0xFFE6E0FF),
    secondary = Color(0xFF4FD1C5),
    background = Color.Black,
    surface = Color.Black,
    surfaceVariant = Color(0xFF15151C),
    surfaceContainer = Color(0xFF101016),
    surfaceContainerHigh = Color(0xFF1A1A22),
    onSurfaceVariant = Color(0xFFB8B8C8),
  )

fun formatSize(bytes: Long): String =
  when {
    bytes >= 1L shl 30 -> String.format(java.util.Locale.US, "%.1f GB", bytes / (1L shl 30).toDouble())
    bytes >= 1L shl 20 -> String.format(java.util.Locale.US, "%.0f MB", bytes / (1L shl 20).toDouble())
    else -> "${bytes / 1024} KB"
  }

/** Section title used across tabs. */
@Composable
fun SectionTitle(text: String) {
  Text(
    text,
    style = MaterialTheme.typography.titleMedium,
    fontWeight = FontWeight.SemiBold,
    color = MaterialTheme.colorScheme.primary,
    modifier = Modifier.padding(top = 12.dp, bottom = 4.dp),
  )
}

/** A colored square with the first letter of a name (no cover art offline). */
@Composable
fun LetterTile(name: String, modifier: Modifier = Modifier) {
  val palette = listOf(
    Color(0xFF3B2E7E), Color(0xFF1E5A6E), Color(0xFF6E2E4E), Color(0xFF2E6E3E),
    Color(0xFF6E5A1E), Color(0xFF2E3E7E),
  )
  val color = palette[(name.hashCode() and 0x7FFFFFFF) % palette.size]
  Box(
    modifier = modifier.clip(RoundedCornerShape(12.dp)).background(color),
    contentAlignment = Alignment.Center,
  ) {
    Text(
      name.trim().take(1).uppercase().ifEmpty { "?" },
      style = MaterialTheme.typography.displaySmall,
      fontWeight = FontWeight.Bold,
      color = Color.White.copy(alpha = 0.9f),
    )
  }
}

enum class BrowseMode { Folder, File, MultiFile }

/**
 * Full-screen file browser dialog. Folder: pick the current directory. File:
 * tap a file. MultiFile: check files, then confirm (or take the whole folder).
 */
@Composable
fun FileBrowserDialog(
  title: String,
  start: File,
  mode: BrowseMode,
  filter: (File) -> Boolean,
  onDismiss: () -> Unit,
  onPick: (List<File>) -> Unit,
) {
  var dir by remember { mutableStateOf(if (start.isDirectory) start else File("/sdcard")) }
  val checked = remember { mutableStateListOf<File>() }
  val entries =
    remember(dir) {
      (dir.listFiles()?.toList() ?: emptyList())
        .filter { it.isDirectory || filter(it) }
        .sortedWith(compareBy({ !it.isDirectory }, { it.name.lowercase() }))
    }
  AlertDialog(
    onDismissRequest = onDismiss,
    title = { Text(title) },
    text = {
      Column {
        Row(verticalAlignment = Alignment.CenterVertically) {
          OutlinedButton(
            onClick = { dir.parentFile?.takeIf { it.canRead() }?.let { dir = it } },
          ) { Text("⬆") }
          Spacer(Modifier.width(8.dp))
          Text(
            dir.absolutePath,
            style = MaterialTheme.typography.bodySmall,
            maxLines = 1,
            modifier = Modifier.horizontalScroll(rememberScrollState()),
          )
        }
        LazyColumn(modifier = Modifier.fillMaxWidth().padding(top = 8.dp)) {
          if (entries.isEmpty()) {
            item {
              Text("Empty folder", color = Color.Gray, modifier = Modifier.padding(12.dp))
            }
          }
          items(entries, key = { it.absolutePath }) { f ->
            Row(
              verticalAlignment = Alignment.CenterVertically,
              modifier = Modifier
                .fillMaxWidth()
                .clickable {
                  when {
                    f.isDirectory -> if (f.canRead()) dir = f
                    mode == BrowseMode.File -> onPick(listOf(f))
                    mode == BrowseMode.MultiFile ->
                      if (f in checked) checked.remove(f) else checked.add(f)
                  }
                }
                .padding(vertical = 8.dp),
            ) {
              if (mode == BrowseMode.MultiFile && !f.isDirectory) {
                Checkbox(checked = f in checked, onCheckedChange = {
                  if (it) checked.add(f) else checked.remove(f)
                })
              } else {
                Text(if (f.isDirectory) "📁" else "📄", modifier = Modifier.size(36.dp).padding(6.dp))
              }
              Column(Modifier.weight(1f)) {
                Text(f.name, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (!f.isDirectory) {
                  Text(formatSize(f.length()), style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                }
              }
            }
          }
        }
      }
    },
    confirmButton = {
      when (mode) {
        BrowseMode.Folder -> Button(onClick = { onPick(listOf(dir)) }) { Text("Use this folder") }
        BrowseMode.MultiFile ->
          Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = {
              onPick(dir.walkTopDown().filter { it.isFile && filter(it) }.toList())
            }) { Text("Whole folder") }
            Button(enabled = checked.isNotEmpty(), onClick = { onPick(checked.toList()) }) {
              Text("Install ${checked.size}")
            }
          }
        BrowseMode.File -> {}
      }
    },
    dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    modifier = Modifier.fillMaxSize().padding(vertical = 24.dp),
  )
}
