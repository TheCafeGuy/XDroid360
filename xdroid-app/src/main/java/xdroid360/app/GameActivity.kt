package xdroid360.app

import android.app.AlertDialog
import android.content.ComponentCallbacks2
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.compose.ui.platform.ComposeView
import java.io.File
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import xdroid360.core.Bridge

/**
 * Fullscreen landscape game session. Boots the game path on creation,
 * forwards controller input with the fixed standard mapping and hosts the
 * quick menu (Back or Select+Start); quitting is a menu action.
 *
 * Threads: UI for boot, surface and teardown (the native context needs its
 * looper); one executor for pause, resume and trim, so they apply in order;
 * one sampler thread for the stats, the performance hint and the wedge
 * killer.
 */
class GameActivity : ComponentActivity() {
  private lateinit var surfaceView: SurfaceView
  private lateinit var statusView: TextView
  private var currentSurface: Surface? = null
  private var gamePath = ""
  private var booted = false
  private var sessionEnded = false
  @Volatile private var stopped = true
  // The exit prompt pauses the game: the wedge killer must not count it.
  @Volatile private var confirmingExit = false

  private val menuState = InGameState()
  private val menuActions =
    InGameActions(
      setFastForward = { setFastForward(it) },
      setTone = { Bridge.setToneStrength(it) },
      markGlitch = { markGlitch() },
      quit = { quitGame() },
      persist = { persistMenuPrefs() },
    )

  private val lifecycleExecutor = Executors.newSingleThreadExecutor()
  private val samplerThread = HandlerThread("xdroid-sampler").apply { start() }
  private val sampler = Handler(samplerThread.looper)

  // Sampler thread only.
  private val sysStats by lazy { SystemStats(this) }
  private val perfHint by lazy { PerfHint(this) }
  private val frameRateVoter = FrameRateVoter()
  private val liveFile by lazy { LiveFile(File(filesDir, "xdroid/live.toml")) }
  // Forensic recorder for remote testers (temp, revert-planned).
  @Volatile private var forensics: Forensics? = null
  private val fpsHistory = ArrayDeque<Double>()
  private var lastTickMs = 0L
  private var lastFrames = 0L
  private var ticks = 0L
  private var everPresented = false
  private var stallSeconds = 0
  private var trickleSeconds = 0
  private var titleCaptured = false

  // D-pad as hat axes (DS4): last state per direction, for edge events.
  private val hatDown = BooleanArray(4)
  // Last value sent per axis; NaN forces the first send. Every motion event
  // carries all six axes, mostly unchanged: skip those JNI calls.
  private val axisSent = FloatArray(6) { Float.NaN }
  private var selectHeld = false

  /** Called by the native app context to repaint the surface. */
  @Suppress("unused")
  fun postInvalidateWindowSurface() {
    if (::surfaceView.isInitialized) surfaceView.postInvalidate()
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    hideSystemUi()
    val root = FrameLayout(this)
    surfaceView =
      SurfaceView(this).also { view ->
        view.holder.addCallback(
          object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) = pushSurface(holder.surface)

            override fun surfaceChanged(
              holder: SurfaceHolder,
              format: Int,
              width: Int,
              height: Int,
            ) = pushSurface(holder.surface)

            override fun surfaceDestroyed(holder: SurfaceHolder) = pushSurface(null)
          },
        )
        view.addOnLayoutChangeListener { _, left, top, right, bottom, _, _, _, _ ->
          Bridge.surfaceLayout(left, top, right, bottom)
        }
      }
    root.addView(surfaceView, matchParent())
    statusView =
      TextView(this).also {
        it.setTextColor(0xFFFFFFFF.toInt())
        it.textSize = 12f
        it.setBackgroundColor(0x80000000.toInt())
        it.setPadding(16, 16, 16, 16)
        // Empty, its padding and background showed as a small square.
        it.visibility = View.GONE
      }
    root.addView(
      statusView,
      FrameLayout.LayoutParams(
        FrameLayout.LayoutParams.WRAP_CONTENT,
        FrameLayout.LayoutParams.WRAP_CONTENT,
      ),
    )
    val prefs = prefs()
    menuState.showStats = prefs.getBoolean("ingame_show_stats", true)
    menuState.detailedStats = prefs.getBoolean("ingame_detailed_stats", false)
    menuState.toneStrength = prefs.getFloat("tone_strength", 0f)
    Bridge.setToneStrength(menuState.toneStrength)
    root.addView(
      ComposeView(this).also { it.setContent { InGameOverlay(menuState, menuActions) } },
      matchParent(),
    )
    setContentView(root)

    gamePath = intent.getStringExtra(EXTRA_GAME_PATH).orEmpty()
    if (gamePath.isEmpty()) {
      setStatus("no game path")
      return
    }
    boot()
  }

  // singleTask: a launch while a session is alive arrives here. The emulator
  // runs one title per process, so the running game stays.
  override fun onNewIntent(intent: Intent) {
    super.onNewIntent(intent)
    val path = intent.getStringExtra(EXTRA_GAME_PATH).orEmpty()
    if (path.isNotEmpty() && path != gamePath) {
      Toast.makeText(this, "Quit the running game first", Toast.LENGTH_SHORT).show()
    }
  }

  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    if (hasFocus) hideSystemUi()
  }

  override fun onStart() {
    super.onStart()
    stopped = false
    if (!booted) return
    SessionReports.setState(this, "foreground")
    forensics?.event("start")
    // An open exit prompt keeps the game paused until it is answered.
    if (!confirmingExit) lifecycleExecutor.execute { Bridge.resume() }
    startSampling()
  }

  override fun onStop() {
    // Nothing runs headless (battery, heat); resume is exact on return.
    stopped = true
    sampler.removeCallbacks(tick)
    if (booted) {
      SessionReports.setState(this, "background")
      forensics?.event("stop")
      forensics?.snapshot("stop")
      lifecycleExecutor.execute { Bridge.pause() }
    }
    super.onStop()
  }

  // The RUNNING_* levels are only delivered before Android 14.
  @Suppress("DEPRECATION")
  override fun onTrimMemory(level: Int) {
    super.onTrimMemory(level)
    // Real memory pressure only. UI_HIDDEN comes with every lock or task
    // switch, and dropping the caches there would stutter every resume.
    val pressure =
      level == ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW ||
        level == ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL ||
        level >= ComponentCallbacks2.TRIM_MEMORY_BACKGROUND
    if (booted && pressure) lifecycleExecutor.execute { Bridge.trim() }
  }

  override fun onDestroy() {
    sampler.removeCallbacks(tick)
    if (booted) {
      // The native teardown ends the process (see endSession).
      endSession("activity destroyed", crash = false)
      destroyNative("activity destroyed")
      booted = false
    }
    lifecycleExecutor.shutdown()
    sampler.post { perfHint.close() }
    samplerThread.quitSafely()
    super.onDestroy()
  }

  // The system back gesture closes the quick menu, or asks before leaving the
  // game for the library; it never ends the app. A controller's Back key
  // still toggles the menu (onKeyDown), so a button press cannot quit.
  @Suppress("MissingSuperCall")
  @Deprecated("Back closes the menu or asks to leave the game")
  override fun onBackPressed() {
    if (menuState.menuOpen) toggleMenu() else confirmExit()
  }

  private fun confirmExit() {
    if (!booted) {
      quitGame()
      return
    }
    if (confirmingExit) return
    confirmingExit = true
    lifecycleExecutor.execute { Bridge.pause() }
    var leaving = false
    AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
      .setTitle("Exit game?")
      .setMessage("Progress since the last save will be lost.")
      .setPositiveButton("Exit") { _, _ -> leaving = true }
      .setNegativeButton("Keep playing", null)
      .setOnDismissListener {
        confirmingExit = false
        if (leaving) {
          quitGame()
        } else {
          if (!stopped) lifecycleExecutor.execute { Bridge.resume() }
          hideSystemUi()
        }
      }
      .show()
  }

  private fun boot() {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
      !Environment.isExternalStorageManager()
    ) {
      setStatus("grant All-files access, then reopen the game")
      finish()
      return
    }
    val storage = File(filesDir, "xdroid").apply { mkdirs() }
    val cache = File(cacheDir, "xdroid").apply { mkdirs() }
    val driverPath = prefs().getString("driver_path", "").orEmpty()
    SessionReports.recoverDeadSession(this)?.let { setStatus(it) }
    SessionReports.begin(this, gamePath)
    val rc =
      Bridge.boot(this, assets, storage.absolutePath, cache.absolutePath, gamePath, driverPath)
    if (rc != 0) {
      bootFailed("Boot failed (code $rc)")
      return
    }
    booted = true
    forensics =
      Forensics(this, gamePath).also {
        it.begin(prefs().getString("driver_name", "").orEmpty().ifEmpty { "system" })
        Toast.makeText(this, "Forensic build: data goes to Download/${it.zipFile.name}", Toast.LENGTH_LONG).show()
      }
    Bridge.setFastForward(false)
    // The surface may predate the boot, with no more callbacks coming.
    Bridge.surfaceChanged(currentSurface)
  }

  private fun pushSurface(surface: Surface?) {
    currentSurface = surface
    Bridge.surfaceChanged(surface)
    frameRateVoter.reset()
  }

  // DISP-2. On the UI thread, which also replaces the surface.
  private fun applyFrameRate(rate: Float) {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
    val surface = currentSurface ?: return
    if (!surface.isValid) return
    try {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        surface.setFrameRate(
          rate,
          Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
          Surface.CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS,
        )
      } else {
        surface.setFrameRate(rate, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
      }
      Log.i(TAG, "XDroid360: frame rate vote $rate")
    } catch (e: IllegalArgumentException) {
      Log.w(TAG, "XDroid360: frame rate vote $rate failed: ${e.message}")
    }
  }

  private fun quitGame() {
    persistMenuPrefs()
    if (booted) {
      // The native teardown ends the process (see endSession).
      endSession("quit", crash = false)
      destroyNative("quit")
      booted = false
    }
    // Back to the library even when it is not under this activity.
    startActivity(
      Intent(this, MainActivity::class.java)
        .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP),
    )
    finish()
  }

  // A game that does not load is not a crash: report it and leave.
  private fun bootFailed(message: String) {
    if (sessionEnded) return
    Log.w(TAG, "boot failed: $message")
    endSession("boot failed: $message", crash = false)
    if (booted) {
      destroyNative("boot failed")
      booted = false
    }
    Toast.makeText(applicationContext, message, Toast.LENGTH_LONG).show()
    finish()
  }

  // Runs before the native teardown, which never returns once a title is
  // open: Bridge.destroy -> Emulator::TerminateTitle -> KernelState::
  // TerminateTitle quick-exits the process (upstream 005357043). Ending the
  // session after it left the marker behind, and a clean quit was filed as a
  // crash on the next start.
  private fun endSession(reason: String, crash: Boolean, details: String = "") {
    if (sessionEnded) return
    sessionEnded = true
    SessionReports.end(this, reason, crash, details)
    // After the report, so the zip carries it; complete before the exit.
    forensics?.finishBlocking(reason, FORENSICS_FINAL_MS)
    forensics?.close()
    forensics = null
  }

  // Native teardown joins the emulator threads, and a wedged guest can block
  // it forever: past the deadline, file a crash report and end the process.
  private fun destroyNative(reason: String) {
    val done = AtomicBoolean(false)
    Thread({
      try {
        Thread.sleep(TEARDOWN_LIMIT_MS)
      } catch (_: InterruptedException) {
        return@Thread
      }
      if (!done.get()) {
        Log.w(TAG, "native teardown stuck ($reason), ending the process")
        // The session already ended with a clean report; file this apart.
        SessionReports.fileCrash(this, "teardown stuck ($reason)")
        forensics?.finishBlocking("teardown stuck ($reason)", FORENSICS_FINAL_MS)
        Process.killProcess(Process.myPid())
      }
    }, "xdroid-teardown-watchdog").apply { isDaemon = true }.start()
    Bridge.destroy()
    done.set(true)
  }

  // ---- Sampler: FPS, stats overlay, performance hint, wedge killer. ----

  private val tick =
    object : Runnable {
      override fun run() {
        if (stopped) return
        try {
          sampleOnce()
        } catch (e: Exception) {
          Log.w(TAG, "sampler tick failed: ${e.message}")
        }
        if (!stopped) sampler.postDelayed(this, 1000)
      }
    }

  private fun startSampling() {
    sampler.post {
      // Also drops a tick that re-posted itself across a stop/start.
      sampler.removeCallbacks(tick)
      lastTickMs = SystemClock.uptimeMillis()
      lastFrames = Bridge.frameNumber()
      fpsHistory.clear()
      resetKiller()
      sampler.postDelayed(tick, 1000)
    }
  }

  private fun sampleOnce() {
    val now = SystemClock.uptimeMillis()
    val elapsedMs = now - lastTickMs
    lastTickMs = now
    val frameNumber = Bridge.frameNumber()
    val frames = frameNumber - lastFrames
    lastFrames = frameNumber
    if (elapsedMs <= 0) return
    val fps = frames * 1000.0 / elapsedMs
    forensics?.sample(fps, frames, frameNumber, perfHint.state())
    ticks++
    if (frameNumber > 0) everPresented = true

    fpsHistory.addLast(fps)
    while (fpsHistory.size > 10) fpsHistory.removeFirst()
    val summary =
      String.format(
        Locale.US,
        "%.1f FPS (10s %.1f, %.0f-%.0f)",
        fps,
        fpsHistory.average(),
        fpsHistory.min(),
        fpsHistory.max(),
      )
    Log.i("XDroid360HUD", summary)
    captureTitleId()

    // The full stats walk every thread: only while the overlay shows them,
    // plus the XDROID-SYS log line every 10 s.
    val overlayVisible = menuState.showStats || menuState.menuOpen
    val logSys = ticks % 10 == 0L
    if (overlayVisible || logSys) {
      sysStats.sample()
      if (logSys) Log.i(TAG, sysStats.logLine(summary))
    }
    if (overlayVisible) {
      val cpu = sysStats.cpuText()
      val gpu = sysStats.gpuText()
      val thermal = sysStats.thermalText()
      val detail = sysStats.detailText()
      runOnUiThread {
        menuState.fps = summary
        menuState.cpu = cpu
        menuState.gpu = gpu
        menuState.thermal = thermal
        menuState.detail = detail
      }
    }

    perfHint.update(frames, fps)
    liveFile.poll()?.let { live ->
      live["xdroid_frame_rate_vote"]?.let { frameRateVoter.enabled = it == "true" }
      // DISP-4 A/B: the stats overlay redraws a full-size layer every second.
      live["xdroid_show_stats"]?.let { show -> runOnUiThread { menuState.showStats = show == "true" } }
    }
    frameRateVoter.update(fps)?.let { rate -> runOnUiThread { applyFrameRate(rate) } }
    if (!everPresented) {
      Bridge.bootError()?.let { error -> runOnUiThread { bootFailed(error) } }
    }
    checkWedged(fps, summary)
  }

  private fun resetKiller() {
    stallSeconds = 0
    trickleSeconds = 0
  }

  // A presented game that stops presenting is wedged; a black screen must
  // not run unattended. Loads before the first frame are never caught, and
  // the count holds while the quick menu is open.
  private fun checkWedged(fps: Double, summary: String) {
    if (!everPresented || menuState.menuOpen || confirmingExit) {
      resetKiller()
      return
    }
    stallSeconds = if (fps < 1.0) stallSeconds + 1 else 0
    trickleSeconds = if (fps < 2.0) trickleSeconds + 1 else 0
    val reason =
      when {
        stallSeconds >= STALL_LIMIT_S -> "no frames for $stallSeconds s"
        trickleSeconds >= TRICKLE_LIMIT_S -> "under 2 FPS for $trickleSeconds s"
        else -> return
      }
    Log.w(TAG, "session wedged: $reason")
    sysStats.sample()
    // No native teardown: wedged threads may never join. The process exit
    // ends everything.
    SessionReports.end(
      this,
      "wedged: $reason",
      crash = true,
      details = "last=$summary\n${sysStats.logLine(summary)}",
    )
    // After the crash report, so the forensic zip carries it.
    forensics?.finishBlocking("wedged: $reason", FORENSICS_FINAL_MS)
    Process.killProcess(Process.myPid())
  }

  // Remembers the game's title ID (per-game settings, DLC names) once known.
  private fun captureTitleId() {
    if (titleCaptured) return
    val id = Bridge.titleId()
    if (id == 0L) return
    titleCaptured = true
    val hex = String.format(Locale.US, "%08X", id)
    prefs().edit().putString("title_$gamePath", hex).apply()
    Log.i(TAG, "XDROID-TITLE $hex $gamePath")
  }

  // ---- Quick menu. ----

  private fun toggleMenu() {
    menuState.menuOpen = !menuState.menuOpen
    if (!menuState.menuOpen) persistMenuPrefs()
  }

  private fun setFastForward(on: Boolean) {
    menuState.fastForward = on
    Bridge.setFastForward(on)
  }

  // A tester's glitch report with the guest frame, which lines up with the
  // f:<frame> prefix of xdroid.log (the report comes a few seconds after the
  // glitch, so look back from that frame). Also appended to a file next to
  // xdroid.log, as logcat rotates during long sessions.
  private fun markGlitch() {
    val frame =
      try {
        Bridge.frameNumber()
      } catch (_: UnsatisfiedLinkError) {
        -1L
      }
    val line = "XDROID-USER-GLITCH frame=$frame time=${java.time.LocalTime.now().withNano(0)}"
    Log.w("XDroid360", line)
    forensics?.event("glitch frame=$frame")
    lifecycleExecutor.execute {
      try {
        java.io.File(filesDir, "xdroid/logs/glitch-marks.txt").appendText(line + "\n")
      } catch (e: java.io.IOException) {
        Log.w("XDroid360", "glitch mark not saved: $e")
      }
    }
    // Our own overlay, not a Toast: SystemUI draws Toasts, and it died 3 of 3
    // times at a GPU preemption fault while our hangs held the GPU.
    val note = "Glitch marked (frame $frame)"
    setStatus(note)
    statusView.postDelayed({ if (statusView.text.toString() == note) setStatus("") }, GLITCH_NOTE_MS)
  }

  private fun persistMenuPrefs() {
    prefs().edit()
      .putBoolean("ingame_show_stats", menuState.showStats)
      .putBoolean("ingame_detailed_stats", menuState.detailedStats)
      .putFloat("tone_strength", menuState.toneStrength)
      .apply()
  }

  // ---- Controller input: fixed standard map, applied automatically. ----

  override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
    if (event.device?.isGamepad != true) return super.onKeyDown(keyCode, event)
    val first = event.repeatCount == 0
    when (keyCode) {
      // PS / Home: fast forward toggle.
      KeyEvent.KEYCODE_BUTTON_MODE -> if (first) setFastForward(!menuState.fastForward)
      // Some controllers send Back: the menu, never the exit prompt.
      KeyEvent.KEYCODE_BACK -> if (first) toggleMenu()
      // Triggers send keys and analog axes: drive the axis only.
      KeyEvent.KEYCODE_BUTTON_L2 -> sendAxis(AXIS_LT, 1f)
      KeyEvent.KEYCODE_BUTTON_R2 -> sendAxis(AXIS_RT, 1f)
      else -> {
        if (keyCode == KeyEvent.KEYCODE_BUTTON_SELECT) selectHeld = true
        // Select + Start opens the menu; Start is not forwarded then.
        if (keyCode == KeyEvent.KEYCODE_BUTTON_START && selectHeld) {
          if (first) toggleMenu()
          return true
        }
        val mask = BUTTON_MAP[keyCode] ?: return super.onKeyDown(keyCode, event)
        Bridge.gamepadButton(mask, true)
      }
    }
    return true
  }

  override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
    if (event.device?.isGamepad != true) return super.onKeyUp(keyCode, event)
    when (keyCode) {
      KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_BACK -> {}
      KeyEvent.KEYCODE_BUTTON_L2 -> sendAxis(AXIS_LT, 0f)
      KeyEvent.KEYCODE_BUTTON_R2 -> sendAxis(AXIS_RT, 0f)
      else -> {
        if (keyCode == KeyEvent.KEYCODE_BUTTON_SELECT) selectHeld = false
        val mask = BUTTON_MAP[keyCode] ?: return super.onKeyUp(keyCode, event)
        Bridge.gamepadButton(mask, false)
      }
    }
    return true
  }

  override fun onGenericMotionEvent(event: MotionEvent): Boolean {
    // event.device is null for virtual events and can race a disconnect.
    if (event.device?.isGamepad != true ||
      (event.source and InputDevice.SOURCE_JOYSTICK) == 0
    ) {
      return super.onGenericMotionEvent(event)
    }
    for ((androidAxis, axis) in STICK_AXES) {
      sendAxis(axis, event.getAxisValue(androidAxis))
    }
    val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
    val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
    setHat(0, hatY < -0.5f)
    setHat(1, hatY > 0.5f)
    setHat(2, hatX < -0.5f)
    setHat(3, hatX > 0.5f)
    return true
  }

  // Index 0-3 = up, down, left, right = XInput D-pad masks 1, 2, 4, 8.
  private fun setHat(index: Int, down: Boolean) {
    if (hatDown[index] == down) return
    hatDown[index] = down
    Bridge.gamepadButton(1 shl index, down)
  }

  private fun sendAxis(axis: Int, value: Float) {
    if (axisSent[axis] == value) return
    axisSent[axis] = value
    Bridge.gamepadAxis(axis, value)
  }

  // ---- Helpers. ----

  private fun prefs() = getSharedPreferences("xdroid", MODE_PRIVATE)

  private fun setStatus(text: String) {
    runOnUiThread {
      statusView.text = text
      statusView.visibility = if (text.isEmpty()) View.GONE else View.VISIBLE
    }
  }

  private fun matchParent() =
    FrameLayout.LayoutParams(
      FrameLayout.LayoutParams.MATCH_PARENT,
      FrameLayout.LayoutParams.MATCH_PARENT,
    )

  @Suppress("DEPRECATION")
  private fun hideSystemUi() {
    window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
    window.decorView.systemUiVisibility =
      View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
      View.SYSTEM_UI_FLAG_FULLSCREEN or
      View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
      View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
      View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
      View.SYSTEM_UI_FLAG_LAYOUT_STABLE
  }

  private val InputDevice.isGamepad: Boolean
    get() =
      (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
        (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

  companion object {
    const val EXTRA_GAME_PATH = "game_path"
    private const val TAG = "XDroid360"

    // Wedge killer thresholds: far above any load or menu hitch.
    private const val STALL_LIMIT_S = 30
    private const val TRICKLE_LIMIT_S = 60
    private const val TEARDOWN_LIMIT_MS = 10_000L
    private const val FORENSICS_FINAL_MS = 8_000L
    private const val GLITCH_NOTE_MS = 2_000L

    // Axis ids shared with xdroid_gamepad.h.
    private const val AXIS_LX = 0
    private const val AXIS_LY = 1
    private const val AXIS_RX = 2
    private const val AXIS_RY = 3
    private const val AXIS_LT = 4
    private const val AXIS_RT = 5

    private val STICK_AXES =
      listOf(
        MotionEvent.AXIS_X to AXIS_LX,
        MotionEvent.AXIS_Y to AXIS_LY,
        MotionEvent.AXIS_Z to AXIS_RX,
        MotionEvent.AXIS_RZ to AXIS_RY,
        MotionEvent.AXIS_LTRIGGER to AXIS_LT,
        MotionEvent.AXIS_RTRIGGER to AXIS_RT,
      )

    // XInput button masks.
    private val BUTTON_MAP =
      mapOf(
        KeyEvent.KEYCODE_DPAD_UP to 0x0001,
        KeyEvent.KEYCODE_DPAD_DOWN to 0x0002,
        KeyEvent.KEYCODE_DPAD_LEFT to 0x0004,
        KeyEvent.KEYCODE_DPAD_RIGHT to 0x0008,
        KeyEvent.KEYCODE_BUTTON_START to 0x0010,
        KeyEvent.KEYCODE_BUTTON_SELECT to 0x0020,
        KeyEvent.KEYCODE_BUTTON_THUMBL to 0x0040,
        KeyEvent.KEYCODE_BUTTON_THUMBR to 0x0080,
        KeyEvent.KEYCODE_BUTTON_L1 to 0x0100,
        KeyEvent.KEYCODE_BUTTON_R1 to 0x0200,
        KeyEvent.KEYCODE_BUTTON_A to 0x1000,
        KeyEvent.KEYCODE_BUTTON_B to 0x2000,
        KeyEvent.KEYCODE_BUTTON_X to 0x4000,
        KeyEvent.KEYCODE_BUTTON_Y to 0x8000,
      )
  }
}
