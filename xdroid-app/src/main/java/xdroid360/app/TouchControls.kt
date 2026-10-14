package xdroid360.app

import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.RectF
import android.os.SystemClock
import android.view.MotionEvent
import android.view.View
import android.view.WindowInsets
import java.util.Locale
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** Where the virtual pad sends its input; GameActivity routes it to Bridge. */
interface PadSink {
  fun button(mask: Int, down: Boolean)

  fun axis(axis: Int, value: Float)

  /** The on-screen Guide button: opens the quick menu. */
  fun guide()

  /** The on-screen fast-forward button (hidden by default). */
  fun setFastForward(on: Boolean)
}

/**
 * The virtual Xbox 360 pad: one view under the quick menu overlay, so the
 * stats box and the ⚑ flag keep their touches.
 *
 * Input: each finger owns the control it landed on (the D-pad and face
 * buttons follow a sliding finger; buttons too with "slide between
 * buttons"). Sticks are relative to where the finger lands, the origin
 * follows a finger that passes the ring, and the output goes through a
 * dead zone, an anti-dead zone (360 games apply their own dead zone on
 * XInput), a curve and a sensitivity. Output is the OR of all fingers, sent
 * only on change. Guide and fast forward act on release inside the button.
 *
 * Look: TouchArt bitmaps cached per state; a frame is a few blits. The pad
 * redraws only when its picture changes and dims after a while untouched.
 *
 * Edit mode: drag to move (grid and symmetry snap), pinch to scale, undo;
 * overlapping controls are outlined red.
 */
@SuppressLint("ViewConstructor")
class TouchPadView(
  context: Context,
  private val settings: ControlSettings,
  private val sink: PadSink,
  private val onSelect: (TouchControl?) -> Unit,
) : View(context) {
  val controls = TouchLayout.controls()
  private val byId = controls.associateBy { it.id }
  private val density = resources.displayMetrics.density
  private val art = TouchArt(density)
  val haptics = Haptics(context)

  var editing = false
    set(value) {
      if (field == value) return
      releaseAll()
      field = value
      select(null)
    }
  /** Editor "test": input live inside the editor, values shown. */
  var testMode = false
    set(value) {
      field = value
      invalidate()
    }
  var selected: TouchControl? = null
    private set
  /** Fast forward state, for the lit ▶▶ button and the toggle. */
  var fastForwardOn = false
    set(value) {
      field = value
      invalidate()
    }

  /** The fast-forward button follows its own setting, the rest the layout. */
  fun hidden(c: TouchControl) =
    if (c.kind == TouchKind.FAST_FORWARD) !settings.showFf.value else !c.visible

  private fun shown(c: TouchControl) = !hidden(c)

  /** Called after a layout edit (for the editor's undo state). */
  var onEdited: (() -> Unit)? = null

  private class Track(val control: TouchControl, var ox: Float, var oy: Float, var x: Float, var y: Float) {
    var thumb = false
    var floating = false
    var edge = false
    var slid = false
    var consumed = false
    var dir = 0
    // Raw stick deflection (-1..1) for the knob.
    var kx = 0f
    var ky = 0f
  }

  private val tracks = HashMap<Int, Track>()
  private val lastUp = HashMap<String, Long>()
  private val lastUpPos = HashMap<String, Pair<Float, Float>>()
  private var sentButtons = 0
  private val sentAxes = FloatArray(6)
  private val axes = FloatArray(6)
  private var lastLatencyMs = 0L
  private var lastTrackCount = 0

  // Edit state.
  private var dragId = -1
  private var dragDx = 0f
  private var dragDy = 0f
  private var dragX = 0f
  private var dragY = 0f
  private var pinchId = -1
  private var pinchX = 0f
  private var pinchY = 0f
  private var pinchDist = 0f
  private var pinchScale = 1f
  private val undoStack = ArrayDeque<String>()
  private var guideX = Float.NaN
  private var guideY = Float.NaN

  // Layout waiting for a measured size.
  private var pendingJson: String? = null
  private var pendingReset = true
  private var insetLeft = 0
  private var insetRight = 0

  // Auto-dim: 0 = normal, 1 = dimmed.
  private var dim = 0f
  private var dimAnim: ValueAnimator? = null
  private val dimRunnable = Runnable { animateDim(1f) }

  private val bmpPaint = Paint(Paint.FILTER_BITMAP_FLAG)
  private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    style = Paint.Style.STROKE
    strokeWidth = 2f * density
  }
  private val debugText = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    textSize = 11f * density
    color = Color.WHITE
    setShadowLayer(3f, 0f, 0f, Color.BLACK)
  }
  private val rect = RectF()
  private val rect2 = RectF()

  // ---- Layout. ----

  /** Saved layout JSON (null = preset); applied once the view has a size. */
  fun setLayout(json: String?) {
    pendingJson = json
    pendingReset = true
    applyPending()
  }

  fun layoutJson(): String = TouchLayout.toJson(controls)

  /** Back to the preset (screen size, preset choice, mirror). */
  fun resetLayout() {
    pushUndo()
    setLayout(null)
    select(null)
    onEdited?.invoke()
  }

  private fun applyPending() {
    if (!pendingReset || width == 0 || height == 0) return
    pendingReset = false
    TouchLayout.applyPreset(controls, settings.preset.value, settings.mirror.value, width, height, density)
    TouchLayout.fromJson(controls, pendingJson)
    pendingJson = null
    art.clear()
    invalidate()
  }

  override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
    super.onSizeChanged(w, h, oldw, oldh)
    applyPending()
  }

  override fun onApplyWindowInsets(insets: WindowInsets): WindowInsets {
    val cutout = insets.displayCutout
    insetLeft = cutout?.safeInsetLeft ?: 0
    insetRight = cutout?.safeInsetRight ?: 0
    invalidate()
    return super.onApplyWindowInsets(insets)
  }

  fun setSelectedScale(scale: Float) {
    selected?.scale = scale
    invalidate()
  }

  fun setSelectedHidden(hidden: Boolean) {
    pushUndo()
    val c = selected
    if (c?.kind == TouchKind.FAST_FORWARD) settings.showFf.value = !hidden else c?.visible = !hidden
    invalidate()
    onEdited?.invoke()
  }

  fun setSelectedAlpha(alpha: Float) {
    selected?.alpha = alpha
    invalidate()
  }

  /** Snapshot for undo before a slider drag starts. */
  fun beginEdit() = pushUndo()

  val canUndo get() = undoStack.isNotEmpty()

  fun undo() {
    val json = undoStack.removeLastOrNull() ?: return
    TouchLayout.fromJson(controls, json)
    select(selected)
    invalidate()
    onEdited?.invoke()
  }

  private fun pushUndo() {
    undoStack.addLast(layoutJson())
    while (undoStack.size > 30) undoStack.removeFirst()
  }

  /** Lifts every finger: no button stays held when the pad hides. */
  fun releaseAll() {
    if (tracks.values.any { it.control.kind == TouchKind.FAST_FORWARD } && settings.ffMode.value == "hold") {
      sink.setFastForward(false)
    }
    tracks.clear()
    dragId = -1
    pinchId = -1
    publish(0L)
  }

  /** Settings changed (menu or live.toml): redraw with them. */
  fun settingsChanged() {
    haptics.enabled = settings.haptics.value
    haptics.strength = settings.hapticStrength.value
    if (!settings.autoDim.value) animateDim(0f) else scheduleDim()
    invalidate()
  }

  private fun select(c: TouchControl?) {
    selected = c
    onSelect(c)
    invalidate()
  }

  // ---- Geometry. ----

  private fun widthPx(c: TouchControl) = c.baseDp * c.scale * density

  private fun heightPx(c: TouchControl) =
    when {
      c.kind == TouchKind.TRIGGER -> widthPx(c) * 1.3f
      c.wide -> widthPx(c) * 0.55f
      else -> widthPx(c)
    }

  // Centers keep the whole control inside the cutout insets and the edge
  // margin (curved screens).
  private fun cx(c: TouchControl): Float {
    val half = widthPx(c) / 2
    val lo = insetLeft + settings.edgeMargin.value * density + half
    val hi = width - insetRight - settings.edgeMargin.value * density - half
    return if (lo > hi) width / 2f else (c.x * width).coerceIn(lo, hi)
  }

  private fun cy(c: TouchControl): Float {
    val half = heightPx(c) / 2
    return if (half * 2 > height) height / 2f else (c.y * height).coerceIn(half, height - half)
  }

  private fun box(c: TouchControl, out: RectF, slack: Float = 1f) {
    val hw = widthPx(c) / 2 * slack
    val hh = heightPx(c) / 2 * slack
    out.set(cx(c) - hw, cy(c) - hh, cx(c) + hw, cy(c) + hh)
  }

  // Nearest control whose (slightly enlarged) box holds the point.
  private fun hit(x: Float, y: Float, includeHidden: Boolean, filter: (TouchControl) -> Boolean = { true }): TouchControl? {
    var best: TouchControl? = null
    var bestD = Float.MAX_VALUE
    for (c in controls) {
      if (hidden(c) && !includeHidden) continue
      if (!filter(c)) continue
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

  private fun inside(c: TouchControl, x: Float, y: Float): Boolean {
    box(c, rect, 1.25f)
    return rect.contains(x, y)
  }

  // ---- Touch. ----

  @SuppressLint("ClickableViewAccessibility")
  override fun onTouchEvent(e: MotionEvent): Boolean {
    when (e.actionMasked) {
      MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
        // Deliver moves as they come instead of batched at vsync.
        if (settings.lowLatency.value) requestUnbufferedDispatch(e)
        val i = e.actionIndex
        down(e.getPointerId(i), e.getX(i), e.getY(i), e.getTouchMajor(i))
      }
      MotionEvent.ACTION_MOVE -> {
        for (i in 0 until e.pointerCount) move(e.getPointerId(i), e.getX(i), e.getY(i))
      }
      MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
        val i = e.actionIndex
        up(e.getPointerId(i), e.getX(i), e.getY(i))
      }
      MotionEvent.ACTION_CANCEL -> {
        tracks.clear()
        dragId = -1
        pinchId = -1
      }
    }
    if (editing) {
      invalidate()
    } else {
      publish(e.eventTime)
    }
    wake()
    return true
  }

  private fun down(id: Int, x: Float, y: Float, touchMajor: Float) {
    // Palm: a contact patch wider than any thumb.
    val mm = touchMajor / resources.displayMetrics.xdpi * 25.4f
    if (settings.palmReject.value && mm > PALM_MM) return
    if (editing) {
      editDown(id, x, y)
      return
    }
    var c = hit(x, y, includeHidden = false)
    var floating = false
    val ls = byId.getValue("ls")
    if (c == null && settings.floatingLeft.value && ls.visible && x < width / 3f &&
      tracks.values.none { it.control === ls }
    ) {
      c = ls
      floating = true
    }
    c ?: return
    val t = Track(c, x, y, x, y)
    t.floating = floating
    when (c.kind) {
      TouchKind.STICK -> {
        val prev = lastUpPos[c.id]
        val r = widthPx(c) / 2
        t.thumb = settings.stickDoubleTap.value &&
          SystemClock.uptimeMillis() - (lastUp[c.id] ?: 0L) < DOUBLE_TAP_MS &&
          prev != null && hypot(prev.first - x, prev.second - y) < r * 0.35f
      }
      TouchKind.FAST_FORWARD -> if (settings.ffMode.value == "hold") sink.setFastForward(true)
      TouchKind.GUIDE -> if (settings.guideLongPress.value) {
        postDelayed({
          if (tracks[id] === t && !t.slid && !t.consumed) {
            t.consumed = true
            haptics.click()
            sink.guide()
          }
        }, LONG_PRESS_MS)
      }
      else -> {}
    }
    tracks[id] = t
  }

  private fun move(id: Int, x: Float, y: Float) {
    if (editing) {
      editMove(id, x, y)
      return
    }
    val t = tracks[id] ?: return
    t.x = x
    t.y = y
    val c = t.control
    when (c.kind) {
      TouchKind.GUIDE, TouchKind.FAST_FORWARD -> if (!inside(c, x, y)) t.slid = true
      TouchKind.BUTTON, TouchKind.TRIGGER -> if (settings.slideButtons.value && !inside(c, x, y)) {
        val next = hit(x, y, includeHidden = false) { it.kind == TouchKind.BUTTON || it.kind == TouchKind.TRIGGER }
        if (next != null && next !== c) tracks[id] = Track(next, x, y, x, y)
      }
      else -> {}
    }
  }

  private fun up(id: Int, x: Float, y: Float) {
    if (editing) {
      editUp(id)
      return
    }
    val t = tracks.remove(id) ?: return
    val c = t.control
    when (c.kind) {
      TouchKind.STICK -> {
        lastUp[c.id] = SystemClock.uptimeMillis()
        lastUpPos[c.id] = x to y
      }
      TouchKind.GUIDE ->
        if (!settings.guideLongPress.value && !t.slid && inside(c, x, y)) {
          haptics.click()
          sink.guide()
        }
      TouchKind.FAST_FORWARD ->
        if (settings.ffMode.value == "hold") {
          sink.setFastForward(false)
        } else if (!t.slid && inside(c, x, y)) {
          haptics.click()
          sink.setFastForward(!fastForwardOn)
        }
      else -> {}
    }
  }

  private fun publish(eventTime: Long) {
    var buttons = 0
    axes.fill(0f)
    var anyStick = false
    for (t in tracks.values) {
      val c = t.control
      when (c.kind) {
        TouchKind.STICK -> {
          anyStick = true
          stick(t)
          if (t.thumb) buttons = buttons or c.mask
        }
        TouchKind.DPAD -> {
          val m = dpadMask(t.x - cx(c), t.y - cy(c), widthPx(c) / 2)
          if (m != t.dir && m != 0) haptics.tick()
          t.dir = m
          buttons = buttons or m
        }
        TouchKind.FACE -> buttons = buttons or faceMask(t.x - cx(c), t.y - cy(c), widthPx(c))
        TouchKind.BUTTON -> buttons = buttons or c.mask
        TouchKind.TRIGGER -> {
          axes[c.axis] =
            if (settings.analogTriggers.value) {
              // Finger lower on the trigger = pulled further.
              val top = cy(c) - heightPx(c) / 2
              0.25f + 0.75f * ((t.y - top) / heightPx(c)).coerceIn(0f, 1f)
            } else {
              1f
            }
        }
        TouchKind.GUIDE, TouchKind.FAST_FORWARD -> {}
      }
    }
    val gained = buttons and sentButtons.inv()
    val lost = sentButtons and buttons.inv()
    val changed = buttons xor sentButtons
    sentButtons = buttons
    for (bit in 0 until 16) {
      val m = 1 shl bit
      if (changed and m != 0) sink.button(m, buttons and m != 0)
    }
    var triggerPressed = false
    var axisChanged = false
    for (a in axes.indices) {
      if (axes[a] == sentAxes[a]) continue
      if (a >= 4 && sentAxes[a] == 0f && axes[a] > 0f) triggerPressed = true
      sentAxes[a] = axes[a]
      sink.axis(a, axes[a])
      axisChanged = true
    }
    if (gained != 0 || triggerPressed) {
      haptics.click()
    } else if (lost and 0xF.inv() != 0) {
      haptics.tick()
    }
    if (eventTime > 0) lastLatencyMs = SystemClock.uptimeMillis() - eventTime
    // Redraw only when the picture changes.
    if (changed != 0 || axisChanged || anyStick || tracks.size != lastTrackCount || debugOn()) invalidate()
    lastTrackCount = tracks.size
  }

  private fun stick(t: Track) {
    val c = t.control
    val travel = widthPx(c) / 2 * STICK_TRAVEL
    var dx = t.x - t.ox
    var dy = t.y - t.oy
    var len = hypot(dx, dy)
    if (len > travel) {
      if (!t.edge) haptics.edge()
      t.edge = true
      if (settings.stickFollow.value) {
        // Drag the origin: a thumb that overshoots does not saturate forever.
        t.ox = t.x - dx / len * travel
        t.oy = t.y - dy / len * travel
        dx = t.x - t.ox
        dy = t.y - t.oy
        len = travel
      }
    } else if (len < travel * 0.9f) {
      t.edge = false
    }
    val mag = min(len / travel, 1f)
    t.kx = if (len > 0f) dx / len * mag else 0f
    t.ky = if (len > 0f) dy / len * mag else 0f
    val shaped = shape(mag)
    axes[c.axis] = if (mag > 0f) t.kx / mag * shaped else 0f
    axes[c.axis + 1] = if (mag > 0f) t.ky / mag * shaped else 0f
  }

  private fun shape(m: Float): Float {
    val dz = settings.deadzone.value
    if (m <= dz) return 0f
    var s = (m - dz) / (1f - dz)
    if (settings.curve.value == "exponential") s *= s
    s = min(1f, s * settings.sensitivity.value)
    val anti = settings.antiDeadzone.value
    return anti + (1f - anti) * s
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
    var best = 0
    var second = 0
    var bestD = Float.MAX_VALUE
    var secondD = Float.MAX_VALUE
    for (i in 0 until 4) {
      val d = hypot(dx - FACE_DX[i] * o, dy - FACE_DY[i] * o)
      if (d < bestD) {
        second = best
        secondD = bestD
        best = i
        bestD = d
      } else if (d < secondD) {
        second = i
        secondD = d
      }
    }
    var m = FACE_MASK[best]
    if (hypot(dx, dy) > size * 0.2f && secondD < bestD * 1.2f) m = m or FACE_MASK[second]
    return m
  }

  // ---- Auto-dim. ----

  private fun wake() {
    removeCallbacks(dimRunnable)
    if (dim > 0f) {
      dimAnim?.cancel()
      dim = 0f
      invalidate()
    }
    scheduleDim()
  }

  private fun scheduleDim() {
    removeCallbacks(dimRunnable)
    if (settings.autoDim.value && !editing) {
      postDelayed(dimRunnable, (settings.autoDimSeconds.value * 1000).toLong())
    }
  }

  private fun animateDim(target: Float) {
    if (target > 0f && (tracks.isNotEmpty() || editing)) {
      scheduleDim()
      return
    }
    dimAnim?.cancel()
    dimAnim = ValueAnimator.ofFloat(dim, target).apply {
      duration = 400
      addUpdateListener {
        dim = it.animatedValue as Float
        invalidate()
      }
      start()
    }
  }

  override fun onDetachedFromWindow() {
    removeCallbacks(dimRunnable)
    dimAnim?.cancel()
    art.clear()
    super.onDetachedFromWindow()
  }

  // ---- Edit mode. ----

  private fun editDown(id: Int, x: Float, y: Float) {
    val sel = selected
    if (dragId >= 0 && sel != null && pinchId < 0) {
      // Second finger: pinch the selected control.
      pinchId = id
      pinchX = x
      pinchY = y
      pinchDist = hypot(pinchX - dragX, pinchY - dragY)
      pinchScale = sel.scale
      return
    }
    val c = hit(x, y, includeHidden = true)
    select(c)
    if (c != null) {
      pushUndo()
      dragId = id
      dragX = x
      dragY = y
      dragDx = cx(c) - x
      dragDy = cy(c) - y
    }
  }

  private fun editMove(id: Int, x: Float, y: Float) {
    val c = selected ?: return
    if (width == 0 || height == 0) return
    when (id) {
      pinchId -> {
        pinchX = x
        pinchY = y
      }
      dragId -> {
        dragX = x
        dragY = y
      }
      else -> return
    }
    if (pinchId >= 0) {
      if (pinchDist > 0f) {
        c.scale = (pinchScale * hypot(pinchX - dragX, pinchY - dragY) / pinchDist).coerceIn(MIN_SCALE, MAX_SCALE)
        onSelect(c)
      }
      return
    }
    var px = x + dragDx
    var py = y + dragDy
    if (settings.gridSnap.value) {
      val g = GRID_DP * density
      px = (px / g).roundToInt() * g
      py = (py / g).roundToInt() * g
    }
    var nx = (px / width).coerceIn(0f, 1f)
    var ny = (py / height).coerceIn(0f, 1f)
    // Symmetry: the screen center, a mirror of another control, its row.
    guideX = Float.NaN
    guideY = Float.NaN
    val tol = SNAP_DP * density
    if (abs(nx * width - width / 2f) < tol) {
      nx = 0.5f
      guideX = width / 2f
    }
    for (o in controls) {
      if (o === c || hidden(o)) continue
      if (guideX.isNaN() && abs(nx - (1f - o.x)) * width < tol) {
        nx = 1f - o.x
        guideX = nx * width
      }
      if (guideY.isNaN() && abs(ny - o.y) * height < tol) {
        ny = o.y
        guideY = ny * height
      }
    }
    c.x = nx
    c.y = ny
  }

  private fun editUp(id: Int) {
    if (id == pinchId) {
      pinchId = -1
      onEdited?.invoke()
      return
    }
    if (id == dragId) {
      dragId = -1
      pinchId = -1
      guideX = Float.NaN
      guideY = Float.NaN
      onEdited?.invoke()
    }
  }

  // ---- Drawing. ----

  private fun debugOn() = settings.debugOverlay.value || testMode

  override fun onDraw(canvas: Canvas) {
    val base = settings.opacity.value
    val global = if (editing) max(base, 0.5f) else base + (min(base, DIM_OPACITY) - base) * dim
    for (c in controls) {
      if (hidden(c) && !editing) continue
      val a = (global * c.alpha * if (shown(c)) 1f else 0.35f).coerceIn(0f, 1f)
      bmpPaint.alpha = (a * 255).toInt()
      when (c.kind) {
        TouchKind.STICK -> drawStick(canvas, c)
        TouchKind.DPAD -> drawDpad(canvas, c)
        TouchKind.FACE -> drawFace(canvas, c)
        TouchKind.TRIGGER -> drawTrigger(canvas, c)
        TouchKind.BUTTON, TouchKind.GUIDE, TouchKind.FAST_FORWARD -> drawButton(canvas, c)
      }
    }
    if (editing) drawEditor(canvas)
    if (debugOn()) drawDebug(canvas)
  }

  private fun blit(canvas: Canvas, bmp: Bitmap, x: Float, y: Float) {
    canvas.drawBitmap(bmp, x - bmp.width / 2f, y - bmp.height / 2f, bmpPaint)
  }

  private fun drawStick(canvas: Canvas, c: TouchControl) {
    val w = widthPx(c)
    val r = w / 2
    val t = tracks.values.firstOrNull { it.control === c }
    val bx = if (t?.floating == true) t.ox else cx(c)
    val by = if (t?.floating == true) t.oy else cy(c)
    blit(canvas, art.bitmap("stickbase", w, w) { m -> art.stickBase(this, m + r, m + r, r) }, bx, by)
    val kr = r * 0.48f
    val kdx = t?.kx ?: 0f
    val kdy = t?.ky ?: 0f
    // The knob follows the finger out to the ring; its shadow falls opposite.
    val kx = bx + kdx * r * 0.72f
    val ky = by + kdy * r * 0.72f
    val shadow = art.bitmap("knobshadow", kr * 2, kr * 2) { m -> art.knobShadow(this, m + kr, m + kr, kr) }
    blit(canvas, shadow, kx - kdx * kr * 0.18f, ky + kr * 0.12f - kdy * kr * 0.18f)
    val pressed = t != null
    val knob = art.bitmap(if (pressed) "knob1" else "knob0", kr * 2, kr * 2) { m ->
      art.stickKnob(this, m + kr, m + kr, kr, pressed)
    }
    blit(canvas, knob, kx, ky)
  }

  private fun drawDpad(canvas: Canvas, c: TouchControl) {
    val w = widthPx(c)
    val r = w / 2
    blit(canvas, art.bitmap("dpad", w, w) { m -> art.dpadBase(this, m + r, m + r, r) }, cx(c), cy(c))
    for (dir in 0 until 4) {
      if (sentButtons and (1 shl dir) == 0) continue
      blit(canvas, art.bitmap("dpadarm$dir", w, w) { m -> art.dpadArm(this, m + r, m + r, r, dir) }, cx(c), cy(c))
    }
  }

  private fun drawFace(canvas: Canvas, c: TouchControl) {
    val size = widthPx(c)
    val o = size * FACE_OFFSET
    val br = size * 0.19f
    for (i in 0 until 4) {
      val pressed = sentButtons and FACE_MASK[i] != 0
      val bmp = art.bitmap("face$i${if (pressed) 1 else 0}", br * 2, br * 2) { m ->
        art.faceButton(this, m + br, m + br, br, FACE_COLOR[i], FACE_LABEL[i], pressed)
      }
      blit(canvas, bmp, cx(c) + FACE_DX[i] * o, cy(c) + FACE_DY[i] * o)
    }
  }

  private fun drawTrigger(canvas: Canvas, c: TouchControl) {
    val w = widthPx(c)
    val h = heightPx(c)
    val right = c.id == "rt"
    val label = if (right) "RT" else "LT"
    val bmp = art.bitmap("trig$right", w, h) { m -> art.trigger(this, m, m, w, h, label, right, false) }
    blit(canvas, bmp, cx(c), cy(c))
    val v = sentAxes[c.axis]
    if (v > 0f) {
      // The lit body rises with the trigger value.
      val lit = art.bitmap("trigfill$right", w, h) { m -> art.trigger(this, m, m, w, h, label, right, true) }
      canvas.save()
      canvas.clipRect(0f, cy(c) + h / 2 - h * v, width.toFloat(), height.toFloat())
      blit(canvas, lit, cx(c), cy(c))
      canvas.restore()
    }
  }

  private fun drawButton(canvas: Canvas, c: TouchControl) {
    val w = widthPx(c)
    val h = heightPx(c)
    val pressed = when (c.kind) {
      TouchKind.BUTTON -> sentButtons and c.mask != 0
      TouchKind.FAST_FORWARD -> fastForwardOn || tracks.values.any { it.control === c && !it.slid }
      else -> tracks.values.any { it.control === c && !it.slid }
    }
    val p = if (pressed) 1 else 0
    val bmp = when {
      c.kind == TouchKind.GUIDE -> art.bitmap("guide$p", w, w) { m -> art.guide(this, m + w / 2, m + w / 2, w / 2, pressed) }
      c.id == "lb" || c.id == "rb" ->
        art.bitmap("${c.id}$p", w, h) { m -> art.bumper(this, m, m, w, h, c.id.uppercase(Locale.ROOT), c.id == "rb", pressed) }
      c.kind == TouchKind.FAST_FORWARD -> art.bitmap("ff$p", w, h) { m -> art.pill(this, m, m, w, h, "ff", pressed) }
      c.id == "back" || c.id == "start" -> art.bitmap("${c.id}$p", w, h) { m -> art.pill(this, m, m, w, h, c.id, pressed) }
      else -> art.bitmap("${c.id}$p", w, h) { m -> art.pill(this, m, m, w, h, c.id.uppercase(Locale.ROOT), pressed) }
    }
    blit(canvas, bmp, cx(c), cy(c))
  }

  private fun drawEditor(canvas: Canvas) {
    // Overlaps in red, the selection in green, snap guides dashed.
    for (c in controls) {
      if (hidden(c)) continue
      box(c, rect)
      val overlap = controls.any { o ->
        o !== c && shown(o) && run {
          box(o, rect2)
          RectF.intersects(rect, rect2)
        }
      }
      if (overlap) {
        line.color = Color.argb(220, 255, 80, 80)
        canvas.drawRoundRect(rect, 10f, 10f, line)
      }
    }
    selected?.let { c ->
      box(c, rect)
      rect.inset(-6f, -6f)
      line.color = SELECT_COLOR
      canvas.drawRoundRect(rect, 12f, 12f, line)
    }
    line.color = Color.argb(160, 124, 255, 178)
    line.pathEffect = DashPathEffect(floatArrayOf(10f * density, 6f * density), 0f)
    if (!guideX.isNaN()) canvas.drawLine(guideX, 0f, guideX, height.toFloat(), line)
    if (!guideY.isNaN()) canvas.drawLine(0f, guideY, width.toFloat(), guideY, line)
    line.pathEffect = null
  }

  private fun drawDebug(canvas: Canvas) {
    val s = String.format(
      Locale.US,
      "L %+.2f %+.2f  R %+.2f %+.2f  LT %.2f RT %.2f  btn %04X  %d ms",
      sentAxes[0], sentAxes[1], sentAxes[2], sentAxes[3], sentAxes[4], sentAxes[5], sentButtons, lastLatencyMs,
    )
    canvas.drawText(s, width * 0.3f, height * 0.12f, debugText)
  }

  companion object {
    const val DEFAULT_OPACITY = 0.6f
    const val MIN_SCALE = 0.5f
    const val MAX_SCALE = 2f
    private const val DIM_OPACITY = 0.2f
    private const val DOUBLE_TAP_MS = 250L
    private const val LONG_PRESS_MS = 500L
    private const val PALM_MM = 22f
    private const val GRID_DP = 8f
    private const val SNAP_DP = 10f
    // Finger travel for full deflection, as a fraction of the stick radius.
    private const val STICK_TRAVEL = 0.8f
    private const val FACE_OFFSET = 0.31f
    private val SELECT_COLOR = 0xFF7CFFB2.toInt()

    // Y top, A bottom, X left, B right.
    private val FACE_DX = floatArrayOf(0f, 0f, -1f, 1f)
    private val FACE_DY = floatArrayOf(-1f, 1f, 0f, 0f)
    private val FACE_MASK = intArrayOf(0x8000, 0x1000, 0x4000, 0x2000)
    private val FACE_LABEL = arrayOf("Y", "A", "X", "B")
    private val FACE_COLOR =
      intArrayOf(0xFFF2C12E.toInt(), 0xFF6BCB3A.toInt(), 0xFF3A86F0.toInt(), 0xFFE5483C.toInt())
  }
}
