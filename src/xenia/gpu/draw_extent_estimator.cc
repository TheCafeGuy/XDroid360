/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/draw_extent_estimator.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/graphics_util.h"

DEFINE_bool(
    execute_unclipped_draw_vs_on_cpu, true,
    "Execute the vertex shader for draws with clipping disabled, primarily "
    "screen-space draws (such as clears), on the CPU when possible to estimate "
    "the extent of the EDRAM involved in the draw.\n"
    "Enabling this may significantly improve GPU performance as otherwise up "
    "to the entire EDRAM may be considered used in draws without clipping, "
    "potentially resulting in spurious EDRAM range ownership transfer round "
    "trips between host render targets.\n"
    "Also, on hosts where certain render target formats have to be emulated in "
    "a lossy way (for instance, 16-bit fixed-point via 16-bit floating-point), "
    "this prevents corruption of other render targets located after the "
    "current ones in the EDRAM by lossy range ownership transfers done for "
    "those draws.",
    "GPU.Debug");
DEFINE_bool(
    execute_unclipped_draw_vs_on_cpu_with_scissor, false,
    "Don't restrict the usage of execute_unclipped_draw_vs_on_cpu to only "
    "non-scissored draws (with the right and the bottom sides of the scissor "
    "rectangle at 8192 or beyond) even though if the scissor rectangle is "
    "present, it's usually sufficient for esimating the height of the render "
    "target.\n"
    "Enabling this may cause excessive processing of vertices on the CPU, as "
    "some games draw rectangles (for their UI, for instance) without clipping, "
    "but with a proper scissor rectangle.",
    "GPU.Debug");

namespace xe {
namespace gpu {

void DrawExtentEstimator::PositionYExportSink::Export(
    ucode::ExportRegister export_register, const float* value,
    uint32_t value_mask) {
  if (export_register == ucode::ExportRegister::kVSPosition) {
    if (value_mask & 0b0010) {
      position_y_ = value[1];
    }
    if (value_mask & 0b1000) {
      position_w_ = value[3];
    }
  } else if (export_register ==
             ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
    if (value_mask & 0b0001) {
      point_size_ = value[0];
    }
    if (value_mask & 0b0100) {
      vertex_kill_ = xe::memory::Reinterpret<uint32_t>(value[2]);
    }
  }
}

uint32_t DrawExtentEstimator::EstimateVertexMaxY(const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (!vgt_draw_initiator.num_indices) {
    return 0;
  }
  if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    // TODO(Triang3l): Support immediate indices.
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  // Not reproducing tessellation.
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode,
                                 vgt_draw_initiator.prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  assert_true(vertex_shader.type() == xenos::ShaderType::kVertex);
  assert_true(vertex_shader.is_ucode_analyzed());
  if (!ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  union {
    const void* index_buffer;
    const uint16_t* index_buffer_16;
    const uint32_t* index_buffer_32;
  };
  xenos::Endian index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    xenos::IndexFormat index_format = vgt_draw_initiator.index_size;
    // Mask to physical like the PrimitiveProcessor - the guest may use a
    // mirror window, and TranslatePhysical only covers 512 MB.
    uint32_t index_buffer_base = xenos::CpuToGpu(regs[XE_GPU_REG_VGT_DMA_BASE]);
    uint32_t index_buffer_read_count =
        std::min(uint32_t(vgt_draw_initiator.num_indices),
                 uint32_t(vgt_dma_size.num_words));
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
      // Handle the index endianness to same way as the PrimitiveProcessor.
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
      index_buffer_base &= ~uint32_t(sizeof(uint16_t) - 1);
      if (trace_writer_) {
        trace_writer_->WriteMemoryRead(
            index_buffer_base, sizeof(uint16_t) * index_buffer_read_count);
      }
    } else {
      assert_true(vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32);
      index_buffer_base &= ~uint32_t(sizeof(uint32_t) - 1);
      if (trace_writer_) {
        trace_writer_->WriteMemoryRead(
            index_buffer_base, sizeof(uint32_t) * index_buffer_read_count);
      }
    }
    index_buffer = memory_.TranslatePhysical(index_buffer_base);
  }
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  uint32_t reset_index =
      regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;

  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  float viewport_y_scale = pa_cl_vte_cntl.vport_y_scale_ena
                               ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE)
                               : 1.0f;
  float viewport_y_offset =
      pa_cl_vte_cntl.vport_y_offset_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET)
          : 0.0f;

  int32_t point_vertex_min_diameter_float = 0;
  int32_t point_vertex_max_diameter_float = 0;
  float point_constant_radius_y = 0.0f;
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    point_vertex_min_diameter_float = xe::memory::Reinterpret<int32_t>(
        float(pa_su_point_minmax.min_size) * (2.0f / 16.0f));
    point_vertex_max_diameter_float = xe::memory::Reinterpret<int32_t>(
        float(pa_su_point_minmax.max_size) * (2.0f / 16.0f));
    point_constant_radius_y =
        float(regs.Get<reg::PA_SU_POINT_SIZE>().height) * (1.0f / 16.0f);
  }

  float max_y = -FLT_MAX;

  shader_interpreter_.SetShader(vertex_shader);

  PositionYExportSink position_y_export_sink;
  shader_interpreter_.SetExportSink(&position_y_export_sink);
  for (uint32_t i = 0; i < vgt_draw_initiator.num_indices; ++i) {
    uint32_t vertex_index;
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
      if (i < vgt_dma_size.num_words) {
        if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
          vertex_index = index_buffer_16[i];
        } else {
          vertex_index = index_buffer_32[i];
        }
        // The Xenos only uses 24 bits of the index (reset_indx is 24-bit).
        vertex_index = xenos::GpuSwap(vertex_index, index_endian) & 0xFFFFFF;
      } else {
        vertex_index = 0;
      }
      if (pa_su_sc_mode_cntl.multi_prim_ib_ena && vertex_index == reset_index) {
        continue;
      }
    } else {
      assert_true(vgt_draw_initiator.source_select ==
                  xenos::SourceSelect::kAutoIndex);
      vertex_index = i;
    }
    vertex_index =
        std::min(max_index,
                 std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));

    position_y_export_sink.Reset();

    shader_interpreter_.temp_registers()[0] = float(vertex_index);
    shader_interpreter_.Execute();

    if (position_y_export_sink.vertex_kill().has_value() &&
        (position_y_export_sink.vertex_kill().value() & ~(UINT32_C(1) << 31))) {
      continue;
    }
    if (!position_y_export_sink.position_y().has_value()) {
      continue;
    }
    float vertex_y = position_y_export_sink.position_y().value();
    if (!pa_cl_vte_cntl.vtx_xy_fmt) {
      if (!position_y_export_sink.position_w().has_value()) {
        continue;
      }
      vertex_y /= position_y_export_sink.position_w().value();
    }

    vertex_y = vertex_y * viewport_y_scale + viewport_y_offset;

    if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
      float point_radius_y;
      if (position_y_export_sink.point_size().has_value()) {
        // Vertex-specified diameter. Clamped effectively as a signed integer in
        // the hardware, -NaN, -Infinity ... -0 to the minimum, +Infinity, +NaN
        // to the maximum.
        point_radius_y =
            0.5f *
            xe::memory::Reinterpret<float>(std::min(
                point_vertex_max_diameter_float,
                std::max(point_vertex_min_diameter_float,
                         xe::memory::Reinterpret<int32_t>(
                             position_y_export_sink.point_size().value()))));
      } else {
        // Constant radius.
        point_radius_y = point_constant_radius_y;
      }
      vertex_y += point_radius_y;
    }

    // std::max is `a < b ? b : a`, thus in case of NaN, the first argument is
    // always returned - max_y, which is initialized to a normalized value.
    max_y = std::max(max_y, vertex_y);
  }
  shader_interpreter_.SetExportSink(nullptr);

  int32_t max_y_24p8 = ui::FloatToD3D11Fixed16p8(max_y);
  // 16p8 range is -32768 to 32767+255/256, but it's stored as uint32_t here,
  // as 24p8, so overflowing up to -8388608 to 8388608+255/256 is safe. The
  // range of the window offset plus the half-pixel offset is -16384 to 16384.5,
  // so it's safe to add both - adding it will neither move the 16p8 clamping
  // bounds -32768 and 32767+255/256 into the 0...8192 screen space range, nor
  // cause 24p8 overflow.
  if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center ==
      xenos::PixelCenter::kD3DZero) {
    max_y_24p8 += 128;
  }
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    max_y_24p8 += regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset * 256;
  }
  // Top-left rule - .5 exclusive without MSAA, 1. exclusive with MSAA.
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  return (uint32_t(std::max(int32_t(0), max_y_24p8)) +
          ((rb_surface_info.msaa_samples == xenos::MsaaSamples::k1X) ? 127
                                                                     : 255)) >>
         8;
}

void DrawExtentEstimator::PositionExportSink::Export(
    ucode::ExportRegister export_register, const float* value,
    uint32_t value_mask) {
  if (export_register == ucode::ExportRegister::kVSPosition) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (value_mask & (1u << i)) {
        position_[i] = value[i];
      }
    }
    position_mask_ |= value_mask;
  } else if (export_register ==
             ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
    if ((value_mask & 0b0100) &&
        (xe::memory::Reinterpret<uint32_t>(value[2]) & ~(UINT32_C(1) << 31))) {
      vertex_kill_ = true;
    }
  }
}

bool DrawExtentEstimator::EstimateRectangleCover(const Shader& vertex_shader,
                                                 int32_t& left, int32_t& top,
                                                 int32_t& right,
                                                 int32_t& bottom) {
  SCOPE_profile_cpu_f("gpu");
  const RegisterFile& regs = register_file_;

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vertex_count;
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kRectangleList) {
    vertex_count = 3;
  } else if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kQuadList) {
    vertex_count = 4;
  } else {
    return RejectRectangleCover(kRejectPrimitive);
  }
  if (vgt_draw_initiator.num_indices != vertex_count ||
      (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
       vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex)) {
    return RejectRectangleCover(kRejectVertexCount);
  }
  // A clipped draw keeps its whole shape only when every vertex is inside
  // the clip volume (checked per vertex below) and no user plane is on.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const bool clipped = !pa_cl_clip_cntl.clip_disable;
  if (clipped && (pa_cl_clip_cntl.value & 0b111111)) {
    return RejectRectangleCover(kRejectClipped);
  }
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode,
                                 vgt_draw_initiator.prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return RejectRectangleCover(kRejectTessellated);
  }
  if (!vertex_shader.is_ucode_analyzed() ||
      !ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return RejectRectangleCover(kRejectShader);
  }

  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  const void* index_buffer = nullptr;
  xenos::Endian index_endian = vgt_dma_size.swap_mode;
  const bool index_16 =
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16;
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    if (vgt_dma_size.num_words < vertex_count) {
      return RejectRectangleCover(kRejectIndices);
    }
    uint32_t index_buffer_base = xenos::CpuToGpu(regs[XE_GPU_REG_VGT_DMA_BASE]);
    if (index_16) {
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
      index_buffer_base &= ~uint32_t(sizeof(uint16_t) - 1);
    } else {
      index_buffer_base &= ~uint32_t(sizeof(uint32_t) - 1);
    }
    if (trace_writer_) {
      trace_writer_->WriteMemoryRead(
          index_buffer_base,
          uint32_t(index_16 ? sizeof(uint16_t) : sizeof(uint32_t)) *
              vertex_count);
    }
    index_buffer = memory_.TranslatePhysical(index_buffer_base);
  }
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  uint32_t reset_index =
      regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;

  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  const float scale_x = pa_cl_vte_cntl.vport_x_scale_ena
                            ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE)
                            : 1.0f;
  const float offset_x = pa_cl_vte_cntl.vport_x_offset_ena
                             ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET)
                             : 0.0f;
  const float scale_y = pa_cl_vte_cntl.vport_y_scale_ena
                            ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE)
                            : 1.0f;
  const float offset_y = pa_cl_vte_cntl.vport_y_offset_ena
                             ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET)
                             : 0.0f;
  // Half-pixel and window offsets, added after the viewport transform.
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  float add_x = 0.0f, add_y = 0.0f;
  if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center ==
      xenos::PixelCenter::kD3DZero) {
    add_x += 0.5f;
    add_y += 0.5f;
  }
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    add_x += float(pa_sc_window_offset.window_x_offset);
    add_y += float(pa_sc_window_offset.window_y_offset);
  }

  float xs[4], ys[4];
  shader_interpreter_.SetShader(vertex_shader);
  PositionExportSink sink;
  shader_interpreter_.SetExportSink(&sink);
  bool ok = true;
  bool clip_rejected = false;
  for (uint32_t i = 0; i < vertex_count; ++i) {
    uint32_t vertex_index = i;
    if (index_buffer) {
      vertex_index = index_16 ? static_cast<const uint16_t*>(index_buffer)[i]
                              : static_cast<const uint32_t*>(index_buffer)[i];
      vertex_index = xenos::GpuSwap(vertex_index, index_endian) & 0xFFFFFF;
      if (pa_su_sc_mode_cntl.multi_prim_ib_ena && vertex_index == reset_index) {
        ok = false;
        break;
      }
    }
    vertex_index =
        std::min(max_index,
                 std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));
    sink.Reset();
    shader_interpreter_.temp_registers()[0] = float(vertex_index);
    shader_interpreter_.Execute();
    if (sink.vertex_kill_ || (sink.position_mask_ & 0b0011) != 0b0011) {
      ok = false;
      break;
    }
    float x = sink.position_[0], y = sink.position_[1];
    if (clipped) {
      // Inside the view volume: -w <= x, y <= w, and 0 (D3D) or -w <= z <= w.
      const float w = sink.position_[3], z = sink.position_[2];
      if ((sink.position_mask_ & 0b1111) != 0b1111 ||
          pa_cl_vte_cntl.vtx_xy_fmt || !(w > 0.0f) || !(std::abs(x) <= w) ||
          !(std::abs(y) <= w) || !(z <= w) ||
          !(pa_cl_clip_cntl.dx_clip_space_def ? z >= 0.0f : z >= -w)) {
        clip_rejected = true;
        ok = false;
        break;
      }
    }
    if (!pa_cl_vte_cntl.vtx_xy_fmt) {
      if (!(sink.position_mask_ & 0b1000) || !(sink.position_[3] > 0.0f)) {
        ok = false;
        break;
      }
      x /= sink.position_[3];
      y /= sink.position_[3];
    }
    xs[i] = x * scale_x + offset_x + add_x;
    ys[i] = y * scale_y + offset_y + add_y;
    if (!std::isfinite(xs[i]) || !std::isfinite(ys[i])) {
      ok = false;
      break;
    }
  }
  shader_interpreter_.SetExportSink(nullptr);
  if (!ok) {
    return RejectRectangleCover(clip_rejected ? kRejectClipped : kRejectVertex);
  }

  // Axis-aligned: every corner takes one of two X and one of two Y values,
  // and at least 3 of the 4 combinations appear (a rectangle list gives 3).
  float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
  for (uint32_t i = 1; i < vertex_count; ++i) {
    x0 = std::min(x0, xs[i]);
    x1 = std::max(x1, xs[i]);
    y0 = std::min(y0, ys[i]);
    y1 = std::max(y1, ys[i]);
  }
  bool seen_corner[4] = {};
  for (uint32_t i = 0; i < vertex_count; ++i) {
    if ((xs[i] != x0 && xs[i] != x1) || (ys[i] != y0 && ys[i] != y1)) {
      return RejectRectangleCover(kRejectNotAxisAligned);
    }
    seen_corner[(xs[i] == x1 ? 1 : 0) | (ys[i] == y1 ? 2 : 0)] = true;
  }
  uint32_t corners = 0;
  for (bool corner : seen_corner) {
    corners += corner ? 1 : 0;
  }
  if (corners < 3 || !(x1 > x0) || !(y1 > y0)) {
    return RejectRectangleCover(kRejectNotAxisAligned);
  }

  // Pixels whose every sample is inside. Without MSAA the center (x + 0.5)
  // must be in [x0, x1) (top-left rule); with MSAA the whole pixel must be,
  // which holds for any sample pattern.
  const bool msaa =
      regs.Get<reg::RB_SURFACE_INFO>().msaa_samples != xenos::MsaaSamples::k1X;
  const float limit = float(xenos::kTexture2DCubeMaxWidthHeight * 2);
  x0 = std::max(-limit, std::min(limit, x0));
  x1 = std::max(-limit, std::min(limit, x1));
  y0 = std::max(-limit, std::min(limit, y0));
  y1 = std::max(-limit, std::min(limit, y1));
  if (msaa) {
    left = int32_t(std::ceil(x0));
    top = int32_t(std::ceil(y0));
    right = int32_t(std::floor(x1));
    bottom = int32_t(std::floor(y1));
  } else {
    left = int32_t(std::ceil(x0 - 0.5f));
    top = int32_t(std::ceil(y0 - 0.5f));
    right = int32_t(std::ceil(x1 - 0.5f));
    bottom = int32_t(std::ceil(y1 - 0.5f));
  }

  // Scissors.
  auto window_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  auto window_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t scissor_left = int32_t(window_tl.tl_x);
  int32_t scissor_top = int32_t(window_tl.tl_y);
  int32_t scissor_right = int32_t(window_br.br_x);
  int32_t scissor_bottom = int32_t(window_br.br_y);
  if (!window_tl.window_offset_disable) {
    scissor_left += pa_sc_window_offset.window_x_offset;
    scissor_right += pa_sc_window_offset.window_x_offset;
    scissor_top += pa_sc_window_offset.window_y_offset;
    scissor_bottom += pa_sc_window_offset.window_y_offset;
  }
  auto screen_tl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
  auto screen_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  left = std::max(left, std::max(scissor_left, int32_t(screen_tl.tl_x)));
  top = std::max(top, std::max(scissor_top, int32_t(screen_tl.tl_y)));
  right = std::min(right, std::min(scissor_right, int32_t(screen_br.br_x)));
  bottom = std::min(bottom, std::min(scissor_bottom, int32_t(screen_br.br_y)));
  if (!(right > left && bottom > top)) {
    return RejectRectangleCover(kRejectScissored);
  }
  ++rectangle_cover_counts[kCovered];
  return true;
}

uint32_t DrawExtentEstimator::EstimateMaxY(bool try_to_estimate_vertex_max_y,
                                           const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  int32_t window_y_offset = pa_sc_window_offset.window_y_offset;

  // Scissor.
  auto pa_sc_window_scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t scissor_bottom = int32_t(pa_sc_window_scissor_br.br_y);
  bool scissor_window_offset =
      !regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().window_offset_disable;
  if (scissor_window_offset) {
    scissor_bottom += window_y_offset;
  }
  auto pa_sc_screen_scissor_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  scissor_bottom =
      std::min(scissor_bottom, int32_t(pa_sc_screen_scissor_br.br_y));
  uint32_t max_y = uint32_t(std::max(scissor_bottom, int32_t(0)));

  if (regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable) {
    // Actual extent from the vertices.
    if (try_to_estimate_vertex_max_y &&
        cvars::execute_unclipped_draw_vs_on_cpu) {
      bool estimate_vertex_max_y;
      if (cvars::execute_unclipped_draw_vs_on_cpu_with_scissor) {
        estimate_vertex_max_y = true;
      } else {
        estimate_vertex_max_y = false;
        if (scissor_bottom >= xenos::kTexture2DCubeMaxWidthHeight) {
          // Handle just the usual special 8192x8192 case in Direct3D 9 - 8192
          // may be a normal render target height (80x8192 is well within the
          // EDRAM size, for instance), no need to process the vertices on the
          // CPU in this case.
          int32_t scissor_right = int32_t(pa_sc_window_scissor_br.br_x);
          if (scissor_window_offset) {
            scissor_right += pa_sc_window_offset.window_x_offset;
          }
          scissor_right =
              std::min(scissor_right, int32_t(pa_sc_screen_scissor_br.br_x));
          if (scissor_right >= xenos::kTexture2DCubeMaxWidthHeight) {
            estimate_vertex_max_y = true;
          }
        }
      }
      if (estimate_vertex_max_y) {
        max_y = std::min(max_y, EstimateVertexMaxY(vertex_shader));
      }
    }
  } else {
    // Viewport. Though the Xenos itself doesn't have an implicit viewport
    // scissor (it's set by Direct3D 9 when a viewport is used), on hosts, it
    // usually exists and can't be disabled.
    auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    float viewport_bottom = 0.0f;
    // First calculate all the integer.0 or integer.5 offsetting exactly at full
    // precision.
    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
      viewport_bottom += float(window_y_offset);
    }
    if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center ==
        xenos::PixelCenter::kD3DZero) {
      viewport_bottom += 0.5f;
    }
    // Then apply the floating-point viewport offset.
    if (pa_cl_vte_cntl.vport_y_offset_ena) {
      viewport_bottom += regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
    }
    viewport_bottom +=
        pa_cl_vte_cntl.vport_y_scale_ena
            ? std::abs(regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE))
            : 1.0f;
    // Using floor, or, rather, truncation (because maxing with zero anyway)
    // similar to how viewport scissoring behaves on real AMD, Intel and Nvidia
    // GPUs on Direct3D 12 (but not WARP), also like in
    // draw_util::GetHostViewportInfo.
    // max(0.0f, viewport_bottom) to drop NaN and < 0 - max picks the first
    // argument in the !(a < b) case (always for NaN), min as float (max_y is
    // well below 2^24) to safely drop very large values.
    max_y = uint32_t(std::min(float(max_y), std::max(0.0f, viewport_bottom)));
  }

  return max_y;
}

}  // namespace gpu
}  // namespace xe
