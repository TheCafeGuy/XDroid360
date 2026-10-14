package xdroid360.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.os.SystemClock
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import org.json.JSONObject

/** Where the virtual pad sends its input; GameActivity routes it to Bridge. */
interface PadSink {
  fun button(mask: Int, down: Boolean)

  fun axis(axis: Int, value: Float)

  /** The on-screen Guide button: opens the quick menu. */
  fun guide()
}

/** Quick menu hooks for the touch pad and the controller remap. */
interface TouchActions {
  /** Show/hide/vibrate toggles or the layout changed: apply and save. */
  fun settingsChanged()

  fun apply(visible: Boolean, editing: Boolean)

  fun setScale(scale: Float)

  fun setOpacity(opacity: Float)

  fun setHidden(hidden: Boolean)

  fun resetLayout()

  fun remapChanged()
}

enum class TouchKind { STICK, DPAD, FACE, BUTTON, TRIGGER }

/**
 * One on-screen control. Position is the center as a fraction of the pad
 * view, so a layout survives resolution and aspect changes.
 */
class TouchControl(
  val id: String,
  val label: String,
  val kind: TouchKind,
  val baseDp: Float,
  val defX: Float,
  val defY: Float,
  // BUTTON: XInput mask (0 = Guide). STICK: the thumb click mask.
  val mask: Int = 0,
  // STICK: X axis id (Y is axis + 1). TRIGGER: the trigger axis id.
  val axis: Int = -1,
  val wide: Boolean = false,
) {
  var x = defX
  var y = defY
  var scale = 1f
  var visible = true

  fun reset() {
    x = defX
    y = defY
    scale = 1f
    visible = true
  }
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
    "L3 (left stick click)" to 0x0040,
    "R3 (right stick click)" to 0x0080,
    "D-pad up" to 0x0001,
    "D-pad down" to 0x0002,
    "D-pad left" to 0x0004,
    "D-pad right" to 0x0008,
  )

/** Xbox 360 layout sized for a landscape phone. */
private fun defaultControls() =
  listOf(
    TouchControl("ls", "Left stick", TouchKind.STICK, 120f, 0.13f, 0.52f, mask = 0x0040, axis = 0),
    TouchControl("rs", "Right stick", TouchKind.STICK, 110f, 0.74f, 0.78f, mask = 0x0080, axis = 2),
    TouchControl("dpad", "D-pad", TouchKind.DPAD, 120f, 0.28f, 0.80f),
    TouchControl("face", "A B X Y", TouchKind.FACE, 140f, 0.88f, 0.52f),
    TouchControl("lt", "LT", TouchKind.TRIGGER, 72f, 0.07f, 0.22f, axis = 4, wide = true),
    TouchControl("lb", "LB", TouchKind.BUTTON, 72f, 0.18f, 0.22f, mask = 0x0100, wide = true),
    TouchControl("rb", "RB", TouchKind.BUTTON, 72f, 0.82f, 0.22f, mask = 0x0200, wide = true),
    TouchControl("rt", "RT", TouchKind.TRIGGER, 72f, 0.93f, 0.22f, axis = 5, wide = true),
    TouchControl("back", "Back", TouchKind.BUTTON, 56f, 0.42f, 0.92f, mask = 0x0020, wide = true),
    TouchControl("guide", "≡", TouchKind.BUTTON, 38f, 0.50f, 0.92f),
    TouchControl("start", "Start", TouchKind.BUTTON, 56f, 0.58f, 0.92f, mask = 0x0010, wide = true),
  )

/**
 * The virtual Xbox 360 pad: one view under the quick menu overlay, so the
 * stats box and the ⚑ flag keep their touches. Multi-touch: each finger
 * belongs to the control it landed on; the D-pad and the face buttons follow
 * a sliding finger. Output is the OR of all fingers, sent only on change.
 * Sticks are relative to where the finger lands; a double tap holds the
 * stick click. In edit mode a finger drags a control and a tap selects it.
 */
@SuppressLint("ViewConstructor")
class TouchPadView(
  context: Context,
  private val sink: PadSink,
  private val onSelect: (TouchControl?) -> Unit,
) : View(context) {
  val controls = defaultControls()
  var opacity = DEFAULT_OPACITY
    set(value) {
      field = value
      invalidate()
    }
  var haptics = true
  var editing = false
    set(value) {
      if (field == value) return
      releaseAll()
      field = value
      select(null)
    }
  var selected: TouchControl? = null
    private set

  private class Track(
    val control: TouchControl,
    val ox: Float,
    val oy: Float,
    var x: Float,
    var y: Float,
    val thumb: Boolean,
  )

  private val density = resources.displayMetrics.density
  private val tracks = HashMap<Int, Track>()
  private val lastUp = HashMap<String, Long>()
  private var sentButtons = 0
  private val sentAxes = FloatArray(6)
  private var dragId = -1
  private var dragDx = 0f
  private var dragDy = 0f

  private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
  private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    style = Paint.Style.STROKE
    strokeWidth = 2f * resources.displayMetrics.density
  }
  private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    textAlign = Paint.Align.CENTER
    isFakeBoldText = true
  }
  private val rect = RectF()

  // ---- Layout. ----

  fun setSelectedScale(scale: Float) {
    selected?.scale = scale
    invalidate()
  }

  fun setSelectedHidden(hidden: Boolean) {
    selected?.visible = !hidden
    invalidate()
  }

  fun resetLayout() {
    controls.forEach { it.reset() }
    opacity = DEFAULT_OPACITY
    select(null)
  }

  fun toJson(): String {
    val root = JSONObject()
    for (c in controls) {
      root.put(
        c.id,
        JSONObject().put("x", c.x.toDouble()).put("y", c.y.toDouble())
          .put("s", c.scale.toDouble()).put("v", c.visible),
      )
    }
    return root.toString()
  }

  fun loadJson(json: String?) {
    if (json.isNullOrEmpty()) return
    val root = try {
      JSONObject(json)
    } catch (e: org.json.JSONException) {
      return
    }
    for (c in controls) {
      val o = root.optJSONObject(c.id) ?: continue
      c.x = o.optDouble("x", c.defX.toDouble()).toFloat().coerceIn(0f, 1f)
      c.y = o.optDouble("y", c.defY.toDouble()).toFloat().coerceIn(0f, 1f)
      c.scale = o.optDouble("s", 1.0).toFloat().coerceIn(MIN_SCALE, MAX_SCALE)
      c.visible = o.optBoolean("v", true)
    }
    invalidate()
  }

  /** Lifts every finger: no button stays held when the pad hides. */
  fun releaseAll() {
    tracks.clear()
    dragId = -1
    publish()
  }

  private fun select(c: TouchControl?) {
    selected = c
    onSelect(c)
    invalidate()
  }

  private fun cx(c: TouchControl) = c.x * width

  private fun cy(c: TouchControl) = c.y * height

  private fun widthPx(c: TouchControl) = c.baseDp * c.scale * density

  private fun heightPx(c: TouchControl) = if (c.wide) widthPx(c) * 0.55f else widthPx(c)

  // Nearest control whose (slightly enlarged) box holds the point.
  private fun hit(x: Float, y: Float, includeHidden: Boolean): TouchControl? {
    var best: TouchControl? = null
    var bestD = Float.MAX_VALUE
    for (c in controls) {
      if (!c.visible && !includeHidden) continue
      val slack = when (c.kind) {
        TouchKind.STICK -> 1.35f
        TouchKind.DPAD, TouchKind.FACE -> 1.15f
        else -> 1.25f
      }
      val dx = abs(x - cx(c)) / (widthPx(c) / 2 * slack)
      val dy = abs(y - cy(c)) / (heightPx(c) / 2 * slack)
      if (dx > 1f || dy > 1f) continue
      val d = dx * dx + dy * dy
      if (d < bestD) {
        bestD = d
        best = c
      }
    }
    return best
  }

  // ---- Touch. ----

  @SuppressLint("ClickableViewAccessibility")
  override fun onTouchEvent(e: MotionEvent): Boolean {
    when (e.actionMasked) {
      MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
        val i = e.actionIndex
        down(e.getPointerId(i), e.getX(i), e.getY(i))
      }
      MotionEvent.ACTION_MOVE -> {
        for (i in 0 until e.pointerCount) move(e.getPointerId(i), e.getX(i), e.getY(i))
      }
      MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> up(e.getPointerId(e.actionIndex))
      MotionEvent.ACTION_CANCEL -> {
        tracks.clear()
        dragId = -1
      }
    }
    if (!editing) publish() else invalidate()
    return true
  }

  private fun down(id: Int, x: Float, y: Float) {
    if (editing) {
      val c = hit(x, y, includeHidden = true)
      select(c)
      if (c != null && dragId < 0) {
        dragId = id
        dragDx = cx(c) - x
        dragDy = cy(c) - y
      }
      return
    }
    val c = hit(x, y, includeHidden = false) ?: return
    if (c.kind == TouchKind.BUTTON && c.mask == 0) {
      sink.guide()
      return
    }
    val now = SystemClock.uptimeMillis()
    val thumb = c.kind == TouchKind.STICK && now - (lastUp[c.id] ?: 0L) < DOUBLE_TAP_MS
    tracks[id] = Track(c, x, y, x, y, thumb)
  }

  private fun move(id: Int, x: Float, y: Float) {
    if (editing) {
      if (id != dragId) return
      val c = selected ?: return
      if (width == 0 || height == 0) return
      c.x = ((x + dragDx) / width).coerceIn(0f, 1f)
      c.y = ((y + dragDy) / height).coerceIn(0f, 1f)
      return
    }
    val t = tracks[id] ?: return
    t.x = x
    t.y = y
  }

  private fun up(id: Int) {
    if (editing) {
      if (id == dragId) dragId = -1
      return
    }
    val t = tracks.remove(id) ?: return
    lastUp[t.control.id] = SystemClock.uptimeMillis()
  }

  private fun publish() {
    var buttons = 0
    val axes = FloatArray(6)
    for (t in tracks.values) {
      val c = t.control
      val r = widthPx(c) / 2
      when (c.kind) {
        TouchKind.STICK -> {
          var dx = (t.x - t.ox) / (r * STICK_TRAVEL)
          var dy = (t.y - t.oy) / (r * STICK_TRAVEL)
          val len = hypot(dx, dy)
          if (len > 1f) {
            dx /= len
            dy /= len
          }
          axes[c.axis] = dx
          axes[c.axis + 1] = dy
          if (t.thumb) buttons = buttons or c.mask
        }
        TouchKind.DPAD -> buttons = buttons or dpadMask(t.x - cx(c), t.y - cy(c), r)
        TouchKind.FACE -> buttons = buttons or faceMask(t.x - cx(c), t.y - cy(c), widthPx(c))
        TouchKind.BUTTON -> buttons = buttons or c.mask
        TouchKind.TRIGGER -> axes[c.axis] = 1f
      }
    }
    val gained = buttons and sentButtons.inv()
    val changed = buttons xor sentButtons
    sentButtons = buttons
    for (bit in 0 until 16) {
      val m = 1 shl bit
      if (changed and m != 0) sink.button(m, buttons and m != 0)
    }
    var triggerPressed = false
    for (a in axes.indices) {
      if (axes[a] == sentAxes[a]) continue
      if (a >= 4 && axes[a] > 0f) triggerPressed = true
      sentAxes[a] = axes[a]
      sink.axis(a, axes[a])
    }
    if (haptics && (gained != 0 || triggerPressed)) {
      performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
    }
    invalidate()
  }

  // 8-way: sin(22.5 deg) ~ 0.38 splits the diagonals evenly.
  private fun dpadMask(dx: Float, dy: Float, r: Float): Int {
    val len = hypot(dx, dy)
    if (len < r * 0.2f) return 0
    val nx = dx / len
    val ny = dy / len
    var m = 0
    if (ny < -0.38f) m = m or 0x1
    if (ny > 0.38f) m = m or 0x2
    if (nx < -0.38f) m = m or 0x4
    if (nx > 0.38f) m = m or 0x8
    return m
  }

  // Nearest face button; a finger between two neighbours presses both.
  private fun faceMask(dx: Float, dy: Float, size: Float): Int {
    if (hypot(dx, dy) > size * 0.75f) return 0
    val o = size * FACE_OFFSET
    val d = FloatArray(4) { i -> hypot(dx - FACE_DX[i] * o, dy - FACE_DY[i] * o) }
    val order = d.indices.sortedBy { d[it] }
    var m = FACE_MASK[order[0]]
    if (hypot(dx, dy) > size * 0.2f && d[order[1]] < d[order[0]] * 1.2f) m = m or FACE_MASK[order[1]]
    return m
  }

  // ---- Drawing. ----

  override fun onDraw(canvas: Canvas) {
    val base = if (editing) max(opacity, 0.5f) else opacity
    for (c in controls) {
      if (!c.visible && !editing) continue
      val a = if (c.visible) base else base * 0.35f
      when (c.kind) {
        TouchKind.STICK -> drawStick(canvas, c, a)
        TouchKind.DPAD -> drawDpad(canvas, c, a)
        TouchKind.FACE -> drawFace(canvas, c, a)
        TouchKind.BUTTON, TouchKind.TRIGGER -> drawButton(canvas, c, a)
      }
      if (editing && c === selected) {
        stroke.color = SELECT_COLOR
        rect.set(
          cx(c) - widthPx(c) / 2 - 6, cy(c) - heightPx(c) / 2 - 6,
          cx(c) + widthPx(c) / 2 + 6, cy(c) + heightPx(c) / 2 + 6,
        )
        canvas.drawRoundRect(rect, 12f, 12f, stroke)
      }
    }
  }

  private fun setPaints(a: Float, pressed: Boolean, tint: Int = Color.WHITE) {
    fill.color = if (pressed) withAlpha(tint, a * 0.55f) else withAlpha(Color.BLACK, a * 0.45f)
    stroke.color = withAlpha(Color.WHITE, a * 0.85f)
  }

  private fun drawStick(canvas: Canvas, c: TouchControl, a: Float) {
    val r = widthPx(c) / 2
    val active = tracks.values.any { it.control === c }
    setPaints(a, false)
    canvas.drawCircle(cx(c), cy(c), r, fill)
    canvas.drawCircle(cx(c), cy(c), r, stroke)
    val kx = cx(c) + sentAxes[c.axis] * r * 0.55f
    val ky = cy(c) + sentAxes[c.axis + 1] * r * 0.55f
    setPaints(a, active || sentButtons and c.mask != 0)
    canvas.drawCircle(kx, ky, r * 0.45f, fill)
    canvas.drawCircle(kx, ky, r * 0.45f, stroke)
  }

  private fun drawDpad(canvas: Canvas, c: TouchControl, a: Float) {
    val r = widthPx(c) / 2
    val w = r * 0.36f
    val x = cx(c)
    val y = cy(c)
    // Up, down, left, right arms.
    val arms = arrayOf(
      floatArrayOf(x - w, y - r, x + w, y - w * 0.3f),
      floatArrayOf(x - w, y + w * 0.3f, x + w, y + r),
      floatArrayOf(x - r, y - w, x - w * 0.3f, y + w),
      floatArrayOf(x + w * 0.3f, y - w, x + r, y + w),
    )
    for (i in 0 until 4) {
      setPaints(a, sentButtons and (1 shl i) != 0)
      rect.set(arms[i][0], arms[i][1], arms[i][2], arms[i][3])
      canvas.drawRoundRect(rect, w * 0.3f, w * 0.3f, fill)
      canvas.drawRoundRect(rect, w * 0.3f, w * 0.3f, stroke)
    }
  }

  private fun drawFace(canvas: Canvas, c: TouchControl, a: Float) {
    val size = widthPx(c)
    val o = size * FACE_OFFSET
    val br = size * 0.19f
    text.textSize = br * 0.95f
    for (i in 0 until 4) {
      val x = cx(c) + FACE_DX[i] * o
      val y = cy(c) + FACE_DY[i] * o
      setPaints(a, sentButtons and FACE_MASK[i] != 0, FACE_COLOR[i])
      canvas.drawCircle(x, y, br, fill)
      canvas.drawCircle(x, y, br, stroke)
      text.color = withAlpha(FACE_COLOR[i], a.coerceAtLeast(0.35f))
      canvas.drawText(FACE_LABEL[i], x, y - (text.descent() + text.ascent()) / 2, text)
    }
  }

  private fun drawButton(canvas: Canvas, c: TouchControl, a: Float) {
    val pressed = when {
      c.kind == TouchKind.TRIGGER -> sentAxes[c.axis] > 0f
      c.mask != 0 -> sentButtons and c.mask != 0
      else -> false
    }
    setPaints(a, pressed)
    val w = widthPx(c) / 2
    val h = heightPx(c) / 2
    rect.set(cx(c) - w, cy(c) - h, cx(c) + w, cy(c) + h)
    canvas.drawRoundRect(rect, h, h, fill)
    canvas.drawRoundRect(rect, h, h, stroke)
    text.textSize = h * if (c.label.length > 2) 0.7f else 0.9f
    text.color = withAlpha(Color.WHITE, a.coerceAtLeast(0.35f))
    canvas.drawText(c.label, cx(c), cy(c) - (text.descent() + text.ascent()) / 2, text)
  }

  private fun withAlpha(color: Int, a: Float) =
    Color.argb((a.coerceIn(0f, 1f) * 255).toInt(), Color.red(color), Color.green(color), Color.blue(color))

  companion object {
    const val DEFAULT_OPACITY = 0.6f
    const val MIN_SCALE = 0.5f
    const val MAX_SCALE = 2f
    private const val DOUBLE_TAP_MS = 250L
    // Finger travel for full deflection, as a fraction of the stick radius.
    private const val STICK_TRAVEL = 0.8f
    private const val FACE_OFFSET = 0.31f
    private const val SELECT_COLOR = 0xFF7CFFB2.toInt()

    // Y top, A bottom, X left, B right.
    private val FACE_DX = floatArrayOf(0f, 0f, -1f, 1f)
    private val FACE_DY = floatArrayOf(-1f, 1f, 0f, 0f)
    private val FACE_MASK = intArrayOf(0x8000, 0x1000, 0x4000, 0x2000)
    private val FACE_LABEL = arrayOf("Y", "A", "X", "B")
    private val FACE_COLOR =
      intArrayOf(0xFFF2C12E.toInt(), 0xFF6BCB3A.toInt(), 0xFF3A86F0.toInt(), 0xFFE5483C.toInt())
  }
}
