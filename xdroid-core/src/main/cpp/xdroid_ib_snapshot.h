/**
 * XDroid360 HUD-1 arms F0/F1 (temp, revert-planned): indirect buffers as the
 * guest kicked them.
 *
 * Real Xenos prefetches an indirect buffer shortly after the kick; xenia's
 * command processor reads it from guest memory only when it parses it, about
 * one frame later. hud1-x (docs/runs/2026-10-13/hud1-x-cp-source.md) caught
 * a frame whose HUD draws were the previous frame's: the HUD command block had
 * been rewritten between the kick and the parse.
 *
 * - xdroid_cp_ib_kick_hash (F0): at every kick, walk the new ring commands and
 *   the indirect buffers they call (nested up to depth 4), hash each buffer;
 *   when the CP enters it, hash it again and count/log the differences.
 * - xdroid_cp_snapshot_ib (F1): also copy each buffer at kick into a host
 *   arena; the CP executes the copy.
 *
 * Producer: the guest thread that writes the write pointer. Consumer: the CP
 * thread. Lock-free single producer / single consumer (no lock may be taken
 * in a guest watchdog region, CLAUDE.md rule 8).
 */

#ifndef XDROID_IB_SNAPSHOT_H_
#define XDROID_IB_SNAPSHOT_H_

namespace xdroid {

// Installs the GPU hooks; they do nothing while both cvars are off.
void InstallIbSnapshotHooks();

}  // namespace xdroid

#endif  // XDROID_IB_SNAPSHOT_H_
