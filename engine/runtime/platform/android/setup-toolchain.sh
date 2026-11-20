#!/usr/bin/env bash
# The Android toolchain: SDL3 built for arm64-v8a with the NDK, into <root>/toolchain-android, which
# runtime/build.py's TARGET=android and dev/android-install.sh both read. The same SDL release the
# other platforms pin, so a game behaves the same way on all of them.
#
#   runtime/platform/android/setup-toolchain.sh     on a Linux x86-64 machine with an Android SDK+NDK
#
# ANDROID_HOME (default ~/Android/Sdk) and ANDROID_NDK (default the newest NDK under it) choose the
# toolchain; API 28 is the floor, the oldest Android with the Vulkan SDL_GPU needs here.
set -euo pipefail
SDL=3.4.16
SDL_SHA=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
API=28

R=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
D=${ATIV_ANDROID_TOOLCHAIN:-$R/toolchain-android}
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
NDK=${ANDROID_NDK:-$(ls -1d "$SDK"/ndk/*/ 2>/dev/null | sort -V | tail -1)}
[[ -f ${NDK:-}/build/cmake/android.toolchain.cmake ]] || { echo "no Android NDK (set ANDROID_NDK, or install one under $SDK/ndk)" >&2; exit 1; }
mkdir -p "$D/dl"; cd "$D"

# a download's sha256 against its pin above, so a tampered or changed archive stops the script
verify() { local h; h=$(sha256sum "$1" | awk '{print $1}'); [[ $h == "$2" ]] && return; echo "checksum mismatch: $1 (expected $2, got $h)" >&2; rm -f "$1"; exit 1; }
fetch() { [[ -f $2 ]] || curl -fL --retry 3 -o "$2" "$1"; verify "$2" "$3"; }
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz" "dl/SDL3-$SDL.tar.gz" "$SDL_SHA"

# the source tree is kept: dev/android-install.sh compiles SDL's own Java classes out of
# android-project/, which are half of the package and must match the libSDL3.so built here
rm -rf sdl-src; mkdir sdl-src
tar xzf "dl/SDL3-$SDL.tar.gz" -C sdl-src --strip-components 1

if [[ ! -f SDL3/lib/libSDL3.so ]]; then
  rm -rf sdl-build
  cmake -S sdl-src -B sdl-build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$D/SDL3" -DCMAKE_INSTALL_LIBDIR=lib \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF \
    > sdl-configure.log
  cmake --build sdl-build -j"$(nproc)" > sdl-build.log 2>&1
  cmake --install sdl-build >> sdl-build.log 2>&1
fi
cp -f sdl-src/LICENSE.txt SDL3/LICENSE.txt 2>/dev/null || true
echo "Android toolchain ready: $D"
echo "  SDL3  $(file -b SDL3/lib/libSDL3.so | cut -d, -f1-2)"
echo "  build a game: TARGET=android ./vitarecomp build <title>"
echo "  install it:   dev/android-install.sh <title>"
