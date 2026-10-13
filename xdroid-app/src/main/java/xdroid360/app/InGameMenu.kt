package xdroid360.app

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
}

/** Callbacks into GameActivity. */
class InGameActions(
  val setFastForward: (Boolean) -> Unit,
  val setTone: (Float) -> Unit,
  val markGlitch: () -> Unit,
  val quit: () -> Unit,
  val persist: () -> Unit,
)

@Composable
fun InGameOverlay(state: InGameState, actions: InGameActions) {
  MaterialThemeOverlay {
    Box(Modifier.fillMaxSize()) {
      // Compact stats (top-left), tap to open the menu.
      if (state.showStats && !state.menuOpen) {
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
      if (!state.menuOpen) {
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
            "Select+Start or controller Back: this menu · PS/Home: fast forward · ⚑ top: tap reports a glitch, hold opens this menu · Swipe back: exit to the library",
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
