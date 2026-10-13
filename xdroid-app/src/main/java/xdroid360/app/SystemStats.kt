package xdroid360.app

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import java.io.File

/**
 * Cheap once-per-second system sampler for the quick menu and the
 * XDROID-SYS log line: process/thread CPU, GPU busy, CPU cluster
 * clocks, thermal status/headroom, battery temperature, screen brightness
 * (to catch thermal dimming) and RSS. Every source is best effort: anything
 * the app may not read is simply left out.
 */
class SystemStats(private val context: Context) {
  private var lastProcTicks = -1L
  private var lastThreadTicks = HashMap<Int, Long>()
  private var lastSampleMs = 0L
  private val cores = Runtime.getRuntime().availableProcessors()
  private val clusterCpus: List<Int> by lazy { detectClusters() }

  private var processCpuPct = 0.0
  private var topThreads: List<Pair<String, Double>> = emptyList()
  private var gpuBusyPct: Double? = null
  private var clusterMhz: List<Int> = emptyList()
  private var thermalStatus = -1
  private var thermalHeadroom: Float? = null
  private var batteryTempC: Float? = null
  private var brightness: Int? = null
  private var rssMb = 0L

  fun sample() {
    val now = android.os.SystemClock.elapsedRealtime()
    val dtS = if (lastSampleMs > 0) (now - lastSampleMs) / 1000.0 else 0.0
    lastSampleMs = now

    readTicks("/proc/self/stat")?.let { ticks ->
      if (lastProcTicks >= 0 && dtS > 0) processCpuPct = (ticks - lastProcTicks) / 100.0 / dtS * 100.0
      lastProcTicks = ticks
    }

    val threads = HashMap<Int, Long>()
    val names = HashMap<Int, String>()
    File("/proc/self/task").listFiles()?.forEach { dir ->
      val tid = dir.name.toIntOrNull() ?: return@forEach
      val stat = try {
        File(dir, "stat").readText()
      } catch (_: Exception) {
        return@forEach
      }
      val name = stat.substringAfter('(').substringBeforeLast(')')
      val parts = stat.substringAfterLast(") ").split(' ')
      if (parts.size > 13) {
        threads[tid] = parts[11].toLong() + parts[12].toLong()
        names[tid] = name
      }
    }
    if (dtS > 0) {
      topThreads =
        threads.mapNotNull { (tid, t) ->
          val prev = lastThreadTicks[tid] ?: return@mapNotNull null
          (names[tid] ?: "?") to (t - prev) / 100.0 / dtS * 100.0
        }.sortedByDescending { it.second }.take(5)
    }
    lastThreadTicks = threads

    gpuBusyPct =
      try {
        val p = File("/sys/class/kgsl/kgsl-3d0/gpubusy").readText().trim().split(Regex("\\s+"))
        val busy = p[0].toDouble()
        val total = p[1].toDouble()
        if (total > 0) busy / total * 100.0 else null
      } catch (_: Exception) {
        null
      }

    clusterMhz =
      clusterCpus.mapNotNull { cpu ->
        try {
          File("/sys/devices/system/cpu/cpu$cpu/cpufreq/scaling_cur_freq").readText().trim().toInt() / 1000
        } catch (_: Exception) {
          null
        }
      }

    val pm = context.getSystemService(PowerManager::class.java)
    if (Build.VERSION.SDK_INT >= 29 && pm != null) thermalStatus = pm.currentThermalStatus
    if (Build.VERSION.SDK_INT >= 30 && pm != null) {
      thermalHeadroom = pm.getThermalHeadroom(10).takeIf { !it.isNaN() }
    }
    batteryTempC =
      context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        ?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
        ?.takeIf { it != Int.MIN_VALUE }
        ?.let { it / 10f }
    brightness =
      try {
        android.provider.Settings.System.getInt(context.contentResolver, android.provider.Settings.System.SCREEN_BRIGHTNESS)
      } catch (_: Exception) {
        null
      }
    rssMb =
      try {
        File("/proc/self/status").readLines().firstOrNull { it.startsWith("VmRSS:") }
          ?.split(Regex("\\s+"))?.getOrNull(1)?.toLong()?.div(1024) ?: 0L
      } catch (_: Exception) {
        0L
      }
  }

  private fun thermalLabel(): String =
    when (thermalStatus) {
      0 -> "cool"
      1 -> "light"
      2 -> "moderate"
      3 -> "severe"
      4 -> "critical"
      5 -> "emergency"
      6 -> "shutdown"
      else -> "?"
    }

  fun cpuText(): String = String.format(java.util.Locale.US, "CPU %.0f%%", processCpuPct)

  fun gpuText(): String = gpuBusyPct?.let { String.format(java.util.Locale.US, "GPU %.0f%%", it) } ?: ""

  fun thermalText(): String =
    buildString {
      batteryTempC?.let { append(String.format(java.util.Locale.US, "%.0f°C", it)) }
      if (thermalStatus >= 0) {
        if (isNotEmpty()) append(' ')
        append(thermalLabel())
      }
    }

  fun detailText(): String =
    buildString {
      append(topThreads.joinToString("  ") { (n, p) -> String.format(java.util.Locale.US, "%s %.0f%%", n.take(12), p) })
      if (clusterMhz.isNotEmpty()) append("\nclocks MHz ${clusterMhz.joinToString("/")}")
      thermalHeadroom?.let { append(String.format(java.util.Locale.US, "  headroom %.2f", it)) }
      brightness?.let { append("  bright $it") }
      append("  rss ${rssMb}MB")
    }

  fun logLine(fpsSummary: String): String =
    "XDROID-SYS $fpsSummary cpu=${"%.0f".format(processCpuPct)}% " +
      "gpu=${gpuBusyPct?.let { "%.0f".format(it) } ?: "na"}% " +
      "clocks=${clusterMhz.joinToString("/")} thermal=${thermalLabel()} " +
      "headroom=${thermalHeadroom ?: "na"} batt=${batteryTempC ?: "na"}C " +
      "bright=${brightness ?: "na"} rss=${rssMb}MB cores=$cores top=" +
      topThreads.joinToString(",") { (n, p) -> "${n.replace(' ', '_')}:${"%.0f".format(p)}" }

  private fun readTicks(path: String): Long? =
    try {
      val parts = File(path).readText().substringAfterLast(") ").split(' ')
      parts[11].toLong() + parts[12].toLong()
    } catch (_: Exception) {
      null
    }

  /** First CPU of each frequency domain. */
  private fun detectClusters(): List<Int> {
    val seen = HashSet<String>()
    val out = mutableListOf<Int>()
    for (cpu in 0 until cores) {
      val related =
        try {
          File("/sys/devices/system/cpu/cpu$cpu/cpufreq/related_cpus").readText().trim()
        } catch (_: Exception) {
          continue
        }
      if (seen.add(related)) out.add(cpu)
    }
    return out
  }
}
