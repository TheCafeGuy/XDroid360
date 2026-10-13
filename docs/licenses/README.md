# Licenses and notices

XDroid360 is BSD-3-Clause (`LICENSE`, inherited from xenia). The APK also
contains third-party code. Their notices ship inside the app at
`assets/licenses/NOTICES.txt` (About > Open-source licenses), generated at build
time by `:xdroid-app:generateLicenseNotices` from [`notices.tsv`](notices.tsv).
Texts that upstream only keeps inside source headers are copied here
(`*-LICENSE.txt`, `*-NOTICE.txt`).

Inventory as of 2026-10-09 (link line of `libxdroid360.so`, the headers its
sources include, the adrenotools hook libraries and the JVM dependencies). No
GPL code is compiled in. `kgsl.h` in the adrenotools hooks is a Linux UAPI
header under GPL-2.0 WITH Linux-syscall-note, which covers user programs that
use it. zstd is used under its BSD option. The NDK's libc++ is linked
statically under Apache-2.0 WITH LLVM-exception, which needs no notice for
compiled code.

## Keeping it right

- New third-party code in the build (a `target_link_libraries` entry, a
  `third_party` include, a Gradle `implementation`): add a line to
  `notices.tsv`. The build fails if a listed file is missing.
- Code ported from another project: keep its copyright and license in the
  file header if the license asks for it (BSD, MIT), add it to `notices.tsv`,
  and name the source and commit in the commit message. Never GPL/LGPL code.
- Do not enable FFmpeg's GPL parts (`--enable-gpl`, x264, x265, postproc).

## Before any public release (STOP and ask the user)

FFmpeg (LGPL-2.1-or-later) and libmspack (LGPL-2.1) are linked statically,
as in upstream xenia. LGPL 2.1 section 6 then requires, with every APK
distributed:

1. The LGPL text and the notices (done: NOTICES.txt).
2. The source code of the LGPL libraries as shipped: `third_party/FFmpeg` at
   the pinned commit of https://github.com/has207/FFmpeg, and
   `third_party/mspack`.
3. A way to relink with a modified library: publish the complete XDroid360
   source (it builds the same `.so`) next to the APK, or ship the object files.
   NOTICES.txt promises the source "from the same place you obtained this
   app", so the release must actually provide it there, or include a written
   offer valid for three years.

Other release checks: the BSD-3 clause 3 of xenia and AC6_recomp forbids
using their authors' names to endorse the app, and box art downloaded at
runtime is not distributed with the app.
