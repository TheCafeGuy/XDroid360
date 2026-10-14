package xdroid360.app

import android.graphics.Bitmap
import android.graphics.BlurMaskFilter
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin

/**
 * Our own vector art for the touch pad, in the spirit of an Xbox 360 pad
 * (domed face buttons, concave stick wells, D-pad disc, curved shoulders)
 * with no Microsoft logo or asset: the Guide button is a neutral ring with
 * three bars. Every control state is rendered once to a cached bitmap; the
 * view then only blits with its opacity, so a touch costs a few drawBitmap
 * calls instead of gradients and shadows every frame.
 */
class TouchArt(private val density: Float) {
  private val cache = HashMap<String, Bitmap>()
  private val p = Paint(Paint.ANTI_ALIAS_FLAG)
  private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    textAlign = Paint.Align.CENTER
    isFakeBoldText = true
  }
  private val rect = RectF()
  private val path = Path()

  fun clear() {
    cache.values.forEach { it.recycle() }
    cache.clear()
  }

  /** Margin around a control box for glows and shadows, in px. */
  fun margin(w: Float, h: Float) = max(w, h) * 0.18f + 4f

  /**
   * The cached bitmap for one state of a w x h px control. [draw] paints it
   * with the control box at (m, m)-(m + w, m + h).
   */
  fun bitmap(key: String, w: Float, h: Float, draw: Canvas.(m: Float) -> Unit): Bitmap {
    val m = margin(w, h)
    val k = "$key@${w.toInt()}x${h.toInt()}"
    cache[k]?.let { return it }
    val bmp = Bitmap.createBitmap((w + 2 * m).toInt().coerceAtLeast(1), (h + 2 * m).toInt().coerceAtLeast(1), Bitmap.Config.ARGB_8888)
    Canvas(bmp).draw(m)
    cache[k] = bmp
    return bmp
  }

  // ---- Face buttons: domed, translucent colour cap, embossed letter. ----

  fun faceButton(c: Canvas, cx: Float, cy: Float, r0: Float, tint: Int, label: String, pressed: Boolean) {
    val r = if (pressed) r0 * 0.94f else r0
    // Drop shadow.
    p.reset()
    p.isAntiAlias = true
    p.color = Color.argb(110, 0, 0, 0)
    p.maskFilter = BlurMaskFilter(r * 0.18f, BlurMaskFilter.Blur.NORMAL)
    c.drawCircle(cx, cy + r * 0.10f, r, p)
    p.maskFilter = null
    // Dome: light top-left, the colour cap darker at the rim.
    p.shader = RadialGradient(
      cx - r * 0.3f, cy - r * 0.35f, r * 1.4f,
      intArrayOf(lighten(tint, 0.55f), tint, darken(tint, 0.45f)),
      floatArrayOf(0f, 0.55f, 1f), Shader.TileMode.CLAMP,
    )
    p.alpha = if (pressed) 245 else 205
    c.drawCircle(cx, cy, r, p)
    p.shader = null
    // Rim.
    p.style = Paint.Style.STROKE
    p.strokeWidth = r * 0.07f
    p.color = Color.argb(150, 0, 0, 0)
    c.drawCircle(cx, cy, r * 0.97f, p)
    p.style = Paint.Style.FILL
    // Specular highlight.
    p.shader = LinearGradient(cx, cy - r, cx, cy, Color.argb(120, 255, 255, 255), Color.TRANSPARENT, Shader.TileMode.CLAMP)
    rect.set(cx - r * 0.62f, cy - r * 0.86f, cx + r * 0.62f, cy - r * 0.06f)
    c.drawOval(rect, p)
    p.shader = null
    // Letter: tinted, embossed (dark under), glowing when pressed.
    text.textSize = r * 1.05f
    val ty = cy - (text.descent() + text.ascent()) / 2
    text.clearShadowLayer()
    text.color = Color.argb(150, 0, 0, 0)
    c.drawText(label, cx, ty + r * 0.06f, text)
    text.color = if (pressed) lighten(tint, 0.6f) else lighten(tint, 0.25f)
    if (pressed) text.setShadowLayer(r * 0.35f, 0f, 0f, tint)
    c.drawText(label, cx, ty, text)
    text.clearShadowLayer()
  }

  // ---- Sticks: concave well, convex head with a notched grip ring. ----

  fun stickBase(c: Canvas, cx: Float, cy: Float, r: Float) {
    p.reset()
    p.isAntiAlias = true
    p.shader = RadialGradient(
      cx, cy, r,
      intArrayOf(Color.argb(200, 10, 10, 14), Color.argb(170, 30, 30, 38), Color.argb(150, 70, 70, 82)),
      floatArrayOf(0f, 0.8f, 1f), Shader.TileMode.CLAMP,
    )
    c.drawCircle(cx, cy, r, p)
    p.shader = null
    p.style = Paint.Style.STROKE
    p.strokeWidth = 2f * density
    p.color = Color.argb(150, 200, 200, 210)
    c.drawCircle(cx, cy, r - density, p)
    p.style = Paint.Style.FILL
  }

  fun knobShadow(c: Canvas, cx: Float, cy: Float, r: Float) {
    p.reset()
    p.isAntiAlias = true
    p.color = Color.argb(140, 0, 0, 0)
    p.maskFilter = BlurMaskFilter(r * 0.25f, BlurMaskFilter.Blur.NORMAL)
    c.drawCircle(cx, cy, r, p)
    p.maskFilter = null
  }

  fun stickKnob(c: Canvas, cx: Float, cy: Float, r: Float, pressed: Boolean) {
    p.reset()
    p.isAntiAlias = true
    val top = if (pressed) Color.rgb(150, 150, 160) else Color.rgb(118, 118, 128)
    p.shader = RadialGradient(
      cx - r * 0.3f, cy - r * 0.35f, r * 1.3f,
      intArrayOf(top, Color.rgb(60, 60, 68), Color.rgb(28, 28, 32)),
      floatArrayOf(0f, 0.65f, 1f), Shader.TileMode.CLAMP,
    )
    c.drawCircle(cx, cy, r, p)
    p.shader = null
    // Grip ring: fine notches on the rim.
    p.style = Paint.Style.STROKE
    p.strokeWidth = max(1f, r * 0.035f)
    p.color = Color.argb(150, 0, 0, 0)
    val n = 36
    for (i in 0 until n) {
      val a = (i * 2 * Math.PI / n).toFloat()
      c.drawLine(cx + cos(a) * r * 0.8f, cy + sin(a) * r * 0.8f, cx + cos(a) * r * 0.97f, cy + sin(a) * r * 0.97f, p)
    }
    // Concave thumb rest.
    p.style = Paint.Style.FILL
    p.shader = RadialGradient(cx, cy + r * 0.1f, r * 0.62f, Color.argb(90, 0, 0, 0), Color.argb(0, 0, 0, 0), Shader.TileMode.CLAMP)
    c.drawCircle(cx, cy, r * 0.62f, p)
    p.shader = null
    if (pressed) {
      p.style = Paint.Style.STROKE
      p.strokeWidth = 2f * density
      p.color = Color.argb(200, 124, 255, 178)
      c.drawCircle(cx, cy, r * 0.99f, p)
      p.style = Paint.Style.FILL
    }
  }

  // ---- D-pad: round plate with a raised cross. ----

  fun dpadBase(c: Canvas, cx: Float, cy: Float, r: Float) {
    p.reset()
    p.isAntiAlias = true
    p.shader = RadialGradient(
      cx, cy - r * 0.2f, r * 1.2f,
      intArrayOf(Color.argb(190, 64, 64, 72), Color.argb(190, 22, 22, 26)), null, Shader.TileMode.CLAMP,
    )
    c.drawCircle(cx, cy, r, p)
    p.shader = null
    p.style = Paint.Style.STROKE
    p.strokeWidth = 2f * density
    p.color = Color.argb(140, 200, 200, 210)
    c.drawCircle(cx, cy, r - density, p)
    p.style = Paint.Style.FILL
    crossPath(cx, cy, r * 0.86f, r * 0.30f)
    p.color = Color.argb(150, 0, 0, 0)
    p.maskFilter = BlurMaskFilter(r * 0.06f, BlurMaskFilter.Blur.NORMAL)
    c.save()
    c.translate(0f, r * 0.05f)
    c.drawPath(path, p)
    c.restore()
    p.maskFilter = null
    p.shader = LinearGradient(cx, cy - r, cx, cy + r, Color.rgb(110, 110, 120), Color.rgb(48, 48, 54), Shader.TileMode.CLAMP)
    c.drawPath(path, p)
    p.shader = null
    // Arrows on the arms.
    p.color = Color.argb(170, 230, 230, 235)
    for (dir in 0 until 4) arrow(c, cx, cy, r * 0.68f, r * 0.11f, dir)
  }

  /** Darkened arm overlay for a pressed direction (0 up, 1 down, 2 left, 3 right). */
  fun dpadArm(c: Canvas, cx: Float, cy: Float, r: Float, dir: Int) {
    p.reset()
    p.isAntiAlias = true
    val arm = r * 0.86f
    val half = r * 0.30f
    when (dir) {
      0 -> rect.set(cx - half, cy - arm, cx + half, cy - half * 0.6f)
      1 -> rect.set(cx - half, cy + half * 0.6f, cx + half, cy + arm)
      2 -> rect.set(cx - arm, cy - half, cx - half * 0.6f, cy + half)
      else -> rect.set(cx + half * 0.6f, cy - half, cx + arm, cy + half)
    }
    p.color = Color.argb(150, 0, 0, 0)
    c.drawRoundRect(rect, half * 0.4f, half * 0.4f, p)
    p.color = Color.argb(220, 124, 255, 178)
    arrow(c, cx, cy, r * 0.68f, r * 0.11f, dir)
  }

  private fun crossPath(cx: Float, cy: Float, arm: Float, half: Float) {
    path.reset()
    val rr = half * 0.4f
    rect.set(cx - half, cy - arm, cx + half, cy + arm)
    path.addRoundRect(rect, rr, rr, Path.Direction.CW)
    rect.set(cx - arm, cy - half, cx + arm, cy + half)
    path.addRoundRect(rect, rr, rr, Path.Direction.CW)
    path.fillType = Path.FillType.WINDING
  }

  private fun arrow(c: Canvas, cx: Float, cy: Float, dist: Float, s: Float, dir: Int) {
    val (dx, dy) = when (dir) {
      0 -> 0f to -1f
      1 -> 0f to 1f
      2 -> -1f to 0f
      else -> 1f to 0f
    }
    val tx = cx + dx * dist
    val ty = cy + dy * dist
    val t = Path()
    t.moveTo(tx + dx * s, ty + dy * s)
    t.lineTo(tx - dx * s + dy * s, ty - dy * s - dx * s)
    t.lineTo(tx - dx * s - dy * s, ty - dy * s + dx * s)
    t.close()
    c.drawPath(t, p)
  }

  // ---- Shoulders: curved bumpers, tall triggers that fill with the value. ----

  fun bumper(c: Canvas, l: Float, t: Float, w: Float, h: Float, label: String, right: Boolean, pressed: Boolean) {
    p.reset()
    p.isAntiAlias = true
    path.reset()
    // Pill whose top edge rises toward the outer side, like the shoulder.
    val inner = t + h * 0.35f
    path.moveTo(l + h * 0.5f, t + h)
    path.lineTo(l + w - h * 0.5f, t + h)
    path.quadTo(l + w, t + h, l + w, t + h * 0.5f)
    if (right) {
      path.cubicTo(l + w, t + h * 0.1f, l + w * 0.7f, t, l + w * 0.45f, t)
      path.cubicTo(l + w * 0.2f, t, l, inner, l, t + h * 0.6f)
    } else {
      path.cubicTo(l + w, inner, l + w * 0.8f, t, l + w * 0.55f, t)
      path.cubicTo(l + w * 0.3f, t, l, t + h * 0.1f, l, t + h * 0.5f)
    }
    path.quadTo(l, t + h, l + h * 0.5f, t + h)
    path.close()
    p.shader = LinearGradient(
      0f, t, 0f, t + h,
      if (pressed) Color.rgb(150, 150, 160) else Color.rgb(96, 96, 106),
      Color.rgb(30, 30, 36), Shader.TileMode.CLAMP,
    )
    p.alpha = 215
    c.drawPath(path, p)
    p.shader = null
    p.style = Paint.Style.STROKE
    p.strokeWidth = 1.5f * density
    p.color = if (pressed) Color.argb(220, 124, 255, 178) else Color.argb(150, 210, 210, 220)
    c.drawPath(path, p)
    p.style = Paint.Style.FILL
    label(c, label, l + w / 2, t + h * 0.62f, h * 0.42f)
  }

  /** Trigger body; with [filled] the lit version the view clips by value. */
  fun trigger(c: Canvas, l: Float, t: Float, w: Float, h: Float, label: String, right: Boolean, filled: Boolean) {
    p.reset()
    p.isAntiAlias = true
    path.reset()
    // Tall body, its outer top corner rounded like the trigger's curve.
    val big = w * 0.62f
    val small = w * 0.2f
    val bottom = w * 0.32f
    val radii =
      if (right) floatArrayOf(small, small, big, big, bottom, bottom, bottom, bottom)
      else floatArrayOf(big, big, small, small, bottom, bottom, bottom, bottom)
    rect.set(l, t, l + w, t + h)
    path.addRoundRect(rect, radii, Path.Direction.CW)
    p.shader = if (filled) {
      LinearGradient(0f, t, 0f, t + h, Color.rgb(124, 255, 178), Color.rgb(40, 150, 90), Shader.TileMode.CLAMP)
    } else {
      LinearGradient(l, 0f, l + w, 0f, Color.rgb(88, 88, 98), Color.rgb(34, 34, 40), Shader.TileMode.CLAMP)
    }
    p.alpha = if (filled) 200 else 215
    c.drawPath(path, p)
    p.shader = null
    p.style = Paint.Style.STROKE
    p.strokeWidth = 1.5f * density
    p.color = Color.argb(150, 210, 210, 220)
    c.drawPath(path, p)
    p.style = Paint.Style.FILL
    label(c, label, l + w / 2, t + h * 0.62f, w * 0.36f)
  }

  // ---- Small buttons: Back (◀), Start (▶), L3/R3, fast forward. ----

  fun pill(c: Canvas, l: Float, t: Float, w: Float, h: Float, glyph: String, pressed: Boolean) {
    p.reset()
    p.isAntiAlias = true
    rect.set(l, t, l + w, t + h)
    p.shader = LinearGradient(0f, t, 0f, t + h, if (pressed) Color.rgb(150, 150, 160) else Color.rgb(84, 84, 94), Color.rgb(28, 28, 34), Shader.TileMode.CLAMP)
    p.alpha = 210
    c.drawRoundRect(rect, h / 2, h / 2, p)
    p.shader = null
    p.style = Paint.Style.STROKE
    p.strokeWidth = 1.5f * density
    p.color = if (pressed) Color.argb(220, 124, 255, 178) else Color.argb(150, 210, 210, 220)
    c.drawRoundRect(rect, h / 2, h / 2, p)
    p.style = Paint.Style.FILL
    p.color = Color.argb(230, 235, 235, 240)
    val cx = l + w / 2
    val cy = t + h / 2
    val s = h * 0.22f
    when (glyph) {
      "back" -> arrowHead(c, cx + s * 0.3f, cy, s, left = true)
      "start" -> arrowHead(c, cx - s * 0.3f, cy, s, left = false)
      "ff" -> {
        arrowHead(c, cx - s * 0.9f, cy, s, left = false)
        arrowHead(c, cx + s * 0.5f, cy, s, left = false)
      }
      else -> label(c, glyph, cx, cy + h * 0.14f, h * 0.4f)
    }
  }

  private fun arrowHead(c: Canvas, x: Float, y: Float, s: Float, left: Boolean) {
    val d = if (left) -1f else 1f
    val t = Path()
    t.moveTo(x + d * s, y)
    t.lineTo(x - d * s * 0.7f, y - s)
    t.lineTo(x - d * s * 0.7f, y + s)
    t.close()
    c.drawPath(t, p)
  }

  /** Neutral guide: a ring with three bars and a soft green glow. */
  fun guide(c: Canvas, cx: Float, cy: Float, r: Float, pressed: Boolean) {
    p.reset()
    p.isAntiAlias = true
    p.shader = RadialGradient(cx, cy - r * 0.3f, r * 1.3f, Color.rgb(70, 70, 80), Color.rgb(18, 18, 22), Shader.TileMode.CLAMP)
    p.alpha = 220
    c.drawCircle(cx, cy, r, p)
    p.shader = null
    p.style = Paint.Style.STROKE
    p.strokeWidth = r * 0.12f
    p.color = if (pressed) Color.rgb(150, 255, 190) else Color.rgb(90, 200, 130)
    p.setShadowLayer(r * (if (pressed) 0.5f else 0.25f), 0f, 0f, Color.rgb(60, 220, 120))
    c.drawCircle(cx, cy, r * 0.8f, p)
    p.style = Paint.Style.FILL
    val bw = r * 0.62f
    val bh = r * 0.11f
    for (i in -1..1) {
      rect.set(cx - bw / 2, cy + i * r * 0.26f - bh / 2, cx + bw / 2, cy + i * r * 0.26f + bh / 2)
      c.drawRoundRect(rect, bh / 2, bh / 2, p)
    }
    p.clearShadowLayer()
  }

  private fun label(c: Canvas, s: String, x: Float, y: Float, size: Float) {
    text.textSize = size
    text.color = Color.argb(140, 0, 0, 0)
    c.drawText(s, x, y + size * 0.06f, text)
    text.color = Color.argb(235, 240, 240, 245)
    c.drawText(s, x, y, text)
  }

  private fun lighten(c: Int, f: Float) =
    Color.rgb(
      (Color.red(c) + (255 - Color.red(c)) * f).toInt(),
      (Color.green(c) + (255 - Color.green(c)) * f).toInt(),
      (Color.blue(c) + (255 - Color.blue(c)) * f).toInt(),
    )

  private fun darken(c: Int, f: Float) =
    Color.rgb((Color.red(c) * (1 - f)).toInt(), (Color.green(c) * (1 - f)).toInt(), (Color.blue(c) * (1 - f)).toInt())
}
