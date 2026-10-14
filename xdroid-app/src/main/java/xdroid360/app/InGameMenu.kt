package xdroid360.app

import android.view.KeyEvent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Live state shared between GameActivity's samplers and the overlay. */
class InGameState {
  var menuOpen by mutableStateOf(false)
  var showStats by mutableStateOf(true)
  var detailedStats by mutableStateOf(false)
  var fastForward by mutableStateOf(false)
  var toneStrength by mutableStateOf(0f)
  var fps by mutableStateOf("")
  var cpu by mutableStateOf("")
  var gpu by mutableStateOf("")
  var thermal by mutableStateOf("")
  var detail by mutableStateOf("")

  // Touch pad and controller.
  var touchShow by mutableStateOf(true)
  var touchAutoHide by mutableStateOf(true)
  var touchHaptics by mutableStateOf(true)
  var gamepadConnected by mutableStateOf(false)
  var touchEditing by mutableStateOf(false)
  var touchSelected by mutableStateOf("")
  var touchScale by mutableStateOf(1f)
  var touchSelHidden by mutableStateOf(false)
  var touchOpacity by mutableStateOf(TouchPadView.DEFAULT_OPACITY)
  var remapOpen by mutableStateOf(false)
  // Target being captured: the next controller key is bound to it.
  var remapTarget by mutableStateOf<Int?>(null)
  // Android keycode to target (XInput mask, TARGET_LT or TARGET_RT).
  var remapOverrides by mutableStateOf(mapOf<Int, Int>())
}

/** Callbacks into GameActivity. */
class InGameActions(
  val setFastForward: (Boolean) -> Unit,
  val setTone: (Float) -> Unit,
  val markGlitch: () -> Unit,
  val quit: () -> Unit,
  val persist: () -> Unit,
  val touch: TouchActions,
)

@Composable
fun InGameOverlay(state: InGameState, actions: InGameActions) {
  MaterialThemeOverlay {
    // The pad hides with a controller (auto mode) and under the menu; edit
    // mode always shows it.
    val padVisible = state.touchEditing ||
      (state.touchShow && !(state.touchAutoHide && state.gamepadConnected) && !state.menuOpen)
    LaunchedEffect(padVisible, state.touchEditing) { actions.touch.apply(padVisible, state.touchEditing) }
    Box(Modifier.fillMaxSize()) {
      if (state.touchEditing) TouchEditBar(state, actions, Modifier.align(Alignment.TopCenter))
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
      // Glitch flag (top-center): tap reports a glitch, hold opens the menu
      // (controllers open it with Select+Start, touch players need a way).
      if (!state.menuOpen && !state.touchEditing) {
        Text(
          "⚑",
          color = Color(0x88FFFFFF),
          fontSize = 18.sp,
          modifier = Modifier
            .align(Alignment.TopCenter)
            .pointerInput(Unit) {
              detectTapGestures(
                onTap = { actions.markGlitch() },
                onLongPress = { state.menuOpen = true },
              )
            }
            .padding(horizontal = 24.dp, vertical = 2.dp),
        )
      }
      AnimatedVisibility(
        visible = state.menuOpen,
        enter = slideInVertically { -it },
        exit = slideOutVertically { -it },
        modifier = Modifier.align(Alignment.TopCenter),
      ) {
        Column(
          modifier = Modifier
            .padding(top = 8.dp)
            .widthIn(max = 560.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(bottomStart = 20.dp, bottomEnd = 20.dp, topStart = 20.dp, topEnd = 20.dp))
            .background(Color(0xEE101016))
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
          verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
          Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Quick menu", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            OutlinedButton(onClick = { state.menuOpen = false; actions.persist() }) { Text("Resume ▴") }
          }
          // Performance readout.
          Column(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Color(0xFF1A1A22)).padding(12.dp),
          ) {
            Text("Performance", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.SemiBold)
            Text(state.fps.ifEmpty { "FPS —" }, fontFamily = FontFamily.Monospace)
            Text(state.cpu.ifEmpty { "CPU —" }, fontFamily = FontFamily.Monospace)
            Text(state.gpu.ifEmpty { "GPU —" }, fontFamily = FontFamily.Monospace)
            Text(state.thermal.ifEmpty { "Thermal —" }, fontFamily = FontFamily.Monospace)
            if (state.detail.isNotEmpty()) {
              Text(state.detail, fontFamily = FontFamily.Monospace, fontSize = 11.sp, color = Color(0xFFB8B8C8))
            }
          }
          ToggleRow("Show stats overlay", state.showStats) { state.showStats = it }
          ToggleRow("Detailed stats", state.detailedStats) { state.detailedStats = it }
          ToggleRow("Fast forward", state.fastForward) {
            state.fastForward = it
            actions.setFastForward(it)
          }
          Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
              Text("AMOLED tone mapping", modifier = Modifier.weight(1f))
              Text(if (state.toneStrength <= 0f) "Off" else "${(state.toneStrength * 100).toInt()}%")
            }
            Text(
              "Softens highlights that burn out on OLED (snow, sky, white HUD).",
              style = MaterialTheme.typography.bodySmall,
              color = Color.Gray,
            )
            Slider(
              value = state.toneStrength,
              onValueChange = {
                state.toneStrength = it
                actions.setTone(it)
              },
              valueRange = 0f..1f,
              steps = 9,
            )
          }
          if (state.remapOpen) RemapPanel(state, actions) else ControlsSection(state, actions)
          Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = {
              actions.markGlitch()
              state.menuOpen = false
              actions.persist()
            }) { Text("Report glitch") }
            Spacer(Modifier.weight(1f))
            Button(onClick = { actions.quit() }) { Text("Exit game") }
          }
          Text(
            "Select+Start, controller Back or the on-screen ≡: this menu · PS/Home: fast forward · ⚑ top: tap reports a glitch, hold opens this menu · Swipe back: exit to the library",
            style = MaterialTheme.typography.labelSmall,
            color = Color.Gray,
          )
        }
      }
    }
  }
}

@Composable
private fun ToggleRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
  Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
    Text(label, modifier = Modifier.weight(1f))
    Spacer(Modifier.width(8.dp))
    Switch(checked = checked, onCheckedChange = onChange)
  }
}

@Composable
private fun MaterialThemeOverlay(content: @Composable () -> Unit) {
  MaterialTheme(colorScheme = XdColors, content = content)
}

@Composable
private fun ControlsSection(state: InGameState, actions: InGameActions) {
  Column(
    Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Color(0xFF1A1A22)).padding(12.dp),
    verticalArrangement = Arrangement.spacedBy(6.dp),
  ) {
    Text("Controls", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.SemiBold)
    Text(
      if (state.gamepadConnected) "Controller connected" else "No controller connected",
      style = MaterialTheme.typography.bodySmall,
      color = Color.Gray,
    )
    ToggleRow("Touch controls", state.touchShow) {
      state.touchShow = it
      actions.touch.settingsChanged()
    }
    ToggleRow("Hide them when a controller is connected", state.touchAutoHide) {
      state.touchAutoHide = it
      actions.touch.settingsChanged()
    }
    ToggleRow("Vibrate on touch", state.touchHaptics) {
      state.touchHaptics = it
      actions.touch.settingsChanged()
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
      OutlinedButton(onClick = {
        state.menuOpen = false
        state.touchEditing = true
      }) { Text("Edit touch layout") }
      OutlinedButton(onClick = { state.remapOpen = true }) { Text("Remap controller") }
    }
  }
}

@Composable
private fun RemapPanel(state: InGameState, actions: InGameActions) {
  Column(
    Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Color(0xFF1A1A22)).padding(12.dp),
    verticalArrangement = Arrangement.spacedBy(4.dp),
  ) {
    Text("Remap controller", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.SemiBold)
    Text(
      "Buttons map automatically to the standard layout. Tap an input, then press the controller button you want for it.",
      style = MaterialTheme.typography.bodySmall,
      color = Color.Gray,
    )
    for ((name, target) in PAD_TARGETS) {
      val keys = state.remapOverrides.filterValues { it == target }.keys
      val binding = when {
        state.remapTarget == target -> "Press a button…"
        keys.isEmpty() -> "Automatic"
        else -> keys.joinToString { KeyEvent.keyCodeToString(it).removePrefix("KEYCODE_") }
      }
      Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp))
          .clickable { state.remapTarget = if (state.remapTarget == target) null else target }
          .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
      ) {
        Text(name, modifier = Modifier.weight(1f))
        Text(
          binding,
          fontFamily = FontFamily.Monospace,
          fontSize = 12.sp,
          color = if (state.remapTarget == target) Color(0xFF7CFFB2) else Color(0xFFB8B8C8),
        )
      }
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
      OutlinedButton(onClick = {
        state.remapTarget = null
        state.remapOverrides = emptyMap()
        actions.touch.remapChanged()
      }) { Text("Reset to automatic") }
      Spacer(Modifier.weight(1f))
      Button(onClick = {
        state.remapTarget = null
        state.remapOpen = false
      }) { Text("Done") }
    }
  }
}

@Composable
private fun TouchEditBar(state: InGameState, actions: InGameActions, modifier: Modifier) {
  Column(
    modifier = modifier
      .padding(top = 8.dp)
      .widthIn(max = 420.dp)
      .clip(RoundedCornerShape(16.dp))
      .background(Color(0xDD101016))
      .padding(horizontal = 14.dp, vertical = 8.dp),
  ) {
    Text(
      if (state.touchSelected.isEmpty()) "Drag a control to move it, tap it to resize or hide it"
      else "Selected: ${state.touchSelected}",
      style = MaterialTheme.typography.bodySmall,
    )
    if (state.touchSelected.isNotEmpty()) {
      Row(verticalAlignment = Alignment.CenterVertically) {
        Text("Size", modifier = Modifier.width(64.dp), style = MaterialTheme.typography.bodySmall)
        Slider(
          value = state.touchScale,
          onValueChange = {
            state.touchScale = it
            actions.touch.setScale(it)
          },
          valueRange = TouchPadView.MIN_SCALE..TouchPadView.MAX_SCALE,
          modifier = Modifier.weight(1f),
        )
      }
      ToggleRow("Hide this control", state.touchSelHidden) {
        state.touchSelHidden = it
        actions.touch.setHidden(it)
      }
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
      Text("Opacity", modifier = Modifier.width(64.dp), style = MaterialTheme.typography.bodySmall)
      Slider(
        value = state.touchOpacity,
        onValueChange = {
          state.touchOpacity = it
          actions.touch.setOpacity(it)
        },
        valueRange = 0.1f..1f,
        modifier = Modifier.weight(1f),
      )
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
      OutlinedButton(onClick = { actions.touch.resetLayout() }) { Text("Reset") }
      Spacer(Modifier.weight(1f))
      Button(onClick = {
        state.touchEditing = false
        actions.touch.settingsChanged()
      }) { Text("Done") }
    }
  }
}
