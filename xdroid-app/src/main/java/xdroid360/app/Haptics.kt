package xdroid360.app

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager

/**
 * Touch pad feedback through the Vibrator, not performHapticFeedback: that
 * one follows the system "touch vibration" switch (off on many phones) and
 * is weak. Composition primitives (API 30+) when the motor supports them,
 * short one-shots otherwise. Our own toggle and strength decide.
 */
class Haptics(context: Context) {
  private val vibrator: Vibrator? =
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      context.getSystemService(VibratorManager::class.java)?.defaultVibrator
    } else {
      @Suppress("DEPRECATION")
      context.getSystemService(Vibrator::class.java)
    }

  private val primitives =
    Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
      vibrator?.areAllPrimitivesSupported(
        VibrationEffect.Composition.PRIMITIVE_CLICK,
        VibrationEffect.Composition.PRIMITIVE_TICK,
      ) == true

  var enabled = true
  var strength = 0.7f

  /** Button press. */
  fun click() = play(if (primitives) PRIM_CLICK else -1, 1f, 12)

  /** Release and D-pad direction change. */
  fun tick() = play(if (primitives) PRIM_TICK else -1, 0.5f, 6)

  /** Stick reached the edge of its ring. */
  fun edge() = play(if (primitives) PRIM_TICK else -1, 0.8f, 9)

  private fun play(primitive: Int, scale: Float, fallbackMs: Long) {
    val v = vibrator ?: return
    if (!enabled || !v.hasVibrator()) return
    val s = (strength * scale).coerceIn(0.05f, 1f)
    val effect =
      if (primitive >= 0 && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
        VibrationEffect.startComposition().addPrimitive(primitive, s).compose()
      } else {
        VibrationEffect.createOneShot(fallbackMs, (s * 255).toInt().coerceIn(1, 255))
      }
    v.vibrate(effect)
  }

  private companion object {
    const val PRIM_CLICK = 1 // VibrationEffect.Composition.PRIMITIVE_CLICK
    const val PRIM_TICK = 7 // VibrationEffect.Composition.PRIMITIVE_TICK
  }
}
