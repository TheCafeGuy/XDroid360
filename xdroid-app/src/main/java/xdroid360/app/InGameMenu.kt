package xdroid360.app

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Live state shared between GameActivity's samplers and the overlay. */
class InGameState {
  var menuOpen by mutableStateOf(false)
  var tab by mutableStateOf(0)
  var showStats by mutableStateOf(true)
  var detailedStats by mutableStateOf(false)
  var fastForward by mutableStateOf(false)
  var toneStrength by mutableStateOf(0f)
  var fps by mutableStateOf("")
  var cpu by mutableStateOf("")
  var gpu by mutableStateOf("")
  var thermal by mutableStateOf("")
  var detail by mutableStateOf("")
  // Menu header.
  var gameName by mutableStateOf("")
  var titleId by mutableStateOf("")
  var playSeconds by mutableStateOf(0L)
  var battery by mutableStateOf(-1)
  var version by mutableStateOf("")
  // Controls.
  var gamepadConnected by mutableStateOf(false)
  var touchEditing by mutableStateOf(false)

  companion object {
    const val TABS = 4
  }
}

/** What the overlay needs from GameActivity. */
interface InGameHost : ControlsHost {
  val settings: ControlSettings
  val remap: RemapState
  val editorState: EditorState
  val editor: EditorActions

  fun setFastForward(on: Boolean)

  fun setTone(strength: Float)

  fun markGlitch()

  /** Asks before leaving the game. */
  fun quit()

  fun persist()

  fun menuChanged(open: Boolean)

  fun applyPad(visible: Boolean, editing: Boolean)
}

@Composable
fun InGameOverlay(state: InGameState, host: InGameHost) {
  MaterialTheme(colorScheme = XdColors) {
    val s = host.settings
    // The pad hides with a controller (auto mode) and under the menu; the
    // editor always shows it.
    val padVisible = state.touchEditing ||
      (s.show.value && !(s.autoHide.value && state.gamepadConnected) && !state.menuOpen)
    LaunchedEffect(padVisible, state.touchEditing) { host.applyPad(padVisible, state.touchEditing) }
    LaunchedEffect(state.menuOpen) { host.menuChanged(state.menuOpen) }
    Box(Modifier.fillMaxSize()) {
      if (state.touchEditing) TouchEditBar(host.editorState, host.editor, Modifier.align(Alignment.TopCenter))
      // Compact stats (top-left), tap to open the menu.
      if (state.showStats && !state.menuOpen && !state.touchEditing) {
        Column(
          modifier = Modifier
            .padding(start = 40.dp, top = 8.dp)
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0x99000000))
            .clickable { state.menuOpen = true }
            .padding(horizontal = 10.dp, vertical = 6.dp),
        ) {
          Text(
            listOf(state.fps, state.cpu, state.gpu, state.thermal)
              .filter { it.isNotEmpty() }.joinToString("  ·  ") +
              if (state.fastForward) "  ·  FF▶▶" else "",
            color = Color(0xFF7CFFB2),
            fontFamily = FontFamily.Monospace,
            fontSize = 12.sp,
          )
          if (state.detailedStats && state.detail.isNotEmpty()) {
            Text(state.detail, color = Color(0xFFCCCCCC), fontFamily = FontFamily.Monospace, fontSize = 10.sp)
          }
        }
      }
      // Glitch flag (top-center): tap reports a glitch, hold opens the menu.
      if (!state.menuOpen && !state.touchEditing) {
        Text(
          "⚑",
          color = Color(0x88FFFFFF),
          fontSize = 18.sp,
          modifier = Modifier
            .align(Alignment.TopCenter)
            .pointerInput(Unit) {
              detectTapGestures(
                onTap = { host.markGlitch() },
                onLongPress = { state.menuOpen = true },
              )
            }
            .padding(horizontal = 24.dp, vertical = 2.dp),
        )
      }
      // The game dims behind the panel; a tap outside closes it.
      AnimatedVisibility(visible = state.menuOpen, enter = fadeIn(tween(150)), exit = fadeOut(tween(150))) {
        Box(
          Modifier.fillMaxSize().background(Color(0x99000000)).pointerInput(Unit) {
            detectTapGestures { state.menuOpen = false }
          },
        )
      }
      AnimatedVisibility(
        visible = state.menuOpen,
        enter = slideInHorizontally(tween(150)) { -it },
        exit = slideOutHorizontally(tween(150)) { -it },
      ) {
        MenuPanel(state, host)
      }
    }
  }
}

@Composable
private fun MenuPanel(state: InGameState, host: InGameHost) {
  val tabFocus = remember { List(InGameState.TABS) { FocusRequester() } }
  // Controller navigation starts on the current tab.
  LaunchedEffect(state.tab) { runCatching { tabFocus[state.tab].requestFocus() } }
  Column(
    modifier = Modifier
      .fillMaxHeight()
      .widthIn(max = 460.dp)
      .fillMaxWidth(0.6f)
      .background(Color(0xF2101016))
      .pointerInput(Unit) { detectTapGestures { } }
      .padding(horizontal = 16.dp, vertical = 10.dp),
  ) {
    // Header: game, time played, FPS, heat, battery.
    Text(
      state.gameName.ifEmpty { stringResource(R.string.app_game) },
      style = MaterialTheme.typography.titleMedium,
      fontWeight = FontWeight.Bold,
      maxLines = 1,
      overflow = TextOverflow.Ellipsis,
    )
    val played = "%d:%02d".format(state.playSeconds / 60, state.playSeconds % 60)
    Text(
      listOfNotNull(
        "⏱ $played",
        state.fps.takeIf { it.isNotEmpty() },
        state.thermal.takeIf { it.isNotEmpty() },
        if (state.battery >= 0) "🔋 ${state.battery}%" else null,
      ).joinToString("  ·  "),
      style = MaterialTheme.typography.bodySmall,
      color = Color(0xFFB8B8C8),
      fontFamily = FontFamily.Monospace,
    )
    // Tabs: tap, or LB / RB on a controller.
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.padding(vertical = 8.dp)) {
      val names = listOf(R.string.tab_home, R.string.tab_controls, R.string.tab_graphics, R.string.tab_diag)
      names.forEachIndexed { i, n ->
        FilterChip(
          selected = state.tab == i,
          onClick = { state.tab = i },
          label = { Text(stringResource(n), fontSize = 12.sp) },
          modifier = Modifier.focusRequester(tabFocus[i]).heightIn(min = 40.dp),
        )
      }
    }
    Column(
      Modifier.weight(1f).verticalScroll(rememberScrollState()),
      verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
      when (state.tab) {
        0 -> HomeTab(state, host)
        1 -> ControlsSettingsPanel(host.settings, host, host.remap, state.gamepadConnected)
        2 -> GraphicsTab(state, host)
        else -> DiagnosticsTab(state, host)
      }
      Text(stringResource(R.string.menu_help), style = MaterialTheme.typography.labelSmall, color = Color.Gray)
    }
  }
}

private val BIG = Modifier.fillMaxWidth().heightIn(min = 48.dp)

@Composable
private fun HomeTab(state: InGameState, host: InGameHost) {
  val s = host.settings
  Button(onClick = { state.menuOpen = false }, modifier = BIG) { Text(stringResource(R.string.resume)) }
  Section(stringResource(R.string.sec_touch)) {
    SwitchRow(stringResource(R.string.opt_show), s.show.value) {
      s.show.value = it
      host.changed()
    }
    OutlinedButton(onClick = { host.editLayout() }, modifier = BIG) { Text(stringResource(R.string.edit_layout)) }
  }
  OutlinedButton(
    onClick = {
      host.markGlitch()
      state.menuOpen = false
    },
    modifier = BIG,
  ) { Text(stringResource(R.string.report_glitch)) }
  Button(onClick = { host.quit() }, modifier = BIG) { Text(stringResource(R.string.exit_game)) }
}

@Composable
private fun GraphicsTab(state: InGameState, host: InGameHost) {
  Section(stringResource(R.string.perf_title)) {
    Text(state.fps.ifEmpty { "FPS —" }, fontFamily = FontFamily.Monospace)
    Text(state.cpu.ifEmpty { "CPU —" }, fontFamily = FontFamily.Monospace)
    Text(state.gpu.ifEmpty { "GPU —" }, fontFamily = FontFamily.Monospace)
    Text(state.thermal.ifEmpty { "Thermal —" }, fontFamily = FontFamily.Monospace)
    if (state.detail.isNotEmpty()) {
      Text(state.detail, fontFamily = FontFamily.Monospace, fontSize = 11.sp, color = Color(0xFFB8B8C8))
    }
  }
  SwitchRow(stringResource(R.string.stats_overlay), state.showStats) {
    state.showStats = it
    host.persist()
  }
  SwitchRow(stringResource(R.string.detailed_stats), state.detailedStats) {
    state.detailedStats = it
    host.persist()
  }
  SwitchRow(stringResource(R.string.fast_forward), state.fastForward, stringResource(R.string.hint_fast_forward)) {
    host.setFastForward(it)
  }
  Column {
    Row(verticalAlignment = Alignment.CenterVertically) {
      Text(stringResource(R.string.tone_mapping), modifier = Modifier.weight(1f))
      Text(if (state.toneStrength <= 0f) stringResource(R.string.off) else "${(state.toneStrength * 100).toInt()}%")
    }
    Text(stringResource(R.string.hint_tone), style = MaterialTheme.typography.bodySmall, color = Color.Gray)
    Slider(
      value = state.toneStrength,
      onValueChange = {
        state.toneStrength = it
        host.setTone(it)
      },
      onValueChangeFinished = { host.persist() },
      valueRange = 0f..1f,
      steps = 9,
    )
  }
}

@Composable
private fun DiagnosticsTab(state: InGameState, host: InGameHost) {
  val s = host.settings
  SwitchRow(stringResource(R.string.opt_debug), s.debugOverlay.value, stringResource(R.string.hint_debug)) {
    s.debugOverlay.value = it
    host.changed()
  }
  SwitchRow(stringResource(R.string.opt_menu_pause), s.menuPause.value, stringResource(R.string.hint_menu_pause)) {
    s.menuPause.value = it
    host.changed()
  }
  SwitchRow(stringResource(R.string.detailed_stats), state.detailedStats) {
    state.detailedStats = it
    host.persist()
  }
  OutlinedButton(
    onClick = {
      host.markGlitch()
      state.menuOpen = false
    },
    modifier = BIG,
  ) { Text(stringResource(R.string.report_glitch)) }
  Text(
    stringResource(R.string.diag_info, state.version, state.titleId.ifEmpty { "—" }),
    style = MaterialTheme.typography.bodySmall,
    color = Color.Gray,
    fontFamily = FontFamily.Monospace,
  )
}
