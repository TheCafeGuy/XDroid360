// XDroid360 BLACK-H1: inf/NaN operand analysis. See xdroid_nonfinite.h.
#include "xdroid_nonfinite.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "xenia/gpu/shader.h"
#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xdroid_gpu_hooks.h"

namespace xdroid {

namespace {

using xe::gpu::InstructionOperand;
using xe::gpu::InstructionResult;
using xe::gpu::InstructionStorageAddressingMode;
using xe::gpu::InstructionStorageSource;
using xe::gpu::InstructionStorageTarget;
using xe::gpu::ParsedAluInstruction;
using xe::gpu::Shader;
using xe::gpu::ucode::AluScalarOpcode;
using xe::gpu::ucode::AluVectorOpcode;
using xe::gpu::ucode::ControlFlowOpcode;

constexpr uint32_t kMaxIterations = 64;
constexpr uint8_t kAllMayBeNonFinite = 0x0F;

struct Alu {
  uint32_t address;
  // Conditional exec or predicated: the writes may not happen.
  bool may_skip;
  ParsedAluInstruction instr;
};

struct State {
  // Per temp, bit c = component c may be inf/NaN.
  std::array<uint8_t, 64> temps{};
  // The previous scalar result may be inf/NaN.
  bool prev = false;
  // A relative write stored inf/NaN somewhere.
  bool relative = false;

  bool operator==(const State& other) const {
    return temps == other.temps && prev == other.prev &&
           relative == other.relative;
  }

  // Whether the source components of |op| for result components |comps| may
  // be inf/NaN.
  bool Operand(const InstructionOperand& op, uint32_t comps) const {
    if (op.storage_source != InstructionStorageSource::kRegister) {
      return false;
    }
    if (op.storage_addressing_mode !=
        InstructionStorageAddressingMode::kAbsolute) {
      if (relative) {
        return true;
      }
      for (uint8_t t : temps) {
        if (t) {
          return true;
        }
      }
      return false;
    }
    uint32_t source_mask = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (comps & (1u << i)) {
        // k0/k1 (4/5) are constants and fall outside the low 4 bits.
        source_mask |= 1u << uint32_t(op.GetComponent(i));
      }
    }
    return (temps[op.storage_index & 63] & source_mask & 0xF) != 0;
  }

  // Result components each vector operand feeds.
  static uint32_t VectorComps(AluVectorOpcode opcode, uint32_t operand) {
    switch (opcode) {
      case AluVectorOpcode::kDp3:
        return 0b0111;
      case AluVectorOpcode::kDp2Add:
        return operand == 2 ? 0b0001 : 0b0011;
      default:
        return 0b1111;
    }
  }

  bool VectorResult(const ParsedAluInstruction& instr) const {
    switch (instr.vector_opcode) {
      case AluVectorOpcode::kSeq:
      case AluVectorOpcode::kSgt:
      case AluVectorOpcode::kSge:
      case AluVectorOpcode::kSne:
      case AluVectorOpcode::kKillEq:
      case AluVectorOpcode::kKillGt:
      case AluVectorOpcode::kKillGe:
      case AluVectorOpcode::kKillNe:
        return false;
      default:
        break;
    }
    for (uint32_t i = 0; i < instr.vector_operand_count; ++i) {
      if (Operand(instr.vector_operands[i],
                  VectorComps(instr.vector_opcode, i))) {
        return true;
      }
    }
    return false;
  }

  bool ScalarResult(const ParsedAluInstruction& instr) const {
    switch (instr.scalar_opcode) {
      case AluScalarOpcode::kSeqs:
      case AluScalarOpcode::kSgts:
      case AluScalarOpcode::kSges:
      case AluScalarOpcode::kSnes:
      case AluScalarOpcode::kSetpEq:
      case AluScalarOpcode::kSetpNe:
      case AluScalarOpcode::kSetpGt:
      case AluScalarOpcode::kSetpGe:
      case AluScalarOpcode::kKillsEq:
      case AluScalarOpcode::kKillsGt:
      case AluScalarOpcode::kKillsGe:
      case AluScalarOpcode::kKillsNe:
      case AluScalarOpcode::kKillsOne:
        return false;
      case AluScalarOpcode::kExp:
      case AluScalarOpcode::kLog:
      case AluScalarOpcode::kLogc:
      case AluScalarOpcode::kRcp:
      case AluScalarOpcode::kRcpc:
      case AluScalarOpcode::kRsq:
      case AluScalarOpcode::kRsqc:
      case AluScalarOpcode::kRsqf:
      case AluScalarOpcode::kSqrt:
      case AluScalarOpcode::kSetpClr:
      case AluScalarOpcode::kMulsPrev2:
        return true;
      case AluScalarOpcode::kRetainPrev:
        return prev;
      default:
        break;
    }
    bool result = false;
    for (uint32_t i = 0; i < instr.scalar_operand_count; ++i) {
      result |= Operand(instr.scalar_operands[i], 0b0011);
    }
    switch (instr.scalar_opcode) {
      case AluScalarOpcode::kAddsPrev:
      case AluScalarOpcode::kSubsPrev:
      case AluScalarOpcode::kMulsPrev:
        return result || prev;
      default:
        return result;
    }
  }

  // Bits 0/1: vector product operand 0/1, bits 2/3: scalar product operand
  // 0/1, in ZeroIfAnyOperandIsZero argument order.
  uint8_t Products(const ParsedAluInstruction& instr) const {
    uint32_t bits = 0;
    switch (instr.vector_opcode) {
      case AluVectorOpcode::kMul:
      case AluVectorOpcode::kMad:
      case AluVectorOpcode::kDp4:
      case AluVectorOpcode::kDp3:
      case AluVectorOpcode::kDp2Add:
      case AluVectorOpcode::kDst: {
        uint32_t comps = VectorComps(instr.vector_opcode, 0);
        bits |= uint32_t(Operand(instr.vector_operands[0], comps));
        bits |= uint32_t(Operand(instr.vector_operands[1], comps)) << 1;
      } break;
      default:
        break;
    }
    const InstructionOperand& op0 = instr.scalar_operands[0];
    switch (instr.scalar_opcode) {
      case AluScalarOpcode::kMuls:
        bits |= uint32_t(Operand(op0, 0b0001)) << 2;
        bits |= uint32_t(Operand(op0, 0b0010)) << 3;
        break;
      case AluScalarOpcode::kMulsPrev:
        bits |= uint32_t(Operand(op0, 0b0001)) << 2;
        bits |= uint32_t(prev) << 3;
        break;
      case AluScalarOpcode::kMulsPrev2:
        // The multiplication branch already requires a finite ps.
        bits |= uint32_t(Operand(op0, 0b0011)) << 2;
        break;
      case AluScalarOpcode::kMulsc0:
      case AluScalarOpcode::kMulsc1:
        bits |= uint32_t(Operand(op0, 0b0001)) << 2;
        bits |= uint32_t(Operand(instr.scalar_operands[1], 0b0001)) << 3;
        break;
      default:
        break;
    }
    return uint8_t(bits);
  }

  void Write(const InstructionResult& result, bool tainted, bool union_only) {
    uint32_t mask = result.GetUsedWriteMask();
    if (result.storage_target != InstructionStorageTarget::kRegister || !mask) {
      return;
    }
    if (result.storage_addressing_mode !=
        InstructionStorageAddressingMode::kAbsolute) {
      if (tainted) {
        relative = true;
        for (uint8_t& t : temps) {
          t |= uint8_t(mask);
        }
      }
      return;
    }
    uint8_t& t = temps[result.storage_index & 63];
    if (!union_only) {
      t &= uint8_t(~mask);
    }
    if (tainted) {
      t |= uint8_t(mask);
    }
  }

  // Both results read the state from before the instruction.
  void Step(const Alu& alu, bool union_only) {
    const ParsedAluInstruction& instr = alu.instr;
    bool vector_tainted = VectorResult(instr);
    bool scalar_tainted = ScalarResult(instr);
    bool keep = union_only || alu.may_skip;
    Write(instr.vector_and_constant_result, vector_tainted, keep);
    Write(instr.scalar_result, scalar_tainted, keep);
    if (instr.scalar_opcode != AluScalarOpcode::kRetainPrev) {
      prev = keep ? (prev || scalar_tainted) : scalar_tainted;
    }
  }
};

void AnalyzeNonFinite(const Shader& shader, std::vector<uint8_t>& out) {
  const std::vector<uint32_t>& ucode = shader.ucode_data();
  uint32_t address_count = uint32_t(ucode.size() / 3);
  out.assign(address_count, kAllMayBeNonFinite);

  std::vector<Alu> alus;
  bool loopy = false;
  for (uint32_t i = 0; i < shader.cf_pair_index_bound(); ++i) {
    xe::gpu::ucode::ControlFlowInstruction cf_ab[2];
    xe::gpu::ucode::UnpackControlFlowInstructions(ucode.data() + i * 3, cf_ab);
    for (const auto& cf : cf_ab) {
      ControlFlowOpcode opcode = cf.opcode();
      switch (opcode) {
        case ControlFlowOpcode::kLoopStart:
        case ControlFlowOpcode::kLoopEnd:
        case ControlFlowOpcode::kCondCall:
        case ControlFlowOpcode::kReturn:
        case ControlFlowOpcode::kCondJmp:
          loopy = true;
          break;
        default:
          break;
      }
      if (!xe::gpu::ucode::IsControlFlowOpcodeExec(opcode)) {
        continue;
      }
      bool conditional = opcode != ControlFlowOpcode::kExec &&
                         opcode != ControlFlowOpcode::kExecEnd;
      uint32_t sequence = cf.exec.sequence();
      for (uint32_t k = 0; k < cf.exec.count(); ++k, sequence >>= 2) {
        uint32_t address = cf.exec.address() + k;
        if ((sequence & 1) || address >= address_count) {
          continue;
        }
        Alu alu;
        alu.address = address;
        xe::gpu::ParseAluInstruction(
            *reinterpret_cast<const xe::gpu::ucode::AluInstruction*>(
                ucode.data() + address * 3),
            shader.type(), alu.instr);
        alu.may_skip = conditional || alu.instr.is_predicated;
        alus.push_back(alu);
      }
    }
  }

  State state;
  if (loopy) {
    // Union fixpoint: every write may or may not happen, in any order.
    uint32_t iteration = 0;
    for (;; ++iteration) {
      if (iteration == kMaxIterations) {
        return;
      }
      State before = state;
      for (const Alu& alu : alus) {
        state.Step(alu, true);
      }
      if (state == before) {
        break;
      }
    }
  }
  std::vector<bool> seen(address_count);
  for (const Alu& alu : alus) {
    uint8_t bits = state.Products(alu.instr);
    if (seen[alu.address]) {
      out[alu.address] |= bits;
    } else {
      out[alu.address] = bits;
      seen[alu.address] = true;
    }
    // The loopy record pass reads the converged state and writes nothing: a
    // clean write that a jump can skip must not clear taint.
    if (!loopy) {
      state.Step(alu, false);
    }
  }
}

}  // namespace

void InstallNonFiniteAnalysis() {
  xe::gpu::GetXdroidGpuHooks().nonfinite_operands.store(
      AnalyzeNonFinite, std::memory_order_release);
}

}  // namespace xdroid
