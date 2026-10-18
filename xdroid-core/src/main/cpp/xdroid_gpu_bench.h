/**
 * XDroid360: GPU-BENCH - replay a captured GPU trace in a loop for noise-free
 * GPU A/B measurements (no gameplay, same frames every iteration).
 */

#ifndef XDROID_GPU_BENCH_H_
#define XDROID_GPU_BENCH_H_

#include <filesystem>

namespace xe::gpu {
class GraphicsSystem;
}  // namespace xe::gpu

namespace xdroid {

// With xdroid_gpu_bench set to a trace file, starts the replay thread and
// returns true: the caller then skips launching the title. False otherwise.
// A relative path is taken from <storage_root>/traces/.
bool StartGpuBench(xe::gpu::GraphicsSystem* graphics_system,
                   const std::filesystem::path& storage_root);

}  // namespace xdroid

#endif  // XDROID_GPU_BENCH_H_
