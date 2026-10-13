# Contributing to XDroid360

XDroid360 is an Android port built on [Xenia Edge](https://github.com/has207/xenia-edge). Fixes that
are not Android-specific usually belong upstream first; we pull them back in. Changes that started
here and may help upstream are listed in [docs/UPSTREAM_IMPROVEMENTS.md](../docs/UPSTREAM_IMPROVEMENTS.md).

## Information sourcing

All information in Xenia and XDroid360 comes from reverse engineering legally owned games and
hardware, tools Microsoft made public (such as XNA Game Studio), public documentation and
presentations, and code made public by third parties.

The official Microsoft Xbox Development Kits (XDKs) must not be used for any information added to
the project, leaked or not. The contributors do not want the XDKs or anything derived from them.
**Contributions that contain XDK-derived information will be rejected and their author banned.**

Never add, link to or request games, disc images, keys, firmware, dashboards or other copyrighted
material, in code, issues, logs or screenshots.

## Code layout

- `src/`, `third_party/`, `tools/`: the upstream Xenia Edge tree. Keep changes small and guarded
  (`XE_PLATFORM_ANDROID`, `NOT ANDROID`) so upstream syncs stay easy.
- `xdroid-core/`: native Android glue (JNI, audio, input, content, driver loading).
- `xdroid-app/`: the Kotlin/Jetpack Compose app.
- Per-title workarounds live in `xdroid-core/` behind a setting, never as unconditional title-ID
  checks inside the emulator core, and must explain which guest behavior they work around.

## Pull requests

- Build instructions: [docs/building-android.md](../docs/building-android.md).
- Run `xb format` (clang-format) on C++ changes; see [docs/style_guide.md](../docs/style_guide.md).
- Keep commits atomic: each one compiles and runs on its own, and its message says why.
- Performance claims need numbers: device, driver, title and scene, and before/after measured the
  same way over several runs.
- Code ported from another project keeps its copyright and license header, gets a line in
  `docs/licenses/notices.tsv`, and the commit message names the source repository and commit.
  GPL code is not accepted; LGPL code needs prior discussion.

## Reporting bugs

Use the bug report template. Include the build hash, device, Android version, GPU driver (system or
Turnip version), the game's title ID, and the log. Check existing issues first.

## License

XDroid360 is licensed under the BSD 3-Clause license ([LICENSE](../LICENSE)). By contributing you
agree that your contribution is licensed under it. Code under `third_party/` keeps its original
license.
