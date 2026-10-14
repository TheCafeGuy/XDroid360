<h1 align="center">XDroid360</h1>

<p align="center">An unofficial, experimental Xbox 360 emulator for Android, built on Xenia.</p>

<p align="center">
  <a href="https://github.com/TheCafeGuy/XDroid360/releases/latest">Download the latest release</a> ·
  <a href="#features">Features</a> ·
  <a href="#requirements">Requirements</a> ·
  <a href="#building">Building</a>
</p>

XDroid360 runs Xbox 360 games on Android phones over Vulkan. Snapdragon phones with an Adreno GPU are
the main target, using either the system driver or a Turnip (Mesa) driver. It is a fork of
[Xenia Edge](https://github.com/has207/xenia-edge), which is based on
[Xenia Canary](https://github.com/xenia-canary/xenia-canary) and the original
[Xenia](https://github.com/xenia-project/xenia). Android-specific code lives in `xdroid-core/`
(native) and `xdroid-app/` (Kotlin app). The rest of the tree stays close to Xenia Edge.

## Status

**Experimental.** The project started because Ace Combat 6 would not run well in any Android
emulator. The first goal is one game on one phone, played start to finish at a stable 30 FPS, with no
glitches, audio dropouts or crashes even when the phone is hot. More games and phones come after that.

| | |
|---|---|
| Target game | Ace Combat 6 (title ID 4E4D07D1). Other games are untested; reports are welcome. |
| Tested phone | OnePlus 12 (Snapdragon 8 Gen 3, Adreno 750) with a Turnip driver |
| Current result | Missions hold the game's 30 FPS cap |

Expect bugs, crashes and missing features. See [Known issues](#known-issues).

## Features

### Playing
- **On-screen controls**:
  - two analog sticks, D-pad, A/B/X/Y, bumpers, triggers, Back and Start, plus a menu button that
    opens the quick menu;
  - multi-touch, and each stick is centered where your finger lands;
  - vibration on press, and double tap a stick to click it (L3/R3).
- **Touch layout editor**: drag any control, resize it, hide it, set the opacity, or reset to the
  default layout. Your layout is saved.
- **Physical controllers** map to the Xbox 360 layout automatically. On-screen controls hide while a
  controller is connected and come back when it disconnects.
- **Controller remapping**: assign any controller button to any Xbox 360 input, or go back to
  automatic.
- **Quick menu**:
  - opens with Select+Start, the controller's Back button, the on-screen menu button, or by holding
    the ⚑ flag;
  - shows live performance stats, and holds the controls settings and game exit;
  - the PS/Home button toggles fast forward.

### Your games and data
- **Game library** with search, recent games, a per-folder or whole-folder scan, and optional box art
  downloaded by title ID.
- **DLC and title updates** install from the app.
- **Backup and restore**: export game saves, emulator settings and controls to a `.zip`, and import
  them on the same phone or another one. On import, pick **Merge** to keep your other saves, or
  **Replace** to clear those games' saves first.
- **Global and per-game settings.**

### Graphics and performance
- **Custom GPU drivers**: install a Turnip (Mesa) driver `.zip` from the Driver tab, or keep the
  system driver.
- **Game-specific fixes**, starting with Ace Combat 6. For example, a fix for one-frame HUD garbage:
  missing glyphs, a broken radar, and text drawn as blocks.
- **Adapts the screen refresh rate to the game**, for example 30 Hz content instead of a 120 Hz panel,
  to save power.
- **Tells Android how much CPU the game needs** (ADPF performance hints), so clocks match the real
  demand instead of running at full speed.
- **AMOLED tone mapping** option.
- **Device check**: phones whose CPU cannot run the native code get a clear message instead of a crash.

### Reliability
- **Session reports**: if a session ends in a crash, the app finds it on the next start, which makes
  bug reports easier.
- **In-game glitch report**: tap the ⚑ flag to mark the moment a glitch happens.

## Planned

On the way, roughly in this order:

- touch controls that feel and look closer to a real controller: dead zones, a floating stick,
  analog triggers, stronger vibration, 360-style buttons, presets per screen size, per-game layouts,
  and an optional fast-forward button;
- a redesigned quick menu with tabs, usable with a controller or touch;
- controller and touch settings available from the start screen;
- a per-game auto-tuner, dynamic 60/40/30 FPS, and better behavior when the phone is hot.

## Requirements

- Android 10 (API 29) or newer, arm64.
- A Snapdragon phone with an Adreno GPU and an ARMv8.2 or newer CPU. Older CPUs (Snapdragon 835, 660,
  665, 680 class) are not supported.
- Vulkan. Adreno 7xx with a Turnip driver is the tested path.
- Your own legally dumped games (`.iso`, `.xex`, `.zar`). None are provided.

## Install

1. Download the `.apk` from [Releases](https://github.com/TheCafeGuy/XDroid360/releases).
2. Install it. New versions update the old one in place and keep your settings.
   The package id is `io.github.thecafeguy.xdroid360`.
3. Optional: check the file's SHA-256 and the signing certificate listed in the release notes.

## Known issues

- Audio can crackle when the phone is hot.
- The hangar in Ace Combat 6 can crash when you scroll the aircraft list very fast.
- Some textures can appear late or be missing.

To report a problem, open an issue with your phone model, Android version, the game and what you saw.
A screenshot or a short video helps.

## Legal

- XDroid360 is not affiliated with, endorsed by or sponsored by Microsoft, Xbox, or any game publisher.
  "Xbox" and "Xbox 360" are trademarks of Microsoft Corporation. Game names are trademarks of their
  respective owners and are used only to describe compatibility.
- XDroid360 contains no Microsoft code, firmware, keys or SDK material, and no copyrighted game content.
- Piracy is not supported. Use only games you own and dumped yourself from your own discs or console.
  Do not ask for, link to or share game files, keys or firmware in this repository.
- **Box art** is fetched from Microsoft's public marketplace image host by title ID, only when you
  choose to download it.

## License

- Xenia and XDroid360 code: BSD 3-Clause, see [LICENSE](LICENSE).
- Third-party components keep their own licenses. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md);
  the app also shows them under About › Open-source licenses.
- FFmpeg and libmspack (LGPL 2.1) are statically linked. Their source, and the XDroid360 source needed
  to relink the app against modified versions, are available from this repository at the commit each
  release was built from.
- Portions of `xdroid-core/` are derived from [AC6_recomp](https://github.com/sal063/AC6_recomp)
  (BSD 3-Clause). Each such file carries its notice.

## Building

See [docs/building-android.md](docs/building-android.md): Gradle, NDK 29 and CMake 3.30, Windows host
for now. Desktop builds of the upstream tree are described in [docs/building.md](docs/building.md).

## Contributing and security

- [Contributing guide](.github/CONTRIBUTING.md): information sourcing rules (no XDK material), code
  layout, and how to report bugs.
- [Security policy](SECURITY.md): report vulnerabilities privately.

## Credits

XDroid360 exists thanks to the work of others:

- the [Xenia](https://github.com/xenia-project/xenia) team and contributors;
- the [Xenia Canary](https://github.com/xenia-canary/xenia-canary) and
  [Xenia Edge](https://github.com/has207/xenia-edge) maintainers;
- [AC6_recomp](https://github.com/sal063/AC6_recomp) (rexglue);
- the Mesa / Turnip developers, and [libadrenotools](https://github.com/bylaws/libadrenotools) for
  custom driver loading;
- every project listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Made by TheCafeGuy.
