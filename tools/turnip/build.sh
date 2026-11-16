#!/usr/bin/env bash
# Builds our Turnip: third_party/mesa (the Mesa commit of the phone's driver) plus
# xdroid-core/turnip/patches/*.patch, into build/turnip/libvulkan_freedreno.so and the adrenotools
# zip build/turnip/xdroid-turnip.zip (bench: gpubench pseudo-key driver=build/turnip/libvulkan_freedreno.so).
# Needs: NDK (env NDK, default the SDK's 29.0.14206865), meson, ninja (the SDK's cmake 3.30.3 has one),
# python with mako, pyyaml and packaging, win_flex_bison in build/tools/winflexbison.
# The patches touch the submodule only while this runs; it is checked out clean again on exit.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
M=$ROOT/third_party/mesa B=$ROOT/build/turnip P=$ROOT/xdroid-core/turnip/patches
SDK=${ANDROID_SDK:-$LOCALAPPDATA/Android/Sdk}
NDK=${NDK:-$SDK/ndk/29.0.14206865}
TC=$(cygpath -m "$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin")
export PATH="$ROOT/build/tools/winflexbison:$(cygpath -u "$SDK")/cmake/3.30.3/bin:$PATH"
[ -z "$(git -C "$M" status --porcelain --untracked-files=no)" ] || { echo "third_party/mesa has local changes"; exit 1; }
trap 'git -C "$M" checkout -q -- .' EXIT
# The submodule checks out with CRLF (core.autocrlf), the patches are LF.
for p in "$P"/*.patch; do git -C "$M" apply --ignore-whitespace "$p"; done
# Windows: glob gives backslash paths, so the lint allowlist in check_xml_includes.awk never matches.
sed -i "s|sorted(files)|sorted(f.replace(os.sep, \"/\") for f in files)|" "$M/src/freedreno/tests/check_xml_includes.py"
# Windows without core.symlinks writes a symlink as a text file holding its target (include/android_stub/sync/sync.h).
git -C "$M" ls-files -s | awk '$1 == 120000 {print $4}' | while read -r l; do
  t="$M/$(dirname "$l")/$(cat "$M/$l")"; [ ! -f "$t" ] || cp "$t" "$M/$l"; done
mkdir -p "$B/shim"
# Mesa picks win_flex --wincompat on a Windows host: its lexers include <io.h>, which Android lacks.
printf '#include <unistd.h>\n#define _isatty isatty\n#define _fileno fileno\n' >"$B/shim/io.h"
S=$(cygpath -m "$B/shim")
cat >"$B/cross.ini" <<EOF
[binaries]
c = ['$TC/clang.exe', '--target=aarch64-linux-android29']
cpp = ['$TC/clang++.exe', '--target=aarch64-linux-android29']
ar = '$TC/llvm-ar.exe'
strip = '$TC/llvm-strip.exe'
nm = '$TC/llvm-nm.exe'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[built-in options]
c_args = ['-march=armv8-a', '-moutline-atomics', '-I$S']
cpp_args = ['-march=armv8-a', '-moutline-atomics', '-I$S']
c_link_args = ['-static-libstdc++']
cpp_link_args = ['-static-libstdc++']
EOF
if [ ! -f "$B/out/build.ninja" ]; then
  rm -rf "$B/out"
  meson setup "$B/out" "$M" --cross-file "$B/cross.ini" -Dbuildtype=release -Db_ndebug=true \
    -Dplatforms=android -Dplatform-sdk-version=29 -Dandroid-stub=true -Dandroid-strict=false \
    -Dandroid-libbacktrace=disabled -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dgallium-drivers= \
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
    -Dllvm=disabled -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dperfetto=false \
    -Dxmlconfig=disabled -Dexpat=disabled
fi
ninja -C "$B/out" src/freedreno/vulkan/libvulkan_freedreno.so
"$TC/llvm-strip.exe" -o "$B/libvulkan_freedreno.so" "$B/out/src/freedreno/vulkan/libvulkan_freedreno.so"
ver="$(cat "$M/VERSION")-xdroid-$(git -C "$M" rev-parse --short=11 HEAD)-$(cat "$P"/*.patch | md5sum | cut -c1-8)"
python - "$(cygpath -m "$B")" "$ver" <<'EOF'
import json, pathlib, sys, zipfile
b = pathlib.Path(sys.argv[1])
meta = {"schemaVersion": 1, "name": "XDroid360 Turnip", "description": "Mesa Turnip plus XDroid360 patches",
        "author": "XDroid360", "packageVersion": "1", "vendor": "Mesa", "driverVersion": sys.argv[2],
        "minApi": 29, "libraryName": "libvulkan_freedreno.so"}
with zipfile.ZipFile(b / "xdroid-turnip.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("meta.json", json.dumps(meta, indent=2))
    z.write(b / "libvulkan_freedreno.so", "libvulkan_freedreno.so")
EOF
echo "built $B/xdroid-turnip.zip ($ver, $(stat -c %s "$B/libvulkan_freedreno.so") bytes)"
