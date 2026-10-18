#!/usr/bin/env bash
# Installs the XDroid360 Android toolchain on a Linux cloud session (Claude Code
# cloud / Projects threads), matching the pins in .claude/skills/xdroid-build:
# JDK 21, SDK platform 35, NDK 29.0.14206865, CMake 3.30.3. The Gradle wrapper
# (8.11.1) downloads itself.
#
# UNTESTED on a cloud image as of 2026-10-13: the supported build host is the
# user's Windows PC. Report what fails here into the xdroid-build failure catalog.
#
# Usage: bash tools/cloud/setup-android-linux.sh   (then ./gradlew :xdroid-app:assembleDebug (the build phone.sh deploys))
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${ANDROID_SDK_ROOT:-$HOME/android-sdk}
NDK_VERSION=29.0.14206865
CMAKE_VERSION=3.30.3
CMDLINE_TOOLS_ZIP=commandlinetools-linux-11076708_latest.zip

if ! java -version 2>&1 | grep -q 'version "21'; then
  echo "== installing JDK 21"
  sudo apt-get update -qq && sudo apt-get install -y -qq openjdk-21-jdk-headless
fi

if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
  echo "== installing Android command-line tools into $SDK"
  mkdir -p "$SDK/cmdline-tools"
  tmp=$(mktemp -d)
  curl -fsSL "https://dl.google.com/android/repository/$CMDLINE_TOOLS_ZIP" -o "$tmp/tools.zip"
  unzip -q "$tmp/tools.zip" -d "$tmp"
  mv "$tmp/cmdline-tools" "$SDK/cmdline-tools/latest"
  rm -rf "$tmp"
fi

SDKMANAGER="$SDK/cmdline-tools/latest/bin/sdkmanager"
yes | "$SDKMANAGER" --sdk_root="$SDK" --licenses >/dev/null
"$SDKMANAGER" --sdk_root="$SDK" \
  "platforms;android-35" "build-tools;35.0.0" "platform-tools" \
  "ndk;$NDK_VERSION" "cmake;$CMAKE_VERSION"

# local.properties is gitignored and machine-specific.
echo "sdk.dir=$SDK" >"$ROOT/local.properties"

echo "== submodules"
git -C "$ROOT" submodule update --init --recursive --jobs 8

echo "== done: export ANDROID_SDK_ROOT=$SDK, then ./gradlew :xdroid-app:assembleDebug (the build phone.sh deploys)"
