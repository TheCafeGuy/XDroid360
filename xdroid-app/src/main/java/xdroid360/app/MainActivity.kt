package xdroid360.app

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.clickable
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.foundation.Image
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.unit.dp
import java.io.File
import xdroid360.core.Bridge

private enum class Tab(val label: String, val icon: String) {
  Games("Games", "🎮"),
  Settings("Settings", "⚙"),
  Content("DLC", "📦"),
  Driver("Driver", "🧩"),
  About("About", "ℹ"),
}

private data class GameEntry(val file: File, val name: String, val size: Long)

class MainActivity : ComponentActivity() {
  private var status by mutableStateOf("")
  private var tab by mutableStateOf(Tab.Games)
  private val games = mutableStateListOf<GameEntry>()
  private val store by lazy { ConfigStore(File(filesDir, "xdroid").apply { mkdirs() }) }
  private val covers by lazy { CoverStore(File(filesDir, "xdroid/covers")) }
  private var coverRevision by mutableStateOf(0)
  @Volatile private var coverScanRunning = false

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    if (!DeviceSupport.cpuSupported) {
      // Nothing here may touch Bridge: loading the library would crash.
      setContent { UnsupportedScreen() }
      return
    }
    // Before any native logging init, which truncates xdroid.log: a session
    // that died (crash, kill) must keep its log even if no game boots again.
    SessionReports.recoverDeadSession(this)?.let { status = it }
    ensureStorageAccess()
    firstRunDefaults()
    // An install the app was killed during leaves its staging copy (and maybe
    // a replaced one) behind; queued behind any install still running.
    installExecutor.execute {
      File(filesDir, "xdroid/content/.staging").deleteRecursively()
      File(filesDir, "xdroid/content/.old").deleteRecursively()
    }
    askSoc = !DeviceSupport.qualcommSoc && !prefs().getBoolean("soc_warned", false)
    askCovers = !prefs().contains("download_covers")
    rescanGames()
    setContent { App() }
    // Unattended runs (debug builds): `am start ... --es autostart <path>`
    // boots that game; adb cannot start the non-exported GameActivity itself.
    val autostart = intent.getStringExtra("autostart")
    if (savedInstanceState == null && autostart != null &&
        (applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
      File(autostart).takeIf { it.isFile }?.let { launch(it) }
    }
  }

  override fun onResume() {
    super.onResume()
    if (!DeviceSupport.cpuSupported) return
    rescanGames()
  }

  // ---------------------------------------------------------------- App shell

  @Composable
  private fun App() {
    MaterialTheme(colorScheme = XdColors) {
      Scaffold(
        bottomBar = {
          NavigationBar(containerColor = MaterialTheme.colorScheme.surfaceContainer) {
            Tab.entries.forEach { t ->
              NavigationBarItem(
                selected = tab == t,
                onClick = { tab = t },
                icon = { Text(t.icon) },
                label = { Text(t.label) },
              )
            }
          }
        },
      ) { padding ->
        Surface(modifier = Modifier.fillMaxSize().padding(padding)) {
          Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
            if (status.isNotEmpty()) {
              Text(
                status,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.secondary,
                modifier = Modifier.padding(top = 8.dp).clickable { status = "" },
              )
            }
            when (tab) {
              Tab.Games -> GamesTab()
              Tab.Settings -> SettingsTab()
              Tab.Content -> ContentTab()
              Tab.Driver -> DriverTab()
              Tab.About -> AboutTab()
            }
          }
        }
      }
      if (askSoc) SocWarningDialog() else if (askCovers) CoverConsentDialog()
    }
  }

  @Composable
  private fun UnsupportedScreen() {
    MaterialTheme(colorScheme = XdColors) {
      Surface(modifier = Modifier.fillMaxSize()) {
        Column(Modifier.padding(24.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
          Text("XDroid360", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
          Text("This device's CPU is too old for XDroid360.")
          Text("Supported: ${DeviceSupport.SUPPORTED_TEXT}", color = Color.Gray)
        }
      }
    }
  }

  private var askSoc by mutableStateOf(false)

  @Composable
  private fun SocWarningDialog() {
    AlertDialog(
      onDismissRequest = {},
      title = { Text("Unsupported device") },
      text = {
        Text(
          "XDroid360 targets Qualcomm Snapdragon phones. On this device games may " +
            "not start or may render incorrectly.\n\nSupported: ${DeviceSupport.SUPPORTED_TEXT}"
        )
      },
      confirmButton = {
        TextButton(onClick = {
          prefs().edit().putBoolean("soc_warned", true).apply()
          askSoc = false
        }) { Text("Continue anyway") }
      },
    )
  }

  // Box art lookups send title IDs off the device, so they wait for a yes.
  private var askCovers by mutableStateOf(false)

  @Composable
  private fun CoverConsentDialog() {
    val answer = { yes: Boolean ->
      prefs().edit().putBoolean("download_covers", yes).apply()
      askCovers = false
      if (yes) rescanGames()
    }
    AlertDialog(
      onDismissRequest = {},
      title = { Text("Download box art?") },
      text = {
        Text(
          "Covers come from Xbox's image servers. Each game's title ID is sent " +
            "over an unencrypted connection. Without it, the library shows each " +
            "game's own icon. You can change this later in About."
        )
      },
      confirmButton = { TextButton(onClick = { answer(true) }) { Text("Download") } },
      dismissButton = { TextButton(onClick = { answer(false) }) { Text("Icons only") } },
    )
  }

  // ------------------------------------------------------------------- Games

  @OptIn(ExperimentalFoundationApi::class)
  @Composable
  private fun GamesTab() {
    var query by remember { mutableStateOf("") }
    var sortRecent by remember { mutableStateOf(true) }
    var pickFolder by remember { mutableStateOf(false) }
    var menuFor by remember { mutableStateOf<GameEntry?>(null) }

    Row(
      verticalAlignment = Alignment.CenterVertically,
      modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
    ) {
      Column {
        Text("XDroid360", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
        Text("Made with ♥ by TheCafeGuy", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
      }
      Spacer(Modifier.weight(1f))
      OutlinedButton(onClick = { pickFolder = true }) { Text("+ Folder") }
    }
    OutlinedTextField(
      value = query,
      onValueChange = { query = it },
      placeholder = { Text("Search games") },
      singleLine = true,
      modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
    )
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
      FilterChip(selected = sortRecent, onClick = { sortRecent = true }, label = { Text("Recent") })
      FilterChip(selected = !sortRecent, onClick = { sortRecent = false }, label = { Text("A-Z") })
    }
    val shown =
      games
        .filter { query.isBlank() || it.name.contains(query, ignoreCase = true) }
        .sortedWith(
          if (sortRecent) compareByDescending<GameEntry> { lastPlayed(it.file) }.thenBy { it.name.lowercase() }
          else compareBy { it.name.lowercase() },
        )
    if (shown.isEmpty()) {
      Column(Modifier.fillMaxWidth().padding(32.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Text("No games found", style = MaterialTheme.typography.titleMedium)
        Text(
          "Add the folder that holds your .iso / .xex / .zar files.",
          color = Color.Gray,
          modifier = Modifier.padding(8.dp),
        )
        Button(onClick = { pickFolder = true }) { Text("Add games folder") }
      }
    }
    LazyVerticalGrid(
      columns = GridCells.Adaptive(150.dp),
      contentPadding = PaddingValues(vertical = 12.dp),
      horizontalArrangement = Arrangement.spacedBy(12.dp),
      verticalArrangement = Arrangement.spacedBy(12.dp),
      modifier = Modifier.fillMaxSize(),
    ) {
      items(shown, key = { it.file.absolutePath }) { game ->
        Card(
          colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
          modifier = Modifier.combinedClickable(
            onClick = { launch(game.file) },
            onLongClick = { menuFor = game },
          ),
        ) {
          Column(Modifier.padding(10.dp)) {
            @Suppress("UNUSED_VARIABLE") val coverRev = coverRevision
            val cover = titleIdOf(game.file)?.let { covers.bitmap(it) }
            if (cover != null) {
              Image(
                bitmap = cover.asImageBitmap(),
                contentDescription = game.name,
                contentScale = ContentScale.Crop,
                modifier = Modifier.fillMaxWidth().aspectRatio(0.73f).clip(RoundedCornerShape(12.dp)),
              )
            } else {
              LetterTile(game.name, Modifier.fillMaxWidth().aspectRatio(0.73f))
            }
            Text(
              game.name,
              maxLines = 2,
              overflow = TextOverflow.Ellipsis,
              style = MaterialTheme.typography.bodyMedium,
              fontWeight = FontWeight.Medium,
              modifier = Modifier.padding(top = 8.dp),
            )
            val title = titleIdOf(game.file)
            Text(
              listOfNotNull(game.file.extension.uppercase(), formatSize(game.size), title).joinToString(" · "),
              style = MaterialTheme.typography.bodySmall,
              color = Color.Gray,
              maxLines = 1,
            )
          }
        }
      }
    }

    if (pickFolder) {
      FileBrowserDialog(
        "Choose a games folder", File("/sdcard"), BrowseMode.Folder, { false },
        onDismiss = { pickFolder = false },
        onPick = {
          addGameDir(it.first())
          pickFolder = false
        },
      )
    }
    menuFor?.let { game ->
      val title = titleIdOf(game.file)
      AlertDialog(
        onDismissRequest = { menuFor = null },
        title = { Text(game.name) },
        text = {
          Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(game.file.absolutePath, style = MaterialTheme.typography.bodySmall, color = Color.Gray)
            if (title == null) {
              Text("Play it once to unlock per-game settings and DLC info.", color = Color.Gray)
            }
            Button(modifier = Modifier.fillMaxWidth(), onClick = { menuFor = null; launch(game.file) }) { Text("Play") }
            OutlinedButton(
              enabled = title != null,
              modifier = Modifier.fillMaxWidth(),
              onClick = { menuFor = null; settingsScope = title; tab = Tab.Settings },
            ) { Text("Game settings") }
            OutlinedButton(
              modifier = Modifier.fillMaxWidth(),
              onClick = { menuFor = null; tab = Tab.Content },
            ) { Text("Install DLC / updates") }
          }
        },
        confirmButton = { TextButton(onClick = { menuFor = null }) { Text("Close") } },
      )
    }
  }

  private fun launch(file: File) {
    prefs().edit().putLong("played_${file.absolutePath}", System.currentTimeMillis()).apply()
    startActivity(Intent(this, GameActivity::class.java).putExtra(GameActivity.EXTRA_GAME_PATH, file.absolutePath))
  }

  // ---------------------------------------------------------------- Settings

  private var settingsScope by mutableStateOf<String?>(null)
  private var settingsRevision by mutableStateOf(0)

  @Composable
  private fun SettingsTab() {
    var category by remember { mutableStateOf<SettingCategory?>(null) }
    var scopeMenu by remember { mutableStateOf(false) }
    val knownGames = knownTitles()

    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 12.dp)) {
      if (category != null) {
        TextButton(onClick = { category = null }) { Text("‹ Back") }
      }
      Text(
        category?.title ?: "Settings",
        style = MaterialTheme.typography.headlineSmall,
        fontWeight = FontWeight.Bold,
      )
    }
    Box {
      OutlinedButton(onClick = { scopeMenu = true }, modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
        Text(
          settingsScope?.let { "Per-game: ${knownGames[it] ?: it} ($it)" } ?: "Global settings (all games)",
        )
      }
      DropdownMenu(expanded = scopeMenu, onDismissRequest = { scopeMenu = false }) {
        DropdownMenuItem(text = { Text("Global settings") }, onClick = { settingsScope = null; scopeMenu = false })
        knownGames.forEach { (id, name) ->
          DropdownMenuItem(text = { Text("$name ($id)") }, onClick = { settingsScope = id; scopeMenu = false })
        }
      }
    }
    if (settingsScope != null) {
      Text(
        "Per-game values override the global ones. ↺ returns a setting to its global value.",
        style = MaterialTheme.typography.bodySmall,
        color = Color.Gray,
      )
    }
    val current = category
    if (current == null) {
      LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp), contentPadding = PaddingValues(vertical = 8.dp)) {
        items(SettingsCatalog.categories) { cat ->
          Card(
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
            modifier = Modifier.fillMaxWidth().clickable { category = cat },
          ) {
            Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
              Text(cat.icon, style = MaterialTheme.typography.headlineSmall)
              Spacer(Modifier.width(16.dp))
              Column(Modifier.weight(1f)) {
                Text(cat.title, style = MaterialTheme.typography.titleMedium)
                Text(
                  cat.settings.joinToString(", ") { it.title },
                  style = MaterialTheme.typography.bodySmall,
                  color = Color.Gray,
                  maxLines = 1,
                  overflow = TextOverflow.Ellipsis,
                )
              }
              Text("›", style = MaterialTheme.typography.headlineSmall)
            }
          }
        }
      }
    } else {
      LazyColumn(contentPadding = PaddingValues(vertical = 8.dp)) {
        items(current.settings, key = { it.key }) { def -> SettingRow(def) }
      }
    }
  }

  @Composable
  private fun SettingRow(def: SettingDef) {
    @Suppress("UNUSED_VARIABLE") val rev = settingsRevision
    val scope = settingsScope
    val gameValue = scope?.let { store.read(store.gameFile(it), def.key) }
    val globalValue = store.read(store.globalFile, def.key)
    val raw = gameValue ?: globalValue ?: defaultRaw(def)
    val overridden = scope != null && gameValue != null
    var menu by remember { mutableStateOf(false) }

    Card(
      colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
      modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
    ) {
      Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
          Column(Modifier.weight(1f)) {
            Text(def.title, style = MaterialTheme.typography.titleSmall)
            Text(def.description, style = MaterialTheme.typography.bodySmall, color = Color.Gray)
            if (scope != null) {
              Text(
                if (overridden) "Overridden for this game" else "Using global value",
                style = MaterialTheme.typography.labelSmall,
                color = if (overridden) MaterialTheme.colorScheme.primary else Color.Gray,
              )
            }
          }
          if (overridden) {
            TextButton(onClick = {
              store.remove(store.gameFile(scope!!), def.key)
              if (def.key == "draw_resolution_scale_x") store.remove(store.gameFile(scope), "draw_resolution_scale_y")
              settingsRevision++
            }) { Text("↺") }
          }
          when (def) {
            is SettingDef.Toggle ->
              Switch(checked = raw == "true", onCheckedChange = { saveSetting(def, it.toString()) })
            is SettingDef.Choice ->
              Box {
                OutlinedButton(onClick = { menu = true }) {
                  Text(def.options.firstOrNull { it.first == raw }?.second ?: ConfigStore.unquote(raw))
                }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                  def.options.forEach { (value, label) ->
                    DropdownMenuItem(text = { Text(label) }, onClick = { menu = false; saveSetting(def, value) })
                  }
                }
              }
            is SettingDef.Slider -> Text(def.format(raw.toFloatOrNull() ?: def.default))
          }
        }
        if (def is SettingDef.Slider) {
          var value by remember(raw) { mutableStateOf(raw.toFloatOrNull() ?: def.default) }
          Slider(
            value = value,
            onValueChange = { value = it },
            onValueChangeFinished = {
              saveSetting(def, if (def.isInteger) value.toInt().toString() else String.format(java.util.Locale.US, "%.2f", value))
            },
            valueRange = def.min..def.max,
            steps = def.steps,
          )
        }
      }
    }
  }

  private fun defaultRaw(def: SettingDef): String =
    when (def) {
      is SettingDef.Toggle -> def.default.toString()
      is SettingDef.Choice -> def.default
      is SettingDef.Slider -> if (def.isInteger) def.default.toInt().toString() else def.default.toString()
    }

  private fun saveSetting(def: SettingDef, raw: String) {
    val file = settingsScope?.let { store.gameFile(it) } ?: store.globalFile
    try {
      store.write(file, def.section, def.key, raw)
      // The resolution scale applies to both axes.
      if (def.key == "draw_resolution_scale_x") store.write(file, def.section, "draw_resolution_scale_y", raw)
      status = "${def.title}: saved (applies at next game start)"
    } catch (e: Exception) {
      status = "Could not save ${def.title}: ${e.message}"
    }
    settingsRevision++
  }

  // ------------------------------------------------------------- DLC content

  private val installLog = mutableStateListOf<String>()
  private var installing by mutableStateOf(false)

  private companion object {
    // Process-wide, so an activity recreated mid-install queues behind it
    // instead of sharing the native staging folder.
    val installExecutor: java.util.concurrent.ExecutorService =
      java.util.concurrent.Executors.newSingleThreadExecutor()
  }
  private var installProgress by mutableStateOf(0f)

  // ---------------------------------------------------- Saves and settings

  private val dataRoot get() = File(filesDir, "xdroid")
  private var exportDialog by mutableStateOf(false)
  private var pendingExport: Pair<Set<String>, Boolean>? = null
  private var importUri by mutableStateOf<Uri?>(null)
  private var importContents by mutableStateOf<Backup.Contents?>(null)

  private val exportLauncher =
    registerForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
      val (titles, settings) = pendingExport ?: return@registerForActivityResult
      pendingExport = null
      if (uri == null) return@registerForActivityResult
      installExecutor.execute {
        val msg =
          try {
            contentResolver.openOutputStream(uri)?.use { Backup.export(dataRoot, prefs(), it, titles, settings) }
            "Backup saved"
          } catch (e: Exception) {
            "Backup failed: ${e.message}"
          }
        runOnUiThread { status = msg }
      }
    }

  private val importLauncher =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
      if (uri == null) return@registerForActivityResult
      installExecutor.execute {
        val contents =
          try {
            contentResolver.openInputStream(uri)?.use { Backup.inspect(it) }
          } catch (e: Exception) {
            null
          }
        runOnUiThread {
          if (contents == null) {
            status = "Not an XDroid360 backup"
          } else {
            importUri = uri
            importContents = contents
          }
        }
      }
    }

  private fun runImport(replace: Boolean) {
    val uri = importUri ?: return
    importUri = null
    importContents = null
    installExecutor.execute {
      val msg =
        try {
          val n = contentResolver.openInputStream(uri)?.use { Backup.import(dataRoot, prefs(), it, replace) } ?: 0
          "Imported $n files"
        } catch (e: Exception) {
          "Import failed: ${e.message}"
        }
      runOnUiThread {
        status = msg
        settingsRevision++
      }
    }
  }

  @Composable
  private fun BackupCard() {
    Card(
      colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
      modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
    ) {
      Column(Modifier.padding(12.dp)) {
        Text("Saves & settings", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
        Text(
          "Back up game saves, emulator settings and controls to a .zip, or restore one.",
          style = MaterialTheme.typography.bodySmall,
          color = Color.Gray,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.padding(top = 8.dp)) {
          OutlinedButton(onClick = { exportDialog = true }) { Text("Export…") }
          OutlinedButton(onClick = { importLauncher.launch(arrayOf("application/zip", "application/octet-stream")) }) {
            Text("Import…")
          }
        }
      }
    }
    if (exportDialog) ExportDialog()
    importContents?.let { ImportDialog(it) }
  }

  @Composable
  private fun ExportDialog() {
    val saved = remember { Backup.savedTitles(dataRoot) }
    val chosen = remember { mutableStateListOf<String>().apply { addAll(saved) } }
    var settings by remember { mutableStateOf(true) }
    val names = knownTitles()
    AlertDialog(
      onDismissRequest = { exportDialog = false },
      title = { Text("Export") },
      text = {
        Column {
          CheckRow("Settings and controls", settings) { settings = it }
          if (saved.isEmpty()) Text("No game saves yet.", color = Color.Gray)
          for (t in saved) {
            CheckRow("Saves: ${names[t] ?: t}", t in chosen) { if (it) chosen.add(t) else chosen.remove(t) }
          }
        }
      },
      confirmButton = {
        TextButton(
          enabled = settings || chosen.isNotEmpty(),
          onClick = {
            exportDialog = false
            pendingExport = chosen.toSet() to settings
            val day = java.time.LocalDate.now()
            exportLauncher.launch("XDroid360-backup-$day.zip")
          },
        ) { Text("Export") }
      },
      dismissButton = { TextButton(onClick = { exportDialog = false }) { Text("Cancel") } },
    )
  }

  @Composable
  private fun ImportDialog(c: Backup.Contents) {
    val names = knownTitles()
    val parts = c.titles.map { "saves for ${names[it] ?: it}" } + if (c.settings) listOf("settings and controls") else emptyList()
    val dismiss = {
      importUri = null
      importContents = null
    }
    AlertDialog(
      onDismissRequest = dismiss,
      title = { Text("Import backup") },
      text = {
        Text(
          "This backup has " + parts.joinToString(", ").ifEmpty { "nothing" } + "." +
            if (c.titles.isNotEmpty()) {
              "\n\nMerge keeps your other save files and overwrites the ones in the backup. " +
                "Replace deletes the current saves of those games first."
            } else {
              ""
            },
        )
      },
      confirmButton = {
        if (c.titles.isNotEmpty()) {
          Row {
            TextButton(onClick = { runImport(replace = true) }) { Text("Replace") }
            TextButton(onClick = { runImport(replace = false) }) { Text("Merge") }
          }
        } else {
          TextButton(onClick = { runImport(replace = false) }) { Text("Import") }
        }
      },
      dismissButton = { TextButton(onClick = dismiss) { Text("Cancel") } },
    )
  }

  @Composable
  private fun CheckRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth().clickable { onChange(!checked) }) {
      Checkbox(checked = checked, onCheckedChange = onChange)
      Text(label)
    }
  }

  @Composable
  private fun ContentTab() {
    var picker by remember { mutableStateOf(false) }
    @Suppress("UNUSED_VARIABLE") val rev = settingsRevision
    Text(
      "DLC & updates",
      style = MaterialTheme.typography.headlineSmall,
      fontWeight = FontWeight.Bold,
      modifier = Modifier.padding(top = 12.dp),
    )
    Text(
      "Install Xbox 360 content packages (DLC, title updates, Arcade unlocks). Pick several files " +
        "at once, or a whole folder: every package inside is installed in one go.",
      style = MaterialTheme.typography.bodySmall,
      color = Color.Gray,
    )
    Button(
      enabled = !installing,
      onClick = { picker = true },
      modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
    ) { Text(if (installing) "Installing…" else "Install packages") }
    if (installing) {
      LinearProgressIndicator(progress = { installProgress }, modifier = Modifier.fillMaxWidth())
    }
    BackupCard()
    LazyColumn(modifier = Modifier.fillMaxSize()) {
      if (installLog.isNotEmpty()) {
        item { SectionTitle("Last install") }
        items(installLog) { line ->
          Text(
            line,
            style = MaterialTheme.typography.bodySmall,
            color = if (line.startsWith("✗")) Color(0xFFFF8A80) else Color(0xFFB9F6CA),
            modifier = Modifier.padding(vertical = 2.dp),
          )
        }
      }
      val installed = installedContent()
      item { SectionTitle("Installed (${installed.size})") }
      if (installed.isEmpty()) {
        item { Text("Nothing installed yet.", color = Color.Gray) }
      }
      items(installed) { (title, type, name) ->
        Card(
          colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
          modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp),
        ) {
          Column(Modifier.padding(12.dp)) {
            Text(name, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text("${knownTitles()[title] ?: title} · $type", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
          }
        }
      }
    }
    if (picker) {
      FileBrowserDialog(
        "Choose content packages",
        savedDir("last_dlc_dir"),
        BrowseMode.MultiFile,
        // Packages usually have no extension; skip obvious non-packages.
        { f -> f.length() >= 0xB000 && f.extension.lowercase() !in setOf("iso", "xex", "zip", "txt", "jpg", "png", "zar") },
        onDismiss = { picker = false },
        onPick = { files ->
          picker = false
          files.firstOrNull()?.parentFile?.let { prefs().edit().putString("last_dlc_dir", it.absolutePath).apply() }
          installPackages(files)
        },
      )
    }
  }

  private fun installPackages(files: List<File>) {
    if (files.isEmpty()) return
    installing = true
    installProgress = 0f
    installLog.clear()
    val root = File(filesDir, "xdroid").absolutePath
    installExecutor.execute {
      var ok = 0
      files.forEachIndexed { i, f ->
        val result = Bridge.installContent(root, f.absolutePath)
        val parts = result.split("|")
        val line =
          if (parts.firstOrNull() == "OK" && parts.size >= 4) {
            ok++
            prefs().edit().putString("content_name_${parts[2]}_${f.name}", parts[3]).apply()
            "✓ ${parts[3]} (${parts[1]}, ${parts[2]})"
          } else {
            "✗ ${f.name}: ${parts.getOrNull(1) ?: result}"
          }
        runOnUiThread {
          installLog.add(line)
          installProgress = (i + 1) / files.size.toFloat()
        }
      }
      runOnUiThread {
        installing = false
        status = "Installed $ok of ${files.size} packages"
        settingsRevision++
      }
    }
  }

  /** (title id, type, display name) for every installed package folder. */
  private fun installedContent(): List<Triple<String, String, String>> {
    val content = File(filesDir, "xdroid/content")
    val out = mutableListOf<Triple<String, String, String>>()
    // ".staging" holds an install in progress (or one cut short): not content.
    content.listFiles()?.filter { it.isDirectory && !it.name.startsWith(".") }?.forEach { xuid ->
      xuid.listFiles()?.filter { it.isDirectory }?.forEach { title ->
        title.listFiles()?.filter { it.isDirectory && it.name != "Headers" }?.forEach { type ->
          val typeName =
            when (type.name.uppercase()) {
              "00000002" -> "DLC"
              "000B0000" -> "Title update"
              "00000001" -> "Save"
              "000D0000" -> "Arcade"
              else -> type.name
            }
          if (type.name == "00000001") return@forEach
          type.listFiles()?.filter { it.isDirectory }?.forEach { pkg ->
            val name = prefs().getString("content_name_${title.name}_${pkg.name}", null) ?: pkg.name
            out.add(Triple(title.name.uppercase(), typeName, name))
          }
        }
      }
    }
    return out.sortedWith(compareBy({ it.first }, { it.third }))
  }

  // ------------------------------------------------------------------ Driver

  @Composable
  private fun DriverTab() {
    var picker by remember { mutableStateOf(false) }
    var label by remember { mutableStateOf(driverName().ifEmpty { "System driver" }) }
    Text("GPU driver", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold, modifier = Modifier.padding(top = 12.dp))
    Card(
      colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
      modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
    ) {
      Column(Modifier.padding(16.dp)) {
        Text("In use", style = MaterialTheme.typography.labelMedium, color = Color.Gray)
        Text(label, style = MaterialTheme.typography.titleMedium)
        Text(
          "Turnip (Mesa) driver packages (.zip) replace the system Vulkan driver.",
          style = MaterialTheme.typography.bodySmall,
          color = Color.Gray,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.padding(top = 8.dp)) {
          Button(onClick = { picker = true }) { Text("Install .zip") }
          if (driverName().isNotEmpty()) {
            OutlinedButton(onClick = { clearDriver(); label = "System driver" }) { Text("Use system") }
          }
        }
      }
    }
    if (picker) {
      FileBrowserDialog(
        "Choose a driver .zip", savedDir("last_dir"), BrowseMode.File, { it.extension.lowercase() == "zip" },
        onDismiss = { picker = false },
        onPick = { files ->
          picker = false
          files.first().parentFile?.let { saveDir("last_dir", it) }
          installDriver(files.first()) { ok -> if (ok) label = driverName() }
        },
      )
    }
  }

  // ------------------------------------------------------------------- About

  @Composable
  private fun AboutTab() {
    val crashes = SessionReports.logsDir(this).listFiles()?.count { it.name.startsWith("crash-") && it.name.endsWith(".txt") } ?: 0
    LazyColumn(contentPadding = PaddingValues(vertical = 12.dp)) {
      item {
        Text("XDroid360", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
        Text(Bridge.version(), style = MaterialTheme.typography.bodySmall, color = Color.Gray)
        SectionTitle("Device")
        Text("${Build.MANUFACTURER} ${Build.MODEL} · Android ${Build.VERSION.RELEASE} · ${Build.HARDWARE}")
        Text("Supported: ${DeviceSupport.SUPPORTED_TEXT}", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
        SectionTitle("Diagnostics")
        Text("Saved crash reports: $crashes")
        Text("Driver: ${driverName().ifEmpty { "system" }}")
        SectionTitle("Library")
        var download by remember { mutableStateOf(prefs().getBoolean("download_covers", false)) }
        Row(verticalAlignment = Alignment.CenterVertically) {
          Column(Modifier.weight(1f)) {
            Text("Download box art")
            Text(
              "Fetches covers by title ID from Xbox's image servers. Off: game icons only.",
              style = MaterialTheme.typography.bodySmall,
              color = Color.Gray,
            )
          }
          Switch(checked = download, onCheckedChange = {
            download = it
            prefs().edit().putBoolean("download_covers", it).apply()
            if (it) rescanGames()
          })
        }
        SectionTitle("In game")
        Text("Back button or Select + Start: quick menu (stats, fast forward, AMOLED tone, exit).")
        Text("PS / Home button: fast forward on/off.")
        Spacer(Modifier.height(16.dp))
        Text(
          "Unofficial Android port based on xenia-edge. No games, keys or firmware are included.",
          style = MaterialTheme.typography.bodySmall,
          color = Color.Gray,
        )
        var showLicenses by remember { mutableStateOf(false) }
        TextButton(onClick = { showLicenses = true }) { Text("Open-source licenses") }
        if (showLicenses) LicensesDialog { showLicenses = false }
      }
    }
  }

  // ----------------------------------------------------------------- Helpers

  private fun prefs() = getSharedPreferences("xdroid", MODE_PRIVATE)

  private fun lastPlayed(f: File): Long = prefs().getLong("played_${f.absolutePath}", 0L)

  private fun titleIdOf(f: File): String? = prefs().getString("title_${f.absolutePath}", null)

  /** Title id -> game name, for games that have been played at least once. */
  private fun knownTitles(): Map<String, String> {
    val out = sortedMapOf<String, String>()
    prefs().all.forEach { (k, v) ->
      if (k.startsWith("title_") && v is String) {
        out[v] = File(k.removePrefix("title_")).nameWithoutExtension
      }
    }
    return out
  }

  private fun gameDirs(): Set<String> {
    val dirs = prefs().getStringSet("game_dirs", null)?.toMutableSet() ?: mutableSetOf()
    if (dirs.isEmpty()) {
      prefs().getString("last_game_dir", null)?.let { dirs.add(it) }
    }
    return dirs
  }

  private fun addGameDir(dir: File) {
    val dirs = gameDirs().toMutableSet().apply { add(dir.absolutePath) }
    prefs().edit().putStringSet("game_dirs", dirs).apply()
    rescanGames()
    status = "Added ${dir.absolutePath}"
  }

  // Scans run off the UI thread (slow on large or removable storage); only the
  // newest scan's result is applied.
  private var scanGeneration = 0

  private fun rescanGames() {
    val generation = ++scanGeneration
    val dirs = gameDirs()
    Thread {
      val found = mutableListOf<GameEntry>()
      val seen = HashSet<String>()
      for (path in dirs) {
        File(path).walkTopDown().maxDepth(3).forEach { f ->
          if (f.isFile && f.extension.lowercase() in setOf("iso", "xex", "zar") && seen.add(f.absolutePath)) {
            // A default.xex names its game by the folder.
            val name = if (f.name.equals("default.xex", true)) f.parentFile?.name ?: f.nameWithoutExtension else f.nameWithoutExtension
            found.add(GameEntry(f, name, f.length()))
          }
        }
      }
      runOnUiThread {
        if (generation != scanGeneration || isDestroyed) return@runOnUiThread
        games.clear()
        games.addAll(found)
        resolveCovers(found)
      }
    }.start()
  }

  /**
   * Off the UI thread: reads missing title IDs from the game files, then
   * downloads missing box art (if enabled). Titles without box art on the
   * server are retried after a week.
   */
  private fun resolveCovers(list: List<GameEntry>) {
    if (coverScanRunning) return
    coverScanRunning = true
    Thread {
      try {
        val download = prefs().getBoolean("download_covers", false)
        for (game in list) {
          var id = titleIdOf(game.file)
          if (id == null) {
            val probed = Bridge.probeTitleId(game.file.absolutePath)
            if (probed != 0L) {
              id = String.format(java.util.Locale.US, "%08X", probed)
              prefs().edit().putString("title_${game.file.absolutePath}", id).apply()
              runOnUiThread { coverRevision++ }
            }
          }
          if (id == null || !download || covers.hasBoxArt(id)) continue
          val missKey = "cover_miss_$id"
          if (System.currentTimeMillis() - prefs().getLong(missKey, 0L) < 7L * 24 * 3600 * 1000) continue
          when (covers.downloadBoxArt(id)) {
            true -> runOnUiThread { coverRevision++ }
            false -> prefs().edit().putLong(missKey, System.currentTimeMillis()).apply()
            null -> break // Offline: try again on the next scan.
          }
        }
      } finally {
        coverScanRunning = false
      }
    }.start()
  }

  private fun savedDir(key: String): File {
    val saved = prefs().getString(key, null)?.let { File(it) }
    return if (saved != null && saved.isDirectory && saved.canRead()) saved else File("/sdcard")
  }

  private fun saveDir(key: String, dir: File) {
    prefs().edit().putString(key, dir.absolutePath).apply()
  }

  private fun driverName(): String = prefs().getString("driver_name", "").orEmpty()

  /** One-time config changes, applied once each, in order. */
  private fun firstRunDefaults() {
    if (!prefs().getBoolean("defaults_v1", false)) {
      try {
        // Full-screen fill, as native code used to force before the settings
        // UI owned it.
        store.write(store.globalFile, "Display", "present_letterbox", "false")
        prefs().edit().putBoolean("defaults_v1", true).apply()
      } catch (_: Exception) {
      }
    }
    if (prefs().getBoolean("defaults_v2", false)) return
    try {
      // Keys whose native code is gone.
      store.remove(store.globalFile, "vulkan_serialize_frame_close")
      store.remove(store.globalFile, "xdroid_big_core_affinity")
      // Native code used to force this off whatever the file said (the
      // emulator writes every key, so the file shows true). It is a setting
      // now: carry the tested behavior over once.
      store.write(store.globalFile, "GPU", "submit_on_primary_buffer_end", "false")
      prefs().edit().putBoolean("defaults_v2", true).apply()
    } catch (_: Exception) {
    }
  }

  private fun ensureStorageAccess(): Boolean {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
      status = "Grant All-files access to browse your games"
      startActivity(
        Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:$packageName")),
      )
      return false
    }
    return true
  }

  /**
   * Installs a Turnip driver zip into app-private storage. Extracts to a
   * staging folder first, so a bad zip leaves the current driver in place.
   */
  private fun installDriver(zip: File, done: (Boolean) -> Unit) {
    status = "Installing driver…"
    Thread {
      val root = File(filesDir, "xdroid/driver")
      val staging = File(filesDir, "xdroid/driver.staging")
      val result = try {
        staging.deleteRecursively()
        staging.mkdirs()
        java.util.zip.ZipFile(zip).use { zf ->
          for (e in zf.entries()) {
            if (e.isDirectory || !e.name.endsWith(".so")) continue
            // Flatten paths: entry names never escape the staging folder.
            val out = File(staging, File(e.name).name)
            zf.getInputStream(e).use { inp -> out.outputStream().use { inp.copyTo(it) } }
          }
        }
        val libs = staging.listFiles().orEmpty()
        val lib = libs.firstOrNull { it.name.contains("vulkan") && it.name.contains("freedreno") }
          ?: libs.firstOrNull { it.name.contains("freedreno") }
          ?: libs.firstOrNull()
        if (lib == null) {
          "No .so inside the zip"
        } else {
          root.deleteRecursively()
          if (!staging.renameTo(root)) throw java.io.IOException("cannot move driver into place")
          prefs().edit()
            .putString("driver_path", File(root, lib.name).absolutePath)
            .putString("driver_name", zip.name)
            .apply()
          null
        }
      } catch (e: Exception) {
        "Driver install failed: ${e.message}"
      } finally {
        staging.deleteRecursively()
      }
      runOnUiThread {
        status = result ?: "Driver: ${zip.name}"
        done(result == null)
      }
    }.start()
  }

  private fun clearDriver() {
    File(filesDir, "xdroid/driver").deleteRecursively()
    prefs().edit().remove("driver_path").remove("driver_name").apply()
    status = "Driver: system"
  }
}
