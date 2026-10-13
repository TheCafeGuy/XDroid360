package xdroid360.app

import android.app.ActivityManager
import android.app.GameManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.Environment
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import android.view.Display
import java.io.File
import java.io.Writer
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * Forensic recorder for remote testers without adb (temp, revert-planned).
 * One file per game session, complete on its own:
 * Download/XDroid360-forensics-<session>.zip. A killed process runs no code,
 * so the zip is kept current instead of being written only at the end: on
 * every stop, every 15 s, every 5 s while the game shows no frames, and
 * synchronously before the app ends a wedged session (with that session's
 * crash report inside). Nothing is added to a later session's zip. The
 * working folder (files/xdroid/forensics/<session>, last 5 kept) holds:
 *   device.txt  static facts: model, SoC, OS skin, CPU policies with their
 *               limits, GPU sysfs and thermal zones the app may read,
 *               display modes, power/game modes, cpuset, driver;
 *   sys.log     one line per second (format in the analyzer,
 *               tools/device/forensum.py): FPS, process CPU, clock and cap
 *               per CPU policy, GPU sysfs when readable, thermal status and
 *               headroom, battery current/voltage/temperature, display Hz,
 *               ADPF state, and the busiest threads with CPU %, run-queue
 *               wait % and last core; plus lifecycle and glitch events;
 *   xdroid.log  the emulator log ("Frame stats": GPU busy per frame);
 *   logcat.txt  this app's logcat (driver, Vulkan, ADPF, crashes);
 *   the config files and the glitch marks.
 * Every source is best effort: what the OS denies is logged as such.
 */
class Forensics(private val context: Context, private val gamePath: String) {
  private val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
  private val workRoot = File(context.filesDir, "xdroid/forensics")
  private val dir = File(workRoot, stamp)
  private val startWallMs = System.currentTimeMillis()
  val zipFile =
    File(
      Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
      "XDroid360-forensics-$stamp.zip",
    )
  private val io =
    Executors.newSingleThreadExecutor { r ->
      Thread(r, "xdroid-forensics").apply { priority = Thread.MIN_PRIORITY }
    }
  private val startMs = SystemClock.elapsedRealtime()
  private var sysLog: Writer? = null
  // A refresh is queued or running: optional refreshes are skipped.
  private val pending = AtomicBoolean(false)

  private class Policy(val cpus: String, val path: String)

  private class ThreadSample(val ticks: Long, val runNs: Long, val waitNs: Long)

  private val cores = Runtime.getRuntime().availableProcessors()
  private val policies: List<Policy> = detectPolicies()
  private val gpuFiles: List<Pair<String, File>> = GPU_FILES.map { (k, p) -> k to File(p) }.filter { readOrNull(it.second) != null }
  private val zones: List<Pair<String, File>> = detectZones()

  // Sampler thread only.
  private var prevThreads = HashMap<Int, ThreadSample>()
  private var lastSampleMs = 0L
  private var samples = 0L
  private var frozenS = 0

  fun begin(driver: String) {
    try {
      dir.mkdirs()
      workRoot.listFiles()?.filter { it.isDirectory && it != dir }?.sortedBy { it.name }
        ?.dropLast(KEEP_WORK_DIRS - 1)?.forEach { it.deleteRecursively() }
      File(dir, "device.txt").writeText(deviceFacts(driver))
      synchronized(this) { sysLog = File(dir, "sys.log").bufferedWriter() }
      event("begin game=${File(gamePath).name}")
    } catch (e: Exception) {
      Log.w(TAG, "forensics disabled: $e")
    }
  }

  fun event(what: String) = line("t=${elapsedS()} event=$what")

  /** One sampler tick: [frames] presented at [fps]; [guestFrame] matches xdroid.log f:. */
  fun sample(fps: Double, frames: Long, guestFrame: Long, adpf: String) {
    if (sysLog == null) return
    val now = SystemClock.elapsedRealtime()
    val dt = if (lastSampleMs > 0) (now - lastSampleMs) / 1000.0 else 0.0
    lastSampleMs = now
    val sb = StringBuilder(512)
    sb.append("t=").append(elapsedS())
    sb.append(" fps=").append(fmt1(fps)).append(" fr=").append(frames).append(" gf=").append(guestFrame)

    val threads = sampleThreads(dt)
    sb.append(" cpu=").append(threads.first)
    sb.append(" clk=").append(
      policies.joinToString(",") { p ->
        "${mhz("${p.path}/scaling_cur_freq")}/${mhz("${p.path}/scaling_max_freq")}"
      },
    )
    for ((key, file) in gpuFiles) {
      sb.append(" g.").append(key).append('=').append(readOrNull(file)?.trim()?.replace(Regex("\\s+"), "_") ?: "na")
    }
    if (zones.isNotEmpty()) {
      sb.append(" tz=").append(zones.joinToString(",") { (t, f) -> "$t:${readOrNull(f)?.trim() ?: "na"}" })
    }

    val pm = context.getSystemService(PowerManager::class.java)
    if (pm != null) {
      sb.append(" th=").append(pm.currentThermalStatus)
      if (Build.VERSION.SDK_INT >= 30) {
        sb.append(" hr=").append(pm.getThermalHeadroom(0).let { if (it.isNaN()) "na" else fmt2(it.toDouble()) })
      }
      sb.append(" ps=").append(if (pm.isPowerSaveMode) 1 else 0)
    }
    val bm = context.getSystemService(BatteryManager::class.java)
    val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
    if (battery != null) {
      sb.append(" bt=").append(battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0)
      sb.append(" mv=").append(battery.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1))
      sb.append(" plug=").append(battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1))
      sb.append(" lvl=").append(battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1))
    }
    // Raw: microamps on most devices, milliamps on some; the sign of
    // discharge differs between vendors too. The analyzer normalizes.
    sb.append(" cur=").append(bm?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW) ?: "na")
    sb.append(" hz=").append(display()?.refreshRate?.let { fmt1(it.toDouble()) } ?: "na")
    sb.append(" adpf=").append(adpf)
    sb.append(" rss=").append(rssMb())
    sb.append(" thr=").append(threads.second)
    line(sb.toString())

    // Affinity of the busiest threads, rarely: OEM game modes move them.
    if (++samples % 30 == 0L) line("t=${elapsedS()} aff ${affinity()}")
    if (samples % PERIODIC_S == 0L) refresh(optional = true)
    // No frames after the game presented: a hang or a freeze. Refresh while
    // it lasts, as the user may kill the app at any moment.
    if (fps < 1.0 && guestFrame > 0) {
      frozenS++
      if (frozenS == 3) event("freeze gf=$guestFrame")
      if (frozenS >= 3 && (frozenS - 3) % FREEZE_REFRESH_S == 0) refresh(optional = true)
    } else {
      frozenS = 0
    }
  }

  /** Copies the logs and refreshes the zip, off the caller's thread. */
  fun snapshot(reason: String) {
    event("snapshot $reason")
    refresh(optional = false)
  }

  /**
   * Last refresh of a session the app is about to kill (wedged, stuck
   * teardown): call after its crash report is written. Waits up to
   * [timeoutMs] so the zip is complete before the process ends.
   */
  fun finishBlocking(reason: String, timeoutMs: Long) {
    event("end $reason")
    val done = refresh(optional = false) ?: return
    runCatching { done.get(timeoutMs, TimeUnit.MILLISECONDS) }
  }

  private fun refresh(optional: Boolean): Future<*>? {
    if (sysLog == null || (optional && pending.get())) return null
    pending.set(true)
    return try {
      io.submit(
        Runnable {
          try {
            synchronized(this) { sysLog?.flush() }
            copySources()
            ProcessBuilder("logcat", "-d", "-v", "threadtime")
              .redirectErrorStream(true)
              .redirectOutput(File(dir, "logcat.txt"))
              .start()
              .waitFor(15, TimeUnit.SECONDS)
            zip()
          } catch (e: Exception) {
            Log.w(TAG, "forensics refresh failed: $e")
          } finally {
            pending.set(false)
          }
        },
      )
    } catch (_: RejectedExecutionException) {
      null
    }
  }

  fun close() {
    snapshot("end")
    io.execute {
      synchronized(this) {
        runCatching { sysLog?.close() }
        sysLog = null
      }
    }
    io.shutdown()
  }

  // ---- Sampling ----

  // Returns (process CPU %, top threads as name:cpu/wait/cCORE;...). CPU and
  // wait come from schedstat (ns, exact); the run-queue wait is time spent
  // runnable but not running, which shows CPU contention or bad placement.
  private fun sampleThreads(dt: Double): Pair<String, String> {
    val now = HashMap<Int, ThreadSample>()
    val rows = ArrayList<Triple<String, Double, Pair<Double, Int>>>()
    var total = 0.0
    File("/proc/self/task").listFiles()?.forEach { d ->
      val tid = d.name.toIntOrNull() ?: return@forEach
      val stat = readOrNull(File(d, "stat")) ?: return@forEach
      val name = stat.substringAfter('(').substringBeforeLast(')').replace(' ', '_')
      val p = stat.substringAfterLast(") ").split(' ')
      if (p.size < 37) return@forEach
      val ticks = p[11].toLong() + p[12].toLong()
      val core = p[36].toIntOrNull() ?: -1
      val ss = readOrNull(File(d, "schedstat"))?.trim()?.split(' ')
      val run = ss?.getOrNull(0)?.toLongOrNull() ?: -1L
      val wait = ss?.getOrNull(1)?.toLongOrNull() ?: -1L
      now[tid] = ThreadSample(ticks, run, wait)
      val prev = prevThreads[tid] ?: return@forEach
      if (dt <= 0) return@forEach
      val cpu =
        if (run >= 0 && prev.runNs >= 0) (run - prev.runNs) / 1e7 / dt else (ticks - prev.ticks) / dt
      val waitPct = if (wait >= 0 && prev.waitNs >= 0) (wait - prev.waitNs) / 1e7 / dt else -1.0
      total += cpu
      rows.add(Triple(name, cpu, waitPct to core))
    }
    prevThreads = now
    val top = rows.sortedByDescending { it.second }.take(TOP_THREADS).toMutableList()
    // Starved threads too: high run-queue wait with little CPU.
    rows.filter { it !in top && it.third.first >= 10.0 }
      .sortedByDescending { it.third.first }
      .take(4)
      .let { top.addAll(it) }
    val text =
      top.joinToString(";") { (n, c, w) -> "$n:${fmt0(c)}/${fmt0(w.first)}/c${w.second}" }
    return fmt0(total) to text
  }

  private fun affinity(): String {
    val busiest = prevThreads.keys.toList()
    return busiest.mapNotNull { tid ->
      val status = readOrNull(File("/proc/self/task/$tid/status")) ?: return@mapNotNull null
      val name = status.lineSequence().firstOrNull { it.startsWith("Name:") }?.substringAfter(':')?.trim() ?: "?"
      val cpus = status.lineSequence().firstOrNull { it.startsWith("Cpus_allowed_list:") }?.substringAfter(':')?.trim() ?: "?"
      val nvcs = status.lineSequence().firstOrNull { it.startsWith("nonvoluntary_ctxt_switches:") }?.substringAfter(':')?.trim() ?: "?"
      val stat = readOrNull(File("/proc/self/task/$tid/stat"))?.substringAfterLast(") ")?.split(' ')
      val nice = stat?.getOrNull(16) ?: "?"
      val ticks = (stat?.getOrNull(11)?.toLongOrNull() ?: 0L) + (stat?.getOrNull(12)?.toLongOrNull() ?: 0L)
      Triple(ticks, name.replace(' ', '_'), "cpus=$cpus nice=$nice nvcs=$nvcs")
    }.sortedByDescending { it.first }.take(TOP_THREADS).joinToString(" | ") { "${it.second} ${it.third}" } +
      " | cpuset=${readOrNull(File("/proc/self/cpuset"))?.trim() ?: "na"}"
  }

  // ---- Static facts ----

  private fun deviceFacts(driver: String): String =
    buildString {
      appendLine("XDroid360 forensics $stamp")
      appendLine("game=${File(gamePath).name} driver=$driver")
      appendLine("device=${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE}/${Build.PRODUCT}) brand=${Build.BRAND}")
      appendLine("hardware=${Build.HARDWARE} board=${Build.BOARD}")
      if (Build.VERSION.SDK_INT >= 31) appendLine("soc=${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}")
      appendLine("android=${Build.VERSION.RELEASE} sdk=${Build.VERSION.SDK_INT} patch=${Build.VERSION.SECURITY_PATCH}")
      appendLine("build=${Build.DISPLAY} fingerprint=${Build.FINGERPRINT}")
      appendLine("kernel=${System.getProperty("os.version")}")
      for (prop in PROPS) appendLine("prop $prop=${sysProp(prop)}")

      val am = context.getSystemService(ActivityManager::class.java)
      if (am != null) {
        val mi = ActivityManager.MemoryInfo().also { am.getMemoryInfo(it) }
        appendLine("mem total=${mi.totalMem shr 20}MB avail=${mi.availMem shr 20}MB threshold=${mi.threshold shr 20}MB low=${mi.lowMemory}")
      }

      appendLine("cores=$cores")
      for (p in policies) {
        appendLine(
          "policy cpus=${p.cpus} min=${mhz("${p.path}/cpuinfo_min_freq")} max=${mhz("${p.path}/cpuinfo_max_freq")} " +
            "scaling_min=${mhz("${p.path}/scaling_min_freq")} scaling_max=${mhz("${p.path}/scaling_max_freq")} " +
            "gov=${readOrNull(File("${p.path}/scaling_governor"))?.trim() ?: "na"}",
        )
      }
      val parts =
        readOrNull(File("/proc/cpuinfo"))?.lineSequence()?.filter { it.startsWith("CPU part") }
          ?.map { it.substringAfter(':').trim() }?.toList()
      appendLine("cpu_parts=${parts?.joinToString(",") ?: "na"}")
      val cpuset = readOrNull(File("/proc/self/cpuset"))?.trim()
      appendLine("cpuset=$cpuset cpus=${cpuset?.let { readOrNull(File("/dev/cpuset$it/cpus"))?.trim() } ?: "na"}")

      for ((key, path) in GPU_FILES) appendLine("gpu $key=${readOrNull(File(path))?.trim() ?: "unreadable"}")
      appendLine("thermal_zones_readable=${zones.size} ${zones.joinToString(",") { it.first }}")

      val pm = context.getSystemService(PowerManager::class.java)
      if (pm != null) {
        appendLine("thermal status=${pm.currentThermalStatus} headroom=${if (Build.VERSION.SDK_INT >= 30) pm.getThermalHeadroom(0) else "na"}")
        if (Build.VERSION.SDK_INT >= 35) {
          runCatching { appendLine("thermal thresholds=${pm.thermalHeadroomThresholds}") }
        }
        appendLine(
          "power save=${pm.isPowerSaveMode} sustained_supported=${pm.isSustainedPerformanceModeSupported} " +
            "ignoring_battery_opt=${pm.isIgnoringBatteryOptimizations(context.packageName)}",
        )
      }
      if (Build.VERSION.SDK_INT >= 31) {
        runCatching { appendLine("game_mode=${context.getSystemService(GameManager::class.java)?.gameMode}") }
      }
      display()?.let { d ->
        appendLine("display refresh=${d.refreshRate} mode=${d.mode}")
        appendLine("display modes=${d.supportedModes.joinToString(",") { "${it.physicalWidth}x${it.physicalHeight}@${fmt1(it.refreshRate.toDouble())}" }}")
      }
      val bm = context.getSystemService(BatteryManager::class.java)
      appendLine(
        "battery current_now=${bm?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)} " +
          "current_avg=${bm?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_AVERAGE)} " +
          "capacity=${bm?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)} charging=${bm?.isCharging}",
      )
    }

  private fun detectPolicies(): List<Policy> {
    val seen = LinkedHashMap<String, Int>()
    for (cpu in 0 until cores) {
      val related = readOrNull(File("/sys/devices/system/cpu/cpu$cpu/cpufreq/related_cpus"))?.trim() ?: continue
      seen.putIfAbsent(related, cpu)
    }
    return seen.map { (cpus, first) -> Policy(cpus.replace(' ', '-'), "/sys/devices/system/cpu/cpu$first/cpufreq") }
  }

  // Readable zones whose type names a CPU, GPU or skin sensor (bounded).
  private fun detectZones(): List<Pair<String, File>> {
    val out = ArrayList<Pair<String, File>>()
    for (i in 0 until 160) {
      val zone = File("/sys/class/thermal/thermal_zone$i")
      val type = readOrNull(File(zone, "type"))?.trim() ?: continue
      if (!ZONE_TYPES.containsMatchIn(type)) continue
      val temp = File(zone, "temp")
      if (readOrNull(temp) == null) continue
      out.add(type to temp)
      if (out.size >= 12) break
    }
    return out
  }

  // ---- Output ----

  private fun line(text: String) {
    synchronized(this) {
      val w = sysLog ?: return
      try {
        w.write(text)
        w.write("\n")
        w.flush()
      } catch (_: Exception) {
      }
    }
  }

  private fun copySources() {
    val root = File(context.filesDir, "xdroid")
    copy(File(root, "xdroid.log"), "xdroid.log")
    copy(File(root, "logs/glitch-marks.txt"), "glitch-marks.txt")
    copy(File(root, "xenia-edge.config.toml"), "xenia-edge.config.toml")
    File(root, "config").listFiles()?.forEach { copy(it, "config-${it.name}") }
    // This session's own crash report (wedged, stuck teardown), never an
    // older session's.
    File(root, "logs").listFiles { f -> f.name.startsWith("crash-") && f.lastModified() >= startWallMs }
      ?.forEach { copy(it, it.name) }
  }

  private fun copy(src: File, name: String) {
    if (src.isFile) runCatching { src.copyTo(File(dir, name), overwrite = true) }
  }

  private fun zip() {
    val tmp = File(zipFile.parentFile, zipFile.name + ".tmp")
    ZipOutputStream(tmp.outputStream().buffered()).use { zos ->
      dir.listFiles()?.sortedBy { it.name }?.forEach { f ->
        zos.putNextEntry(ZipEntry("$stamp/${f.name}"))
        f.inputStream().use { it.copyTo(zos) }
        zos.closeEntry()
      }
    }
    zipFile.delete()
    tmp.renameTo(zipFile)
  }

  // ---- Helpers ----

  private fun elapsedS() = (SystemClock.elapsedRealtime() - startMs) / 1000

  @Suppress("DEPRECATION")
  private fun display(): Display? =
    if (Build.VERSION.SDK_INT >= 30) {
      runCatching { context.display }.getOrNull()
    } else {
      context.getSystemService(android.view.WindowManager::class.java)?.defaultDisplay
    }

  private fun rssMb(): Long =
    readOrNull(File("/proc/self/status"))?.lineSequence()?.firstOrNull { it.startsWith("VmRSS:") }
      ?.split(Regex("\\s+"))?.getOrNull(1)?.toLongOrNull()?.div(1024) ?: -1L

  private fun mhz(path: String): String = readOrNull(File(path))?.trim()?.toLongOrNull()?.let { (it / 1000).toString() } ?: "na"

  private fun readOrNull(f: File): String? =
    try {
      f.readText().takeIf { it.isNotBlank() }
    } catch (_: Exception) {
      null
    }

  private fun sysProp(key: String): String =
    try {
      Class.forName("android.os.SystemProperties").getMethod("get", String::class.java)
        .invoke(null, key) as String
    } catch (_: Exception) {
      "?"
    }

  private fun fmt0(v: Double) = String.format(Locale.US, "%.0f", v)

  private fun fmt1(v: Double) = String.format(Locale.US, "%.1f", v)

  private fun fmt2(v: Double) = String.format(Locale.US, "%.2f", v)

  private companion object {
    const val TAG = "XDroid360"
    const val TOP_THREADS = 8
    const val KEEP_WORK_DIRS = 5
    const val PERIODIC_S = 15L
    const val FREEZE_REFRESH_S = 5
    val ZONE_TYPES = Regex("cpu|gpu|skin|soc|ddr|battery|sys-therm", RegexOption.IGNORE_CASE)
    val PROPS =
      listOf(
        "ro.build.version.oneui", "ro.board.platform", "ro.soc.model", "ro.hardware.vulkan",
        "ro.gfx.driver.0", "ro.gfx.driver.1", "ro.vendor.qti.va_aosp.support",
        "persist.sys.game.mode", "ro.config.dha_cached_max",
      )
    // Short key to sysfs path. Qualcomm kgsl first, then Samsung's GPU node.
    val GPU_FILES =
      listOf(
        "busy" to "/sys/class/kgsl/kgsl-3d0/gpubusy",
        "busy_pct" to "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
        "clk" to "/sys/class/kgsl/kgsl-3d0/gpuclk",
        "devfreq" to "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
        "max_clk" to "/sys/class/kgsl/kgsl-3d0/max_gpuclk",
        "thermal_pwrlevel" to "/sys/class/kgsl/kgsl-3d0/thermal_pwrlevel",
        "temp" to "/sys/class/kgsl/kgsl-3d0/temp",
        "sec_busy" to "/sys/kernel/gpu/gpu_busy",
        "sec_clk" to "/sys/kernel/gpu/gpu_clock",
        "sec_max_clk" to "/sys/kernel/gpu/gpu_max_clock",
        "sec_min_clk" to "/sys/kernel/gpu/gpu_min_clock",
      )
  }
}
