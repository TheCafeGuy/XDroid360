# Building XDroid360 for Android

The Android app is built with Gradle. Gradle runs CMake through the NDK, and
that CMake build pulls in the upstream Xenia tree (`xdroid-core/src/main/cpp/CMakeLists.txt`).
For desktop builds of the upstream tree, see [building.md](building.md).

## Toolchain

| Tool | Version |
| --- | --- |
| JDK | 21 |
| Android SDK platform / build-tools | 35 / 35.0.0 |
| Android NDK | 29.0.14206865 |
| CMake (SDK package) | 3.30.3 |
| Gradle | 8.11.1 (the wrapper downloads it) |
| Python | 3.10 or newer |

The supported build host today is **Windows x64**. The Gradle and CMake glue
still expects Windows paths for the NDK's LLVM tools and for the host shader
compiler (`xdroid-core/build.gradle.kts`, `third_party/CMakeLists.txt`), so a
Linux or macOS host needs small changes there first; contributions welcome.

The shader build needs a host (x64) build of `xenia-shader-cc`, the upstream
shader compiler, because the ARM64 binary the NDK produces cannot run on the
build machine. The Android configure looks for
`build/host_tools/<Release|Checked|Debug>/xenia-shader-cc.exe`.

## Steps

1. Clone with submodules:

   ```sh
   git clone --recursive https://github.com/TheCafeGuy/XDroid360.git
   cd XDroid360
   # or, in an existing clone:
   git submodule update --init --recursive
   ```

2. Install, with Android Studio's SDK Manager or `sdkmanager`:
   `platforms;android-35`, `build-tools;35.0.0`, `platform-tools`,
   `ndk;29.0.14206865` and `cmake;3.30.3`. Point `local.properties` at the
   SDK (`sdk.dir=...`) and set `ANDROID_NDK_HOME` to the NDK folder.

3. Build the host shader compiler once, as a native desktop build of the
   upstream tree (see [building.md](building.md)), for example
   `xb build --target xenia-shader-cc --config=release`, or with MinGW-w64
   and Ninja:

   ```sh
   cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DXENIA_ENABLE_LTO=OFF
   cmake --build build-host --target xenia-shader-cc
   ```

   Then copy `xenia-shader-cc.exe` and its DLLs into
   `build/host_tools/Release/` (and `Checked/`, `Debug/` for those configs).

4. Build the APK:

   ```sh
   gradlew.bat :xdroid-app:assembleDebug
   ```

   The first build takes several minutes (ThinLTO); incremental builds are much
   faster. The APK is written to `xdroid-app/build/outputs/apk/`.

## Notes

- Compiler flags target ARMv8.2-A and tune for Cortex-X4. Do not use
  `-mcpu=cortex-x3`: it enables SVE, which current Snapdragon phones do not
  run (SIGILL).
- `xdroid-core/src/main/cpp/build_id.cmake` stamps the git commit into the
  library; the app logs it at boot as `XDroid360: build <hash>`.
- The build fails if a file listed in `docs/licenses/notices.tsv` is missing:
  that list generates the in-app open-source notices.
- Please report build failures as issues, with the error lines.

## Common build failures

- `generator Ninja does not match NMake` in `third_party/snappy` or
  `third_party/zlib-ng`: delete the stale in-source `CMakeCache.txt` and
  `CMakeFiles` there.
- `xenia-shader-cc` fails with an exec format error: the host tool is missing
  or is an ARM build; rebuild it (step 3).
- `duplicate symbol` between `*_posix` and `*_android` files: guard the POSIX
  copy with `#if !XE_PLATFORM_ANDROID`.
