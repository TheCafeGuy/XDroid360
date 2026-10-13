# Third-party notices

XDroid360 is a fork of [Xenia Edge](https://github.com/has207/xenia-edge), which builds on
[Xenia Canary](https://github.com/xenia-canary/xenia-canary) and [Xenia](https://github.com/xenia-project/xenia).
Xenia and XDroid360 code is under the BSD 3-Clause license in [LICENSE](LICENSE)
(Copyright (c) 2015, Ben Vanik, and the contributors). XDroid360 is not affiliated with or endorsed by
the Xenia, Xenia Canary or Xenia Edge projects or their authors.

## What ships in the Android app

[`docs/licenses/notices.tsv`](docs/licenses/notices.tsv) is the authoritative list of third-party
code and assets compiled or packaged into the APK, with the license file for each. The build turns it
into `assets/licenses/NOTICES.txt`, shown in the app under About > Open-source licenses, and fails if a
listed license file is missing. License texts that upstream keeps only in source headers are copied
into [`docs/licenses/`](docs/licenses). See [`docs/licenses/README.md`](docs/licenses/README.md) for
how the list is kept current.

Highlights:

- **Xenia / Xenia Edge:** BSD-3-Clause.
- **AC6_recomp** ([sal063/AC6_recomp](https://github.com/sal063/AC6_recomp)): BSD-3-Clause, Copyright
  (c) 2026 rexglue. Files derived from it carry its notice; the full text is in
  [`docs/licenses/AC6_recomp-LICENSE.txt`](docs/licenses/AC6_recomp-LICENSE.txt).
- **libadrenotools / linkernsbypass** (custom GPU driver loading): BSD-2-Clause.
- **FFmpeg** (libavcodec, libavformat, libavutil; LGPL-2.1-or-later) and **libmspack** (LGPL-2.1) are
  linked statically, as in upstream Xenia. FFmpeg is built without its GPL parts. The source of both,
  and the complete XDroid360 source needed to rebuild and relink the app against modified versions,
  are available in this repository at the commit each release was built from
  (`third_party/FFmpeg`, pinned to [has207/FFmpeg](https://github.com/has207/FFmpeg), and
  `third_party/mspack`).
- **Inter typeface:** SIL Open Font License 1.1 ([`assets/font/OFL.txt`](assets/font/OFL.txt)).
- **Xenia icon images** in `assets/icon`: CC BY-SA 4.0 ([`assets/icon/LICENSE`](assets/icon/LICENSE)).
  The XDroid360 launcher icon is a separate drawable in `xdroid-app/`.

## Not shipped

- **GPU drivers:** XDroid360 does not bundle Mesa/Turnip or any other driver. Users install a driver
  package themselves. The `third_party/mesa` submodule (MIT and others, see its `docs/license.rst`)
  is only built for upstream's D3D12 and macOS shader paths, not for Android.
- **Other submodules** under `third_party/` (for example wxWidgets, SDL3, MoltenVK, metal-cpp,
  DirectXShaderCompiler, DirectX-Headers, libusb, Catch2, cpplint) belong to upstream's desktop builds
  or tooling. Each keeps its own license file in its directory.
- **Games, firmware, keys:** none are included. Users supply their own legally obtained games.

## Trademarks

"Xbox" and "Xbox 360" are trademarks of Microsoft Corporation. Game names are trademarks of their
respective owners and are used only to describe compatibility.
