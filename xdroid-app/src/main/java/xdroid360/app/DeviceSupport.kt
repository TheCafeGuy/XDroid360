package xdroid360.app

import android.os.Build
import java.io.File

/**
 * What this device can run. The native library is built for ARMv8.2, so a
 * CPU without its instructions (Cortex-A53/A73 class: Snapdragon 835, 660,
 * 665, 680) must never load it: it would die with SIGILL. The GPU code
 * targets Qualcomm Adreno; other SoCs are not supported.
 */
object DeviceSupport {
  /** Every core has LSE atomics and RDM, as Cortex-A55/A75 and newer do. */
  val cpuSupported: Boolean by lazy {
    try {
      val features = File("/proc/cpuinfo").readLines().filter { it.startsWith("Features") }
      features.isEmpty() ||
        features.all { line ->
          val tokens = line.substringAfter(':').trim().split(Regex("\\s+"))
          "atomics" in tokens && "asimdrdm" in tokens
        }
    } catch (e: Exception) {
      // Unreadable: do not block a device that may well be supported.
      true
    }
  }

  val qualcommSoc: Boolean by lazy {
    val soc = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) Build.SOC_MANUFACTURER else ""
    soc.contains("QTI", ignoreCase = true) ||
      soc.contains("Qualcomm", ignoreCase = true) ||
      Build.HARDWARE.equals("qcom", ignoreCase = true)
  }

  const val SUPPORTED_TEXT =
    "Snapdragon with an Adreno 6xx GPU or newer (Snapdragon 730G, 845 and later). " +
      "Best results: Snapdragon 8 Gen 1 or newer."
}
