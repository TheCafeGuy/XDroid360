package xdroid360.app

import android.content.SharedPreferences
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject

/** One persisted option: a Compose state backed by a `touch_<key>` pref. */
sealed class Opt<T>(val key: String, val def: T) {
  var value by mutableStateOf(def)

  abstract fun read(p: SharedPreferences)

  abstract fun write(e: SharedPreferences.Editor)

  /** Parses a live.toml value; false when it does not parse. */
  abstract fun parse(text: String): Boolean
}

class BoolOpt(key: String, def: Boolean) : Opt<Boolean>(key, def) {
  override fun read(p: SharedPreferences) {
    value = p.getBoolean("touch_$key", def)
  }

  override fun write(e: SharedPreferences.Editor) {
    e.putBoolean("touch_$key", value)
  }

  override fun parse(text: String): Boolean {
    value = when (text) {
      "true", "1" -> true
      "false", "0" -> false
      else -> return false
    }
    return true
  }
}

class FloatOpt(key: String, def: Float, val min: Float, val max: Float) : Opt<Float>(key, def) {
  override fun read(p: SharedPreferences) {
    value = p.getFloat("touch_$key", def).coerceIn(min, max)
  }

  override fun write(e: SharedPreferences.Editor) {
    e.putFloat("touch_$key", value)
  }

  override fun parse(text: String): Boolean {
    value = (text.toFloatOrNull() ?: return false).coerceIn(min, max)
    return true
  }
}

class ChoiceOpt(key: String, def: String, val choices: List<String>) : Opt<String>(key, def) {
  override fun read(p: SharedPreferences) {
    value = p.getString("touch_$key", def)?.takeIf { it in choices } ?: def
  }

  override fun write(e: SharedPreferences.Editor) {
    e.putString("touch_$key", value)
  }

  override fun parse(text: String): Boolean {
    if (text !in choices) return false
    value = text
    return true
  }
}

/**
 * Touch pad and controller options, shared by the game screen and the
 * library's Controls tab. Every option is also a live switch: live.toml key
 * `xdroid_touch_<key>` (phone.sh live / liveab) overrides it in a running
 * game without touching a menu.
 */
class ControlSettings {
  val show = BoolOpt("show", true)
  val autoHide = BoolOpt("autohide", true)
  val haptics = BoolOpt("haptics", true)
  val hapticStrength = FloatOpt("haptic_strength", 0.7f, 0.1f, 1f)
  val opacity = FloatOpt("opacity", TouchPadView.DEFAULT_OPACITY, 0.1f, 1f)
  // P0 feel.
  val lowLatency = BoolOpt("low_latency", true)
  val deadzone = FloatOpt("deadzone", 0.06f, 0f, 0.3f)
  val antiDeadzone = FloatOpt("anti_deadzone", 0.2f, 0f, 0.5f)
  val curve = ChoiceOpt("curve", "linear", listOf("linear", "exponential"))
  val sensitivity = FloatOpt("sensitivity", 1f, 0.5f, 2f)
  val floatingLeft = BoolOpt("floating_left", false)
  val stickFollow = BoolOpt("stick_follow", true)
  val stickDoubleTap = BoolOpt("stick_double_tap", true)
  val analogTriggers = BoolOpt("analog_triggers", false)
  val slideButtons = BoolOpt("slide_buttons", false)
  val guideLongPress = BoolOpt("guide_long_press", false)
  // The on-screen fast-forward button: hidden unless the user shows it.
  val showFf = BoolOpt("show_ff", false)
  val ffMode = ChoiceOpt("ff_mode", "toggle", listOf("toggle", "hold"))
  val palmReject = BoolOpt("palm_reject", true)
  val edgeMargin = FloatOpt("edge_margin", 12f, 0f, 48f)
  // P1 look.
  val autoDim = BoolOpt("auto_dim", true)
  val autoDimSeconds = FloatOpt("auto_dim_seconds", 6f, 2f, 30f)
  val debugOverlay = BoolOpt("debug_overlay", false)
  // P2 layouts.
  val preset = ChoiceOpt("preset", "auto", listOf("auto", "phone20x9", "phone16x9", "tablet", "foldable"))
  val mirror = BoolOpt("mirror", false)
  val perGame = BoolOpt("per_game", true)
  val gridSnap = BoolOpt("grid_snap", true)
  // Quick menu.
  val menuPause = BoolOpt("menu_pause", true)

  val all: List<Opt<*>> =
    listOf(
      show, autoHide, haptics, hapticStrength, opacity, lowLatency, deadzone, antiDeadzone, curve,
      sensitivity, floatingLeft, stickFollow, stickDoubleTap, analogTriggers, slideButtons,
      guideLongPress, showFf, ffMode, palmReject, edgeMargin, autoDim, autoDimSeconds, debugOverlay, preset, mirror,
      perGame, gridSnap, menuPause,
    )

  /** Android keycode to remap target (XInput mask, TARGET_LT or TARGET_RT). */
  var remap by mutableStateOf(mapOf<Int, Int>())

  fun load(p: SharedPreferences) {
    all.forEach { it.read(p) }
    remap =
      p.getString("pad_remap", "").orEmpty().split(',').mapNotNull {
        val kv = it.split(':')
        val k = kv.getOrNull(0)?.toIntOrNull()
        val v = kv.getOrNull(1)?.toIntOrNull()
        if (k != null && v != null) k to v else null
      }.toMap()
  }

  fun save(p: SharedPreferences) {
    val e = p.edit()
    all.forEach { it.write(e) }
    e.putString("pad_remap", remap.entries.joinToString(",") { "${it.key}:${it.value}" })
    e.apply()
  }

  /** Applies `xdroid_touch_<key>` live switches; true if any changed. */
  fun applyLive(live: Map<String, String>): Boolean {
    var any = false
    for (o in all) {
      val text = live["xdroid_touch_${o.key}"] ?: continue
      val before = o.value
      if (o.parse(text) && o.value != before) any = true
    }
    return any
  }

  // ---- Layouts: global, or per game (title ID) with the global as fallback. ----

  fun layoutKey(titleId: String?) =
    if (perGame.value && !titleId.isNullOrEmpty()) "touch_layout_$titleId" else "touch_layout"

  fun loadLayout(p: SharedPreferences, titleId: String?): String? =
    p.getString(layoutKey(titleId), null) ?: p.getString("touch_layout", null)

  fun saveLayout(p: SharedPreferences, titleId: String?, json: String) {
    p.edit().putString(layoutKey(titleId), json).apply()
  }

  /** A game's own layout key, whatever per_game says (library editor). */
  fun gameLayoutKey(titleId: String) = "touch_layout_$titleId"

  fun clearLayout(p: SharedPreferences, titleId: String?) {
    p.edit().remove(layoutKey(titleId)).apply()
  }
}

enum class TouchKind { STICK, DPAD, FACE, BUTTON, TRIGGER, GUIDE, FAST_FORWARD }

/**
 * Where a control sits by default: a horizontal anchor (-1 left edge, 0
 * center, 1 right edge) with an offset in dp toward the screen, and a
 * vertical one (-1 top, 1 bottom). Sizes are dp, so thumbs reach the same
 * physical spots on any aspect; the saved layout is in fractions of the view.
 */
data class Anchor(val hx: Int, val xDp: Float, val vy: Int, val yDp: Float)

class TouchControl(
  val id: String,
  val kind: TouchKind,
  val baseDp: Float,
  val anchor: Anchor,
  // BUTTON: XInput mask. STICK: the thumb click mask.
  val mask: Int = 0,
  // STICK: X axis id (Y is axis + 1). TRIGGER: the trigger axis id.
  val axis: Int = -1,
  val wide: Boolean = false,
  // Mirror swaps these between the left and right side.
  val mirrors: Boolean = false,
  val hiddenByDefault: Boolean = false,
) {
  var x = 0.5f
  var y = 0.5f
  var scale = 1f
  var visible = !hiddenByDefault
  // Per-control opacity multiplier on top of the global one.
  var alpha = 1f

  /** Label key for the editor and menus (resolved by TouchStrings). */
  val labelKey get() = id
}

// Remap targets beyond the 16-bit XInput button masks.
const val TARGET_LT = 0x10000
const val TARGET_RT = 0x20000

/** Xbox 360 inputs a controller key can be remapped to, in menu order. */
val PAD_TARGETS =
  listOf(
    "A" to 0x1000,
    "B" to 0x2000,
    "X" to 0x4000,
    "Y" to 0x8000,
    "LB" to 0x0100,
    "RB" to 0x0200,
    "LT" to TARGET_LT,
    "RT" to TARGET_RT,
    "Back" to 0x0020,
    "Start" to 0x0010,
    "L3" to 0x0040,
    "R3" to 0x0080,
    "D-pad ↑" to 0x0001,
    "D-pad ↓" to 0x0002,
    "D-pad ←" to 0x0004,
    "D-pad →" to 0x0008,
  )

object TouchLayout {
  const val VERSION = 2

  fun controls() =
    listOf(
      TouchControl("ls", TouchKind.STICK, 120f, Anchor(-1, 118f, 1, 195f), mask = 0x0040, axis = 0, mirrors = true),
      TouchControl("rs", TouchKind.STICK, 110f, Anchor(1, 235f, 1, 88f), mask = 0x0080, axis = 2, mirrors = true),
      TouchControl("dpad", TouchKind.DPAD, 120f, Anchor(-1, 255f, 1, 82f), mirrors = true),
      TouchControl("face", TouchKind.FACE, 140f, Anchor(1, 112f, 1, 195f), mirrors = true),
      TouchControl("lt", TouchKind.TRIGGER, 64f, Anchor(-1, 58f, -1, 92f), axis = 4),
      TouchControl("lb", TouchKind.BUTTON, 84f, Anchor(-1, 160f, -1, 82f), mask = 0x0100, wide = true),
      TouchControl("rb", TouchKind.BUTTON, 84f, Anchor(1, 160f, -1, 82f), mask = 0x0200, wide = true),
      TouchControl("rt", TouchKind.TRIGGER, 64f, Anchor(1, 58f, -1, 92f), axis = 5),
      TouchControl("back", TouchKind.BUTTON, 48f, Anchor(0, -78f, 1, 34f), mask = 0x0020, wide = true),
      TouchControl("guide", TouchKind.GUIDE, 54f, Anchor(0, 0f, 1, 38f)),
      TouchControl("start", TouchKind.BUTTON, 48f, Anchor(0, 78f, 1, 34f), mask = 0x0010, wide = true),
      TouchControl("l3", TouchKind.BUTTON, 44f, Anchor(-1, 215f, 1, 215f), mask = 0x0040, hiddenByDefault = true),
      TouchControl("r3", TouchKind.BUTTON, 44f, Anchor(1, 330f, 1, 60f), mask = 0x0080, hiddenByDefault = true),
      TouchControl("ff", TouchKind.FAST_FORWARD, 52f, Anchor(1, 290f, -1, 40f), wide = true, hiddenByDefault = true),
    )

  /** Resolves "auto" from the screen: smallest width, then aspect. */
  fun resolvePreset(preset: String, wDp: Float, hDp: Float): String {
    if (preset != "auto") return preset
    val aspect = maxOf(wDp, hDp) / minOf(wDp, hDp)
    return when {
      minOf(wDp, hDp) >= 600f -> "tablet"
      aspect < 1.5f -> "foldable"
      aspect < 1.9f -> "phone16x9"
      else -> "phone20x9"
    }
  }

  /**
   * Places every control for a preset on a view of w x h px. Tablets and
   * foldables are held by the sides at mid height: bottom controls rise.
   */
  fun applyPreset(
    controls: List<TouchControl>,
    preset: String,
    mirror: Boolean,
    w: Int,
    h: Int,
    density: Float,
  ) {
    if (w == 0 || h == 0) return
    val wDp = w / density
    val hDp = h / density
    val p = resolvePreset(preset, wDp, hDp)
    val scale = when (p) {
      "phone16x9" -> 0.95f
      "tablet" -> 1.1f
      else -> 1f
    }
    val lift = when (p) {
      "tablet" -> hDp * 0.18f
      "foldable" -> hDp * 0.12f
      else -> 0f
    }
    for (c in controls) {
      val a = c.anchor
      val hx = if (mirror && c.mirrors) -a.hx else a.hx
      val xDp = when (hx) {
        -1 -> a.xDp * scale
        1 -> wDp - a.xDp * scale
        else -> wDp / 2 + a.xDp * scale
      }
      val yDp = if (a.vy < 0) a.yDp * scale else hDp - a.yDp * scale - lift
      c.x = (xDp / wDp).coerceIn(0f, 1f)
      c.y = (yDp / hDp).coerceIn(0f, 1f)
      c.scale = scale
      c.visible = !c.hiddenByDefault
      c.alpha = 1f
    }
  }

  fun toJson(controls: List<TouchControl>): String {
    val list = JSONObject()
    for (c in controls) {
      list.put(
        c.id,
        JSONObject().put("x", c.x.toDouble()).put("y", c.y.toDouble()).put("s", c.scale.toDouble())
          .put("v", c.visible).put("a", c.alpha.toDouble()),
      )
    }
    return JSONObject().put("version", VERSION).put("controls", list).toString()
  }

  /** False when there is nothing usable (caller then applies the preset). */
  fun fromJson(controls: List<TouchControl>, json: String?): Boolean {
    if (json.isNullOrEmpty()) return false
    val root = try {
      JSONObject(json)
    } catch (e: org.json.JSONException) {
      return false
    }
    // Version 1 (v0.2.0 development builds) was the bare control map.
    val list = if (root.has("version")) root.optJSONObject("controls") ?: return false else root
    var any = false
    for (c in controls) {
      val o = list.optJSONObject(c.id) ?: continue
      c.x = o.optDouble("x", c.x.toDouble()).toFloat().coerceIn(0f, 1f)
      c.y = o.optDouble("y", c.y.toDouble()).toFloat().coerceIn(0f, 1f)
      c.scale = o.optDouble("s", 1.0).toFloat().coerceIn(TouchPadView.MIN_SCALE, TouchPadView.MAX_SCALE)
      c.visible = o.optBoolean("v", !c.hiddenByDefault)
      c.alpha = o.optDouble("a", 1.0).toFloat().coerceIn(0.1f, 1f)
      any = true
    }
    return any
  }
}
