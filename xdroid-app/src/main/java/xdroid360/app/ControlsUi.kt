package xdroid360.app

import android.view.KeyEvent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.util.Locale

/** What the controls UI needs from the screen hosting it. */
interface ControlsHost {
  /** An option changed: save it and apply it to the pad. */
  fun changed()

  /** Preset or mirror changed: place the controls again. */
  fun presetChanged()

  fun editLayout()
}

/** Remap screen state: the input waiting for a controller key. */
class RemapState {
  var open by mutableStateOf(false)
  var target by mutableStateOf<Int?>(null)
}

/** Layout editor bar state, fed by TouchPadView's selection. */
class EditorState {
  var selectedId by mutableStateOf("")
  var scale by mutableStateOf(1f)
  var hidden by mutableStateOf(false)
  var alpha by mutableStateOf(1f)
  var canUndo by mutableStateOf(false)
  var testing by mutableStateOf(false)
}

interface EditorActions {
  fun beginEdit()

  fun setScale(scale: Float)

  fun setHidden(hidden: Boolean)

  fun setAlpha(alpha: Float)

  fun undo()

  fun reset()

  fun test(on: Boolean)

  fun done()
}

private val PANEL = Color(0xFF1A1A22)
private val ACCENT = Color(0xFF7CFFB2)

@Composable
fun controlName(id: String): String =
  stringResource(
    when (id) {
      "ls" -> R.string.ctl_ls
      "rs" -> R.string.ctl_rs
      "dpad" -> R.string.ctl_dpad
      "face" -> R.string.ctl_face
      "lt" -> R.string.ctl_lt
      "lb" -> R.string.ctl_lb
      "rb" -> R.string.ctl_rb
      "rt" -> R.string.ctl_rt
      "back" -> R.string.ctl_back
      "guide" -> R.string.ctl_guide
      "start" -> R.string.ctl_start
      "l3" -> R.string.ctl_l3
      "r3" -> R.string.ctl_r3
      else -> R.string.ctl_ff
    },
  )

@Composable
fun Section(title: String, content: @Composable () -> Unit) {
  Column(
    Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(PANEL).padding(12.dp),
    verticalArrangement = Arrangement.spacedBy(4.dp),
  ) {
    Text(title, color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.SemiBold)
    content()
  }
}

@Composable
fun SwitchRow(label: String, checked: Boolean, hint: String? = null, onChange: (Boolean) -> Unit) {
  Row(
    verticalAlignment = Alignment.CenterVertically,
    modifier = Modifier.fillMaxWidth().clickable { onChange(!checked) }.padding(vertical = 2.dp),
  ) {
    Column(Modifier.weight(1f)) {
      Text(label)
      if (hint != null) Text(hint, style = MaterialTheme.typography.bodySmall, color = Color.Gray)
    }
    Spacer(Modifier.width(8.dp))
    Switch(checked = checked, onCheckedChange = onChange)
  }
}

@Composable
private fun OptSwitch(label: Int, opt: BoolOpt, host: ControlsHost, hint: Int? = null) {
  SwitchRow(stringResource(label), opt.value, hint?.let { stringResource(it) }) {
    opt.value = it
    host.changed()
  }
}

@Composable
private fun OptSlider(label: Int, opt: FloatOpt, host: ControlsHost, format: (Float) -> String) {
  Column {
    Row(verticalAlignment = Alignment.CenterVertically) {
      Text(stringResource(label), modifier = Modifier.weight(1f))
      Text(format(opt.value), fontFamily = FontFamily.Monospace, fontSize = 12.sp)
    }
    Slider(
      value = opt.value,
      onValueChange = { opt.value = it },
      onValueChangeFinished = { host.changed() },
      valueRange = opt.min..opt.max,
    )
  }
}

@Composable
private fun OptChoice(label: Int, opt: ChoiceOpt, names: List<Int>, onPick: () -> Unit) {
  Column {
    Text(stringResource(label))
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
      opt.choices.forEachIndexed { i, c ->
        FilterChip(
          selected = opt.value == c,
          onClick = {
            opt.value = c
            onPick()
          },
          label = { Text(stringResource(names[i]), fontSize = 12.sp) },
        )
      }
    }
  }
}

private fun pct(v: Float) = "${(v * 100).toInt()} %"

/** Every touch and controller option; used in game and in the library. */
@Composable
fun ControlsSettingsPanel(s: ControlSettings, host: ControlsHost, remap: RemapState, gamepadConnected: Boolean) {
  Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
    Section(stringResource(R.string.sec_touch)) {
      Text(
        stringResource(if (gamepadConnected) R.string.pad_connected else R.string.pad_none),
        style = MaterialTheme.typography.bodySmall,
        color = Color.Gray,
      )
      OptSwitch(R.string.opt_show, s.show, host)
      OptSwitch(R.string.opt_autohide, s.autoHide, host)
      OptSlider(R.string.opt_opacity, s.opacity, host, ::pct)
      OptSwitch(R.string.opt_haptics, s.haptics, host, R.string.hint_haptics)
      if (s.haptics.value) OptSlider(R.string.opt_haptic_strength, s.hapticStrength, host, ::pct)
      OptSwitch(R.string.opt_auto_dim, s.autoDim, host, R.string.hint_auto_dim)
      if (s.autoDim.value) OptSlider(R.string.opt_auto_dim_seconds, s.autoDimSeconds, host) { "${it.toInt()} s" }
      OptSlider(R.string.opt_edge_margin, s.edgeMargin, host) { "${it.toInt()} dp" }
      OptSwitch(R.string.opt_palm, s.palmReject, host)
      OptSwitch(R.string.opt_low_latency, s.lowLatency, host, R.string.hint_low_latency)
      Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Button(onClick = { host.editLayout() }) { Text(stringResource(R.string.edit_layout)) }
      }
    }
    Section(stringResource(R.string.sec_sticks)) {
      OptSlider(R.string.opt_deadzone, s.deadzone, host, ::pct)
      OptSlider(R.string.opt_anti_deadzone, s.antiDeadzone, host, ::pct)
      Text(stringResource(R.string.hint_anti_deadzone), style = MaterialTheme.typography.bodySmall, color = Color.Gray)
      OptChoice(R.string.opt_curve, s.curve, listOf(R.string.curve_linear, R.string.curve_exp)) { host.changed() }
      OptSlider(R.string.opt_sensitivity, s.sensitivity, host) { String.format(Locale.US, "%.2fx", it) }
      OptSwitch(R.string.opt_floating, s.floatingLeft, host, R.string.hint_floating)
      OptSwitch(R.string.opt_follow, s.stickFollow, host, R.string.hint_follow)
      OptSwitch(R.string.opt_double_tap, s.stickDoubleTap, host, R.string.hint_double_tap)
    }
    Section(stringResource(R.string.sec_buttons)) {
      OptSwitch(R.string.opt_analog_triggers, s.analogTriggers, host, R.string.hint_analog_triggers)
      OptSwitch(R.string.opt_slide, s.slideButtons, host, R.string.hint_slide)
      OptSwitch(R.string.opt_guide_long, s.guideLongPress, host, R.string.hint_guide_long)
      OptSwitch(R.string.opt_show_ff, s.showFf, host, R.string.hint_show_ff)
      if (s.showFf.value) {
        OptChoice(R.string.opt_ff_mode, s.ffMode, listOf(R.string.ff_toggle, R.string.ff_hold)) { host.changed() }
      }
    }
    Section(stringResource(R.string.sec_layout)) {
      OptChoice(
        R.string.opt_preset,
        s.preset,
        listOf(R.string.preset_auto, R.string.preset_phone20, R.string.preset_phone16, R.string.preset_tablet, R.string.preset_fold),
      ) { host.presetChanged() }
      SwitchRow(stringResource(R.string.opt_mirror), s.mirror.value, stringResource(R.string.hint_mirror)) {
        s.mirror.value = it
        host.presetChanged()
      }
      OptSwitch(R.string.opt_per_game, s.perGame, host, R.string.hint_per_game)
      OptSwitch(R.string.opt_grid, s.gridSnap, host)
    }
    Section(stringResource(R.string.sec_controller)) {
      if (remap.open) {
        RemapPanel(s, remap, host)
      } else {
        Text(stringResource(R.string.hint_remap), style = MaterialTheme.typography.bodySmall, color = Color.Gray)
        OutlinedButton(onClick = { remap.open = true }) { Text(stringResource(R.string.remap)) }
      }
    }
  }
}

@Composable
fun RemapPanel(s: ControlSettings, remap: RemapState, host: ControlsHost) {
  Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
    Text(stringResource(R.string.remap_how), style = MaterialTheme.typography.bodySmall, color = Color.Gray)
    for ((name, target) in PAD_TARGETS) {
      val keys = s.remap.filterValues { it == target }.keys
      val waiting = remap.target == target
      val binding = when {
        waiting -> stringResource(R.string.remap_press)
        keys.isEmpty() -> stringResource(R.string.remap_auto)
        else -> keys.joinToString { KeyEvent.keyCodeToString(it).removePrefix("KEYCODE_") }
      }
      Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp))
          .clickable { remap.target = if (waiting) null else target }
          .padding(horizontal = 8.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
      ) {
        Text(name, modifier = Modifier.weight(1f))
        Text(binding, fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = if (waiting) ACCENT else Color(0xFFB8B8C8))
      }
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
      OutlinedButton(onClick = {
        remap.target = null
        s.remap = emptyMap()
        host.changed()
      }) { Text(stringResource(R.string.remap_reset)) }
      Spacer(Modifier.weight(1f))
      Button(onClick = {
        remap.target = null
        remap.open = false
      }) { Text(stringResource(R.string.done)) }
    }
  }
}

/** Binds a controller key to the waiting remap target; true if consumed. */
fun captureRemap(s: ControlSettings, remap: RemapState, keyCode: Int): Boolean {
  val target = remap.target ?: return false
  s.remap = s.remap.filterValues { it != target } + (keyCode to target)
  remap.target = null
  return true
}

@Composable
fun TouchEditBar(e: EditorState, actions: EditorActions, modifier: Modifier = Modifier) {
  Column(
    modifier = modifier
      .padding(top = 8.dp)
      .widthIn(max = 440.dp)
      .clip(RoundedCornerShape(16.dp))
      .background(Color(0xDD101016))
      .padding(horizontal = 14.dp, vertical = 8.dp),
  ) {
    Text(
      if (e.testing) {
        stringResource(R.string.edit_testing)
      } else if (e.selectedId.isEmpty()) {
        stringResource(R.string.edit_hint)
      } else {
        stringResource(R.string.edit_selected, controlName(e.selectedId))
      },
      style = MaterialTheme.typography.bodySmall,
    )
    if (!e.testing && e.selectedId.isNotEmpty()) {
      EditSlider(R.string.edit_size, e.scale, TouchPadView.MIN_SCALE..TouchPadView.MAX_SCALE, actions) {
        e.scale = it
        actions.setScale(it)
      }
      EditSlider(R.string.edit_opacity, e.alpha, 0.1f..1f, actions) {
        e.alpha = it
        actions.setAlpha(it)
      }
      SwitchRow(stringResource(R.string.edit_hide), e.hidden) {
        e.hidden = it
        actions.setHidden(it)
      }
    }
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
      OutlinedButton(enabled = e.canUndo && !e.testing, onClick = { actions.undo() }) { Text(stringResource(R.string.edit_undo)) }
      OutlinedButton(enabled = !e.testing, onClick = { actions.reset() }) { Text(stringResource(R.string.edit_reset)) }
      OutlinedButton(onClick = {
        e.testing = !e.testing
        actions.test(e.testing)
      }) { Text(stringResource(if (e.testing) R.string.edit_back else R.string.edit_test)) }
      Spacer(Modifier.weight(1f))
      Button(onClick = { actions.done() }) { Text(stringResource(R.string.done)) }
    }
  }
}

@Composable
private fun EditSlider(
  label: Int,
  value: Float,
  range: ClosedFloatingPointRange<Float>,
  actions: EditorActions,
  onChange: (Float) -> Unit,
) {
  var started by androidx.compose.runtime.remember { mutableStateOf(false) }
  Row(verticalAlignment = Alignment.CenterVertically) {
    Text(stringResource(label), modifier = Modifier.width(72.dp), style = MaterialTheme.typography.bodySmall)
    Slider(
      value = value,
      onValueChange = {
        if (!started) {
          started = true
          actions.beginEdit()
        }
        onChange(it)
      },
      onValueChangeFinished = { started = false },
      valueRange = range,
      modifier = Modifier.weight(1f),
    )
  }
}
