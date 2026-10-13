<h1 align="center">XDroid360</h1>

<p align="center">An unofficial, experimental Android port of Xbox 360 emulation, built on the Xenia emulator.</p>

XDroid360 runs Xbox 360 games on Android phones with Vulkan (Snapdragon/Adreno with the Turnip
driver is the main target). It is a fork of [Xenia Edge](https://github.com/has207/xenia-edge), itself
based on [Xenia Canary](https://github.com/xenia-canary/xenia-canary) and the original
[Xenia](https://github.com/xenia-project/xenia) project. Android-specific code lives in `xdroid-core/`
(native) and `xdroid-app/` (Kotlin app); the upstream tree stays close to Xenia Edge.

## Status

Experimental. Development targets one device and one title first (OnePlus 12 / Snapdragon 8 Gen 3,
Ace Combat 6), aiming for a stable, glitch-free, cool-running emulator before widening compatibility.
Expect bugs, crashes and missing features.

## Legal

- XDroid360 is not affiliated with, endorsed by or sponsored by Microsoft, Xbox, or any game publisher.
  "Xbox" and "Xbox 360" are trademarks of Microsoft Corporation; game names are trademarks of their
  respective owners and are used only to describe compatibility.
- XDroid360 contains no Microsoft code, firmware, keys or SDK material, and no copyrighted game content.
- Piracy is not supported. Use only games you own and dumped yourself from your own discs or console.
  Do not ask for, link to or share game files, keys or firmware in this repository.

## License

- Xenia and XDroid360 code: BSD 3-Clause, see [LICENSE](LICENSE).
- Third-party components keep their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
  The app shows them under "Open-source licenses" (generated from `docs/licenses/notices.tsv`).
- FFmpeg and libmspack (LGPL 2.1) are statically linked. Their source, and the XDroid360 source needed
  to relink the app against modified versions, are available from this repository at the commit each
  release was built from.
- Portions of `xdroid-core/` are derived from [AC6_recomp](https://github.com/sal063/AC6_recomp)
  (BSD 3-Clause); each such file carries its notice.

## Getting started

- **Games:** you need your own legally dumped games (`.iso`, `.xex`, `.zar`). None are provided.
- **GPU driver:** Adreno phones run best with a Turnip (Mesa) Vulkan driver package. XDroid360 does not
  bundle one; install a driver `.zip` from the app's Driver tab, or keep the system driver.
- **Box art:** the game list fetches cover images from Microsoft's public marketplace image host by
  title ID. No game content is shipped with the app.

## Building

See [docs/building-android.md](docs/building-android.md) (Gradle + NDK 29 + CMake 3.30, Windows host
today). Desktop builds of the upstream tree are described in [docs/building.md](docs/building.md).

## Contributing and security

- [Contributing guide](.github/CONTRIBUTING.md): information sourcing rules (no XDK material), code
  layout, and how to report bugs.
- [Security policy](SECURITY.md): report vulnerabilities privately.
- Changes that are not Android-specific and may be worth upstreaming are tracked in
  [docs/UPSTREAM_IMPROVEMENTS.md](docs/UPSTREAM_IMPROVEMENTS.md).

## Credits

The Xenia team and contributors, the Xenia Canary and Xenia Edge maintainers, Mesa/Turnip developers,
and every project listed in the third-party notices.
