// XDroid360 BLACK-H1: translation-time analysis of which multiplication
// operands may be +-inf or NaN, for the exact relaxed-multiply mode
// (xdroid_relaxed_mul bit 2). Where both operands are proven finite, the
// Shader Model 3 "0 * anything = 0" select is dropped; where one is, a single
// compare of the other remains.
//
// Model (a 1:1 port of tools/shader/nonfinite_census.py): a temp component is
// tainted when it may hold inf/NaN. Constants, vertex and texture fetches and
// interpolants are assumed finite, and so are temps never written before a
// read (SPIR-V undefined; SM3 mode masks undef * 0, exact mode does not).
// Sources: exp, log, logc, rcp, rsq, rsqc, rsqf, sqrt and the +-FLT_MAX
// sentinels rcpc, setp_clr, muls_prev2. Other ops follow their operands;
// set/kill results are 0 or 1. Straight-line shaders get a flow-sensitive
// pass; shaders with loops, calls or jumps a union fixpoint.
// Idea only (MIT, nothing copied): Mesa NIR range analysis turns fmulz into
// fmul when both operands are proven finite.
#ifndef XDROID_NONFINITE_H_
#define XDROID_NONFINITE_H_

namespace xdroid {

// Installs XdroidGpuHooks::nonfinite_operands. Call before any translation.
void InstallNonFiniteAnalysis();

}  // namespace xdroid

#endif  // XDROID_NONFINITE_H_
