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
 *
 * Guard (BENCH-2 P2): a broken arm can keep the GPU busy with 60+ ms frames
 * and freeze the phone's UI (2026-10-19). The replay aborts itself when loops
 * run far slower than the host's expectation, and a watchdog thread ends the
 * process when no frame completes at all (stuck GPU work cannot be cancelled
 * any other way; process death makes the kernel driver drop it).
 */

#include "xdroid_gpu_bench.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "xdroid_live.h"
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
DEFINE_string(xdroid_gpu_bench_run_id, "",
              "GPU-BENCH: id echoed on every XDROID-BENCH line, so the host "
              "never reads a leftover log of an earlier run.",
              "GPU");
DEFINE_bool(xdroid_gpu_bench_guard, true,
            "GPU-BENCH: abort a replay that crawls or hangs (see the slow/"
            "hang cvars) instead of freezing the phone.",
            "GPU");
DEFINE_double(xdroid_gpu_bench_expect_ms, 0.0,
              "GPU-BENCH guard: expected wall ms per frame of this trace "
              "(gpubench.sh passes bench_expect.tsv; 0 = no slow check).",
              "GPU");
DEFINE_double(xdroid_gpu_bench_slow_x, 1.6,
              "GPU-BENCH guard: abort after 3 loops in a row slower than this "
              "times xdroid_gpu_bench_expect_ms.",
              "GPU");
DEFINE_uint32(xdroid_gpu_bench_hang_ms, 4000,
              "GPU-BENCH guard: end the process when one timed frame takes "
              "longer than this (60 s during the warm-up, which compiles "
              "pipelines).",
              "GPU");
DEFINE_string(xdroid_gpu_bench_arms, "",
              "GPU-BENCH in-process A/B: \"a:k=v,k=v;b:k=v\" arms of live "
              "cvars (the xdroid_live.cc list; no string values), switched "
              "every xdroid_gpu_bench_block_loops loops in ABBA order, so "
              "both arms share one launch, warm-up and heat curve.",
              "GPU");
DEFINE_uint32(xdroid_gpu_bench_block_loops, 5,
              "GPU-BENCH: loops per arm block (the first loop of each block "
              "is a switch transient and is left out of its median).",
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

// Shared between the replay and its watchdog: atomics only (rule: no locks
// in a watchdog path).
std::atomic<int64_t> frame_start_ns{0};  // 0 = no frame in flight
std::atomic<bool> timed{false};
std::atomic<bool> finished{false};

struct BenchArm {
  std::string name;
  std::string toml;  // "k = v" lines for ApplyLiveText
};

std::vector<BenchArm> ParseArms(const std::string& spec) {
  std::vector<BenchArm> arms;
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t end = spec.find(';', pos);
    if (end == std::string::npos) {
      end = spec.size();
    }
    std::string item = spec.substr(pos, end - pos);
    pos = end + 1;
    size_t colon = item.find(':');
    if (colon == std::string::npos || !colon) {
      continue;
    }
    BenchArm arm{item.substr(0, colon), std::string()};
    std::string kvs = item.substr(colon + 1);
    size_t kpos = 0;
    while (kpos < kvs.size()) {
      size_t comma = kvs.find(',', kpos);
      if (comma == std::string::npos) {
        comma = kvs.size();
      }
      std::string kv = kvs.substr(kpos, comma - kpos);
      kpos = comma + 1;
      size_t eq = kv.find('=');
      if (eq != std::string::npos) {
        arm.toml += kv.substr(0, eq) + " = " + kv.substr(eq + 1) + "\n";
      }
    }
    arms.push_back(std::move(arm));
  }
  return arms;
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string RunTag() {
  return cvars::xdroid_gpu_bench_run_id.empty()
             ? std::string()
             : " run=" + cvars::xdroid_gpu_bench_run_id;
}

// A frame that never completes is GPU work stuck in the kernel queue: no API
// cancels it, and while it runs the compositor gets no GPU time. Log, give the
// log writer a moment, then end the process so the driver drops the context.
void WatchdogThread() {
  while (!finished.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int64_t start = frame_start_ns.load(std::memory_order_acquire);
    if (!start) {
      continue;
    }
    int64_t limit_ms = timed.load(std::memory_order_acquire)
                           ? int64_t(cvars::xdroid_gpu_bench_hang_ms)
                           : 60000;
    int64_t ms = (NowNs() - start) / 1000000;
    if (ms > limit_ms) {
      XELOGE("XDROID-BENCH: abort reason=hang frame_ms={} limit_ms={}{}", ms,
             limit_ms, RunTag());
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      std::_Exit(3);
    }
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
  XELOGI("XDROID-BENCH: start trace={} frames={} loops={}{}", path,
         frame_count, cvars::xdroid_gpu_bench_loops, RunTag());
  if (cvars::xdroid_gpu_bench_guard) {
    std::thread(WatchdogThread).detach();
  }

  // Warm-up: load every frame with all its memory, twice, so pipelines,
  // textures and render targets exist before timing starts.
  for (int pass = 0; pass < 2; ++pass) {
    for (int frame = 0; frame < frame_count; ++frame) {
      frame_start_ns.store(NowNs(), std::memory_order_release);
      player->PlayFrame(frame, pass != 0);
      player->WaitOnPlayback();
    }
  }
  frame_start_ns.store(0, std::memory_order_release);
  timed.store(true, std::memory_order_release);
  if (cvars::xdroid_gpu_bench_profile) {
    cvars::xdroid_pass_profiler = cvars::xdroid_gpu_bench_profile;
  }

  std::vector<double> frame_ms;
  frame_ms.reserve(cvars::xdroid_gpu_bench_loops);
  const double slow_ms =
      cvars::xdroid_gpu_bench_expect_ms * cvars::xdroid_gpu_bench_slow_x;
  uint32_t slow_streak = 0;
  bool aborted = false;
  const std::vector<BenchArm> arms = ParseArms(cvars::xdroid_gpu_bench_arms);
  const uint32_t block_loops =
      std::max<uint32_t>(cvars::xdroid_gpu_bench_block_loops, 2);
  std::vector<double> block_ms;
  size_t arm_index = 0;
  for (uint32_t loop = 0; loop < cvars::xdroid_gpu_bench_loops; ++loop) {
    if (!arms.empty() && loop % block_loops == 0) {
      // ABBA: even rounds run the arms forward, odd rounds backward, so a
      // linear drift (heat) adds the same to every arm over two rounds.
      const uint32_t block = loop / block_loops;
      const size_t n = arms.size();
      const size_t round = block / n, slot = block % n;
      arm_index = round % 2 ? n - 1 - slot : slot;
      ApplyLiveText(arms[arm_index].toml);
      block_ms.clear();
      XELOGI("XDROID-BENCH: block {} arm={} start{}", block,
             arms[arm_index].name, RunTag());
    }
    auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration rested{};
    const bool last_loop = loop + 1 == cvars::xdroid_gpu_bench_loops;
    for (int frame = 0; frame < frame_count; ++frame) {
      frame_start_ns.store(NowNs(), std::memory_order_release);
      player->PlayFrame(frame, true);
      player->WaitOnPlayback();
      frame_start_ns.store(0, std::memory_order_release);
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
    if (!arms.empty()) {
      if (loop % block_loops) {
        block_ms.push_back(ms);
      }
      if (loop % block_loops == block_loops - 1) {
        XELOGI("XDROID-BENCH: block {} arm={} ms/frame median {:.3f}{}",
               loop / block_loops, arms[arm_index].name,
               Percentile(block_ms, 0.5), RunTag());
      }
    }
    // A crawling arm (60+ ms GPU frames) freezes the phone's UI: stop
    // submitting, which frees the GPU, and let the host collect evidence.
    slow_streak = (slow_ms > 0 && ms > slow_ms) ? slow_streak + 1 : 0;
    if (cvars::xdroid_gpu_bench_guard && slow_streak >= 3) {
      XELOGE("XDROID-BENCH: abort reason=slow loop={} ms/frame {:.2f} "
             "expected {:.2f}{}",
             loop + 1, ms, cvars::xdroid_gpu_bench_expect_ms, RunTag());
      aborted = true;
      break;
    }
    if ((loop + 1) % 10 == 0) {
      std::vector<double> last(frame_ms.end() - 10, frame_ms.end());
      XELOGI("XDROID-BENCH: loops {}-{} ms/frame median {:.2f} min {:.2f} "
             "max {:.2f}{}",
             loop - 8, loop + 1, Percentile(last, 0.5), Percentile(last, 0.0),
             Percentile(last, 1.0), RunTag());
    }
  }
  finished.store(true, std::memory_order_release);
  if (aborted) {
    return;
  }
  XELOGI("XDROID-BENCH: done loops={} ms/frame median {:.3f} p10 {:.3f} p90 "
         "{:.3f} min {:.3f} max {:.3f}{}",
         frame_ms.size(), Percentile(frame_ms, 0.5),
         Percentile(frame_ms, 0.1), Percentile(frame_ms, 0.9),
         Percentile(frame_ms, 0.0), Percentile(frame_ms, 1.0), RunTag());
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
