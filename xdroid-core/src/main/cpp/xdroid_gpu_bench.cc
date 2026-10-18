/**
 * XDroid360: GPU-BENCH - replay a captured GPU trace in a loop.
 *
 * Gameplay A/B runs carry +-4-5 ms of GPU noise per frame (scene, flight
 * path, heat), which hides every smaller win. Replaying the same captured
 * frames removes the scene variance: each iteration submits the identical
 * command stream. The first pass loads the whole trace (its initial memory
 * snapshot included); later passes only rewrite guest memory that differs,
 * so a static texture is not reloaded every iteration while the data that
 * changes between the captured frames still is, as in the game.
 *
 * Output: an XDROID-BENCH line per 10 loops and a summary (ms per frame of
 * wall time, which is the GPU time once the GPU is the bottleneck), plus the
 * per-pass GPU profile (xdroid_pass_profiler) every replayed frame.
 */

#include "xdroid_gpu_bench.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/trace_player.h"
#include "xenia/ui/presenter.h"
#include "third_party/stb/stb_image_write.h"

DEFINE_string(xdroid_gpu_bench, "",
              "GPU-BENCH: path of a GPU trace (.xtr) to replay in a loop "
              "instead of running the title (empty = off).",
              "GPU");
DEFINE_uint32(xdroid_gpu_bench_loops, 300,
              "GPU-BENCH: replay passes over the whole trace.", "GPU");
DEFINE_uint32(xdroid_gpu_bench_rest_ms, 0,
              "GPU-BENCH: idle time after each replayed frame, so the system "
              "UI stays responsive during heavy traces (excluded from the "
              "wall time). It lets the GPU clock drop, so gpubench.sh sets "
              "it only for the cutscene trace.",
              "GPU");
DEFINE_uint32(xdroid_gpu_bench_profile, 1,
              "GPU-BENCH: xdroid_pass_profiler value during the replay (1 = "
              "every frame, 0 = keep the configured one).",
              "GPU");
DECLARE_uint32(xdroid_pass_profiler);

namespace xdroid {
namespace {

double Percentile(std::vector<double> values, double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  size_t index = size_t(fraction * double(values.size() - 1) + 0.5);
  return values[std::min(index, values.size() - 1)];
}

// The guest output after the last replayed frame, so two arms can be checked
// for identical images (an optimization must not change a single pixel).
void SaveLastFrame(xe::gpu::GraphicsSystem* graphics_system,
                   const std::string& trace_path, int frame) {
  xe::ui::Presenter* presenter = graphics_system->presenter();
  xe::ui::RawImage image;
  if (!presenter || !presenter->CaptureGuestOutput(image)) {
    XELOGW("XDROID-BENCH: could not capture the guest output");
    return;
  }
  std::string png = trace_path + ".f" + std::to_string(frame) + ".png";
  if (stbi_write_png(png.c_str(), int(image.width), int(image.height), 4,
                     image.data.data(), int(image.stride))) {
    XELOGI("XDROID-BENCH: last frame {}x{} saved to {}", image.width,
           image.height, png);
  }
}

void BenchThread(xe::gpu::GraphicsSystem* graphics_system, std::string path) {
  auto player = std::make_unique<xe::gpu::TracePlayer>(graphics_system);
  if (!player->Open(path)) {
    XELOGE("XDROID-BENCH: could not open the trace {}", path);
    return;
  }
  const int frame_count = player->frame_count();
  if (frame_count <= 0) {
    XELOGE("XDROID-BENCH: the trace {} has no frames", path);
    return;
  }
  XELOGI("XDROID-BENCH: start trace={} frames={} loops={}", path,
         frame_count, cvars::xdroid_gpu_bench_loops);

  // Warm-up: load every frame with all its memory, twice, so pipelines,
  // textures and render targets exist before timing starts.
  for (int pass = 0; pass < 2; ++pass) {
    for (int frame = 0; frame < frame_count; ++frame) {
      player->PlayFrame(frame, pass != 0);
      player->WaitOnPlayback();
    }
  }
  if (cvars::xdroid_gpu_bench_profile) {
    cvars::xdroid_pass_profiler = cvars::xdroid_gpu_bench_profile;
  }

  std::vector<double> frame_ms;
  frame_ms.reserve(cvars::xdroid_gpu_bench_loops);
  for (uint32_t loop = 0; loop < cvars::xdroid_gpu_bench_loops; ++loop) {
    auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration rested{};
    const bool last_loop = loop + 1 == cvars::xdroid_gpu_bench_loops;
    for (int frame = 0; frame < frame_count; ++frame) {
      player->PlayFrame(frame, true);
      player->WaitOnPlayback();
      // Leave the GPU idle for a moment after each frame so the system
      // compositor gets its turn - back-to-back heavy cutscene frames froze
      // the phone's whole UI. Not counted in the wall time; the pass
      // profiler times the GPU work itself.
      if (cvars::xdroid_gpu_bench_rest_ms) {
        auto rest_start = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(cvars::xdroid_gpu_bench_rest_ms));
        rested += std::chrono::steady_clock::now() - rest_start;
      }
      if (last_loop) {
        // Every frame of the last pass (untimed share is tiny next to the
        // loop), so arms can be compared on all the traced frames.
        SaveLastFrame(graphics_system, path, frame);
      }
    }
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start - rested)
                    .count() /
                double(frame_count);
    frame_ms.push_back(ms);
    if ((loop + 1) % 10 == 0) {
      std::vector<double> last(frame_ms.end() - 10, frame_ms.end());
      XELOGI("XDROID-BENCH: loops {}-{} ms/frame median {:.2f} min {:.2f} "
             "max {:.2f}",
             loop - 8, loop + 1, Percentile(last, 0.5), Percentile(last, 0.0),
             Percentile(last, 1.0));
    }
  }
  XELOGI("XDROID-BENCH: done loops={} ms/frame median {:.3f} p10 {:.3f} p90 "
         "{:.3f} min {:.3f} max {:.3f}",
         frame_ms.size(), Percentile(frame_ms, 0.5),
         Percentile(frame_ms, 0.1), Percentile(frame_ms, 0.9),
         Percentile(frame_ms, 0.0), Percentile(frame_ms, 1.0));
}

}  // namespace

bool StartGpuBench(xe::gpu::GraphicsSystem* graphics_system,
                   const std::filesystem::path& storage_root) {
  if (cvars::xdroid_gpu_bench.empty() || !graphics_system) {
    return false;
  }
  std::filesystem::path path(cvars::xdroid_gpu_bench);
  if (path.is_relative()) {
    path = storage_root / "traces" / path;
  }
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    XELOGE("XDROID-BENCH: trace {} not found; running the title",
           path.string());
    return false;
  }
  // Detached: the replay runs until done; the app is closed to end it.
  std::thread(BenchThread, graphics_system, path.string()).detach();
  return true;
}

}  // namespace xdroid
