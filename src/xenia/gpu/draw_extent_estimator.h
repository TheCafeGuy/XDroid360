/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
#define XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_

#include <cstdint>
#include <optional>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const RegisterFile& register_file, const Memory& memory,
                      TraceWriter* trace_writer)
      : register_file_(register_file),
        memory_(memory),
        trace_writer_(trace_writer),
        shader_interpreter_(register_file, memory) {
    shader_interpreter_.SetTraceWriter(trace_writer);
  }

  // The shader must have its ucode analyzed.
  uint32_t EstimateVertexMaxY(const Shader& vertex_shader);
  // A4: for a single unclipped rectangle (3-vertex rectangle list) or quad
  // (4-vertex quad list) whose corners are axis-aligned, the pixels every
  // sample of which the draw covers, after the scissors, as [left, right) x
  // [top, bottom). False when that can't be proven (other primitives, the
  // vertex shader can't run on the CPU, vertex kill, non-rectangular shape).
  bool EstimateRectangleCover(const Shader& vertex_shader, int32_t& left,
                              int32_t& top, int32_t& right, int32_t& bottom);
  // Outcome counts of EstimateRectangleCover, for the A3 census line.
  enum RectangleCoverOutcome : uint32_t {
    kCovered,
    kRejectPrimitive,
    kRejectVertexCount,
    kRejectClipped,
    kRejectTessellated,
    kRejectShader,
    kRejectIndices,
    kRejectVertex,
    kRejectNotAxisAligned,
    kRejectScissored,
    kRectangleCoverOutcomeCount,
  };
  uint64_t rectangle_cover_counts[kRectangleCoverOutcomeCount] = {};
  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y,
                        const Shader& vertex_shader);

 private:
  bool RejectRectangleCover(RectangleCoverOutcome outcome) {
    ++rectangle_cover_counts[outcome];
    return false;
  }

  class PositionExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;
    void Reset() {
      position_mask_ = 0;
      vertex_kill_ = false;
    }
    uint32_t position_mask_ = 0;
    float position_[4] = {};
    bool vertex_kill_ = false;
  };

  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<float>& position_y() const { return position_y_; }
    const std::optional<float>& position_w() const { return position_w_; }
    const std::optional<float>& point_size() const { return point_size_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<float> position_y_;
    std::optional<float> position_w_;
    std::optional<float> point_size_;
    std::optional<uint32_t> vertex_kill_;
  };

  const RegisterFile& register_file_;
  const Memory& memory_;
  TraceWriter* trace_writer_;

  ShaderInterpreter shader_interpreter_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
