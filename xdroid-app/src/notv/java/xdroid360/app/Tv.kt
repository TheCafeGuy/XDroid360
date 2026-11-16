package xdroid360.app

import android.app.Activity
import androidx.activity.ComponentActivity
import androidx.compose.runtime.Composable

/** The public build has no TV features: the no-op twin of src/tv's [Tv]. */
@Suppress("UNUSED_PARAMETER")
class Tv(activity: Activity, onStart: () -> Unit) {
  val active get() = false

  fun ready() {}

  fun live(live: Map<String, String>) {}

  fun info(text: String) {}

  fun yieldLive() {}

  fun close() {}

  @Composable fun MenuRows() {}

  companion object {
    fun libraryButton(activity: ComponentActivity): @Composable () -> Unit = {}
  }
}
