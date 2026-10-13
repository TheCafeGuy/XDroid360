package xdroid360.app

import android.content.Context
import android.os.Build
import android.os.PerformanceHintManager
import android.util.Log
import java.io.File

/**
 * ADPF performance hint (API 31+). Each tick reports the CPU time per frame
 * of the busiest hot thread (the frame's critical path) against the frame
 * period the game runs at, so the OS sizes clocks to the real demand. The
 * thread set is fixed per session, so it is rebuilt every 30 s as the hot
 * threads move. Call from one thread only.
 */
class PerfHint(private val context: Context) {
  private var session: PerformanceHintManager.Session? = null
  private var tids = IntArray(0)
  private var base = emptyMap<Int, Long>()
  private var targetNs = FRAME_60_NS
  private var ticksSinceBuild = REBUILD_TICKS
  // All-thread CPU snapshot one tick before a rebuild picks the hot set.
  private var rebuildBase: Map<Int, Long>? = null

  /** One sampler tick: [frames] presented during it, at [fps]. */
  fun update(frames: Long, fps: Double) {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return
    if (++ticksSinceBuild >= REBUILD_TICKS) {
      val previous = rebuildBase
      if (previous == null) {
        rebuildBase = threadTicks(null)
      } else {
        rebuild(previous)
        rebuildBase = null
        ticksSinceBuild = 0
      }
    }
    val current = session ?: return
    val snapshot = threadTicks(tids)
    var maxTicks = 0L
    for ((tid, ticks) in snapshot) {
      maxTicks = maxOf(maxTicks, ticks - (base[tid] ?: ticks))
    }
    base = snapshot
    // No frame this tick (paused, loading, stalled): dividing the whole
    // tick's CPU by one frame would ask the OS for boost clocks while
    // nothing is presented. Report nothing and let the default governor act.
    if (frames <= 0L) return
    try {
      val target = if (fps <= 32.0) FRAME_30_NS else FRAME_60_NS
      if (target != targetNs) {
        current.updateTargetWorkDuration(target)
        targetNs = target
      }
      val perFrameNs = (maxTicks * NS_PER_TICK / frames)
        .coerceIn(1_000_000L, 100_000_000L)
      current.reportActualWorkDuration(perFrameNs)
    } catch (e: Exception) {
      Log.d(TAG, "perf hint report failed: ${e.message}")
    }
  }

  /** For the forensic log (temp): on:<threads>:<target ms>, or off. */
  fun state(): String = session?.let { "on:${tids.size}:${targetNs / 1_000_000}" } ?: "off"

  fun close() {
    runCatching { session?.close() }
    session = null
  }

  private fun rebuild(previous: Map<Int, Long>) {
    close()
    val now = threadTicks(null)
    val hot =
      now.map { (tid, ticks) -> tid to ticks - (previous[tid] ?: ticks) }
        .filter { it.second > 0 }
        .sortedByDescending { it.second }
        .take(HOT_THREADS)
        .map { it.first }
        .toIntArray()
    if (hot.isEmpty()) return
    try {
      val manager = context.getSystemService(PerformanceHintManager::class.java) ?: return
      session = manager.createHintSession(hot, targetNs)
      tids = hot
      base = threadTicks(hot)
    } catch (e: Exception) {
      Log.d(TAG, "perf hint unavailable: ${e.message}")
    }
  }

  // utime + stime in clock ticks per thread; null = every thread.
  private fun threadTicks(only: IntArray?): Map<Int, Long> {
    val ids = only?.toList()
      ?: File("/proc/self/task").list()?.mapNotNull { it.toIntOrNull() }
      ?: return emptyMap()
    val out = HashMap<Int, Long>(ids.size)
    for (tid in ids) {
      try {
        // After "pid (comm) ", utime and stime (fields 14 and 15) are at 11
        // and 12. Split at the last ") ": thread names can hold parentheses.
        val parts = File("/proc/self/task/$tid/stat").readText()
          .substringAfterLast(") ").split(' ')
        if (parts.size > 12) out[tid] = parts[11].toLong() + parts[12].toLong()
      } catch (_: Exception) {
      }
    }
    return out
  }

  private companion object {
    const val TAG = "XDroid360"
    const val HOT_THREADS = 8
    const val REBUILD_TICKS = 30
    // USER_HZ is 100 on Android.
    const val NS_PER_TICK = 10_000_000L
    const val FRAME_30_NS = 33_333_333L
    const val FRAME_60_NS = 16_666_667L
  }
}
