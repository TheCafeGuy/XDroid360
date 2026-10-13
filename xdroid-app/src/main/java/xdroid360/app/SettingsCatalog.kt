package xdroid360.app

/** One editable emulator setting, stored as `key = value` in [section]. */
sealed class SettingDef(
  val key: String,
  val section: String,
  val title: String,
  val description: String,
) {
  class Toggle(
    key: String, section: String, title: String, description: String,
    val default: Boolean,
  ) : SettingDef(key, section, title, description)

  /** Choices are (raw TOML value, label); raw strings carry their quotes. */
  class Choice(
    key: String, section: String, title: String, description: String,
    val options: List<Pair<String, String>>,
    val default: String,
  ) : SettingDef(key, section, title, description)

  class Slider(
    key: String, section: String, title: String, description: String,
    val min: Float, val max: Float, val steps: Int, val default: Float,
    val isInteger: Boolean, val format: (Float) -> String,
  ) : SettingDef(key, section, title, description)
}

data class SettingCategory(val title: String, val icon: String, val settings: List<SettingDef>)

object SettingsCatalog {
  private fun q(s: String) = "\"$s\""

  val categories: List<SettingCategory> = listOf(
    SettingCategory(
      "System", "🖥",
      listOf(
        SettingDef.Choice(
          "user_language", "Console", "Console language",
          "Language the games see.",
          listOf(
            "1" to "English", "2" to "Japanese", "3" to "German", "4" to "French",
            "5" to "Spanish", "6" to "Italian", "7" to "Korean", "9" to "Portuguese",
            "11" to "Polish", "12" to "Russian",
          ),
          "1",
        ),
        SettingDef.Choice(
          "license_mask", "Content", "Content licenses",
          "Unlock the full version of Arcade titles and licensed DLC.",
          listOf("0" to "None", "1" to "Full game", "-1" to "All licenses"),
          "0",
        ),
      ),
    ),
    SettingCategory(
      "Graphics", "🎨",
      listOf(
        SettingDef.Choice(
          "draw_resolution_scale_x", "GPU", "Resolution scale",
          "Renders at a multiple of the native resolution. Much heavier on the GPU and heat.",
          listOf("1" to "1x (native)", "2" to "2x", "3" to "3x"), "1",
        ),
        SettingDef.Choice(
          "anisotropic_override", "GPU", "Anisotropic filtering",
          "Sharper textures at oblique angles; costs memory bandwidth.",
          listOf("-1" to "Game default", "0" to "Off", "2" to "2x", "3" to "4x", "4" to "8x"),
          "-1",
        ),
        SettingDef.Choice(
          "framerate_limit", "GPU", "Frame rate limit",
          "Host frame cap. A lower cap saves power and heat.",
          listOf("0" to "Unlimited", "30" to "30 FPS", "60" to "60 FPS"), "0",
        ),
        SettingDef.Toggle(
          "guest_display_refresh_cap", "GPU", "Cap guest refresh rate",
          "Keeps the console's vblank at 60 Hz (games run at their real speed).", true,
        ),
        SettingDef.Choice(
          "occlusion_query", "GPU", "Occlusion queries",
          "Lens flares, culling, auto-exposure. Fake is safest, strict most accurate.",
          listOf(
            q("fake") to "Fake", q("fast") to "Fast", q("fast-alt") to "Fast (alt)",
            q("strict") to "Strict",
          ),
          q("fast"),
        ),
        SettingDef.Toggle(
          "async_shader_compilation", "GPU", "Async shader compilation",
          "Compiles shaders in the background instead of stuttering.", true,
        ),
        SettingDef.Toggle(
          "async_shader_skip_draws", "GPU", "Skip draws while compiling",
          "Hides objects whose shaders are still compiling (less stutter, brief pop-in).", true,
        ),
        SettingDef.Toggle(
          "async_shader_vs_interpreter", "GPU", "Vertex shader interpreter",
          "Draws geometry with an interpreter while its shader compiles.", true,
        ),
        SettingDef.Toggle(
          "readback_resolve", "GPU", "Read back render targets",
          "Copies GPU-rendered images back to console memory when games read them.", true,
        ),
      ),
    ),
    SettingCategory(
      "Display", "📱",
      listOf(
        SettingDef.Toggle(
          "present_letterbox", "Display", "Keep aspect ratio",
          "Black bars instead of stretching to the full screen.", false,
        ),
      ),
    ),
    SettingCategory(
      "Audio", "🔊",
      listOf(
        SettingDef.Slider(
          "volume", "APU", "Volume", "Master volume.",
          0f, 100f, 19, 100f, true, { "${it.toInt()}%" },
        ),
        SettingDef.Choice(
          "apu_max_queued_frames", "APU", "Audio buffer",
          "Bigger buffers avoid crackles; smaller ones lower latency.",
          listOf("8" to "Small (low latency)", "16" to "Medium", "32" to "Large (no crackles)"),
          "16",
        ),
        SettingDef.Toggle(
          "use_dedicated_xma_thread", "APU", "Dedicated XMA thread",
          "Decodes game audio on its own thread.", true,
        ),
        SettingDef.Toggle(
          "apu_consumer_paced", "APU", "Output-paced audio",
          "Runs the game's audio callback when the device takes a frame, without a timer.",
          true,
        ),
        SettingDef.Choice(
          "apu_aaudio_performance_mode", "APU", "Audio output mode",
          "Power saving uses fewer, larger device callbacks. Try Low latency if audio crackles.",
          listOf(q("power_saving") to "Power saving", q("none") to "Default", q("low_latency") to "Low latency"),
          q("power_saving"),
        ),
        SettingDef.Choice(
          "xma_decoder", "APU", "XMA decoder", "Audio decoder implementation.",
          listOf(q("new") to "New", q("old") to "Old"), q("new"),
        ),
      ),
    ),
    SettingCategory(
      "Power & CPU", "🔋",
      listOf(
        SettingDef.Choice(
          "guest_spin_sleep_us", "CPU", "Spin-wait sleep",
          "Lets busy-waiting game threads rest. Saves heat; set Off if a game misbehaves.",
          listOf("0" to "Off", "25" to "25 µs", "50" to "50 µs", "100" to "100 µs"), "0",
        ),
        SettingDef.Toggle(
          "wfe_precise_sleep", "CPU", "Precise sleep",
          "Spins the last fraction of a millisecond of each short sleep for exact timing. More heat.",
          false,
        ),
        SettingDef.Choice(
          "guest_vblank_ff_multiplier", "GPU", "Fast forward speed",
          "Speed while fast forward (PS / Home button) is on.",
          listOf("2" to "2x", "3" to "3x", "4" to "4x", "6" to "6x", "8" to "8x"), "4",
        ),
      ),
    ),
    SettingCategory(
      "Input", "🎮",
      listOf(
        SettingDef.Toggle("vibration", "HID", "Vibration", "Controller rumble.", true),
      ),
    ),
    SettingCategory(
      "Advanced", "🛠",
      listOf(
        SettingDef.Toggle(
          "vulkan_gpu_completion_hint", "Vulkan", "GPU completion hint",
          "Tracks GPU progress through memory instead of slow fence queries.", true,
        ),
        SettingDef.Toggle(
          "submit_on_primary_buffer_end", "GPU", "Submit per command buffer",
          "Sends GPU work as soon as each game command buffer ends. More, smaller submits.",
          false,
        ),
        SettingDef.Toggle(
          "vulkan_zpd_copy_wait", "Vulkan", "Blocking query copies",
          "Old occlusion query copy path that can hang the GPU. Troubleshooting only.", false,
        ),
        SettingDef.Choice(
          "vulkan_pipeline_creation_threads", "Vulkan", "Shader compile threads",
          "Threads building pipelines in the background.",
          listOf("-1" to "Auto (3)", "1" to "1", "2" to "2", "3" to "3", "4" to "4", "6" to "6"),
          "-1",
        ),
        SettingDef.Choice(
          "log_level", "Logging", "Log level", "How much the emulator logs.",
          listOf("0" to "Errors", "1" to "Warnings", "2" to "Info", "3" to "Debug"), "2",
        ),
      ),
    ),
  )
}
