package xdroid360.app

import kotlin.math.roundToInt

/**
 * DISP-2: votes the display refresh rate for the game surface from the measured
 * guest frame rate. A mission presents 30 frames/s while the panel ran at
 * 120 Hz with no vote at all; with a fixed-source vote the system can pick a
 * lower multiple of the content rate. Demand-driven: the vote follows the
 * rounded median of the last five 1 s samples (menus 60, missions 30) once
 * that value has held for three samples, and is cleared below 20 FPS
 * (loading, pause). A 10% hysteresis kept a 29 voted during a mission load
 * for the whole flight (2026-10-11), and 29 divides no panel rate.
 *
 * `xdroid_frame_rate_vote = true|false` in files/xdroid/live.toml (LiveFile)
 * switches it live for an on-device A/B (phone.sh liveab).
 */
class FrameRateVoter {
  @Volatile var enabled = true
  private var voted = 0f
  private var pending = 0f
  private var pendingCount = 0
  private val recent = ArrayDeque<Double>()

  /** The new vote (0 = no preference), or null to keep the current one. */
  @Synchronized
  fun update(fps: Double): Float? {
    recent.addLast(fps)
    while (recent.size > 5) recent.removeFirst()
    val target =
      if (!enabled) {
        0f
      } else if (recent.size < 5) {
        return null
      } else {
        val median = recent.sorted()[2]
        if (median < 20.0) 0f else median.roundToInt().toFloat()
      }
    if (target == voted) {
      pendingCount = 0
      return null
    }
    if (target == pending) pendingCount++ else {
      pending = target
      pendingCount = 1
    }
    // Switching the vote off (disabled) applies at once.
    if (enabled && pendingCount < 3) return null
    voted = target
    pendingCount = 0
    return target
  }

  /** A new Surface carries no vote: vote again on the next update. */
  @Synchronized
  fun reset() {
    voted = 0f
    pendingCount = 0
    recent.clear()
  }
}
