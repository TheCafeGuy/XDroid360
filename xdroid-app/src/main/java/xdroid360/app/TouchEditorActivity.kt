package xdroid360.app

import android.os.Bundle
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.ComposeView

/**
 * The touch layout editor from the library (no game running): the shared
 * layout, in landscape like the game. Test mode shows the values the pad
 * would send.
 */
class TouchEditorActivity : ComponentActivity() {
  private val settings = ControlSettings()
  private val editorState = EditorState()
  private lateinit var pad: TouchPadView
  // A game's own layout (library long-press), else the shared one.
  private var title: String? = null

  private val sink =
    object : PadSink {
      override fun button(mask: Int, down: Boolean) {}

      override fun axis(axis: Int, value: Float) {}

      override fun guide() {}

      override fun setFastForward(on: Boolean) {
        pad.fastForwardOn = on
      }
    }

  private val actions =
    object : EditorActions {
      override fun beginEdit() {
        pad.beginEdit()
        editorState.canUndo = true
      }

      override fun setScale(scale: Float) = pad.setSelectedScale(scale)

      override fun setHidden(hidden: Boolean) = pad.setSelectedHidden(hidden)

      override fun setAlpha(alpha: Float) = pad.setSelectedAlpha(alpha)

      override fun undo() = pad.undo()

      override fun reset() = pad.resetLayout()

      override fun test(on: Boolean) {
        pad.editing = !on
        pad.testMode = on
      }

      override fun done() {
        settings.save(prefs())
        val t = title
        if (t != null) {
          prefs().edit().putString(settings.gameLayoutKey(t), pad.layoutJson()).apply()
        } else {
          settings.saveLayout(prefs(), null, pad.layoutJson())
        }
        finish()
      }
    }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    hideSystemUi()
    settings.load(prefs())
    val root = FrameLayout(this)
    root.setBackgroundColor(0xFF20242C.toInt())
    pad =
      TouchPadView(this, settings, sink) { c ->
        editorState.selectedId = c?.id.orEmpty()
        editorState.scale = c?.scale ?: 1f
        editorState.hidden = c != null && pad.hidden(c)
        editorState.alpha = c?.alpha ?: 1f
      }
    pad.onEdited = { editorState.canUndo = pad.canUndo }
    title = intent.getStringExtra(EXTRA_TITLE_ID)
    val t = title
    pad.setLayout(
      if (t != null) prefs().getString(settings.gameLayoutKey(t), null) ?: settings.loadLayout(prefs(), null)
      else settings.loadLayout(prefs(), null),
    )
    pad.settingsChanged()
    pad.editing = true
    root.addView(pad, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
    root.addView(
      ComposeView(this).also {
        it.setContent {
          MaterialTheme(colorScheme = XdColors) {
            Box(Modifier.fillMaxSize()) {
              TouchEditBar(editorState, actions, Modifier.align(Alignment.TopCenter))
            }
          }
        }
      },
      FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT),
    )
    setContentView(root)
  }

  @Suppress("MissingSuperCall")
  @Deprecated("Back saves and closes the editor")
  override fun onBackPressed() = actions.done()

  private fun prefs() = getSharedPreferences("xdroid", MODE_PRIVATE)

  companion object {
    const val EXTRA_TITLE_ID = "title_id"
  }

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
}
