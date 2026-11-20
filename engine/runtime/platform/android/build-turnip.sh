#!/usr/bin/env bash
# Build the pieces that let a replacement Vulkan driver (Mesa's Turnip) be used on Android:
# libadrenotools and its hooks, and libvkshim.so, the library SDL opens instead of libvulkan.so
# (runtime/platform/android/vkshim.c says why a shim is needed at all).
#
#   runtime/platform/android/build-turnip.sh [out dir]      default: toolchain-android/turnip
#
# Everything lands in <out>/lib: libvkshim.so, libadrenotools.so and the three hook libraries, all of
# which dev/android-install.sh copies into the package. libadrenotools is third_party/libadrenotools
# (BSD-2-Clause, Billy Laws); the driver binary itself is supplied by you and is not built here.
set -euo pipefail
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=${1:-$R/toolchain-android/turnip}
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
NDK=${ANDROID_NDK:-$(ls -1d "$SDK"/ndk/*/ | sort -V | tail -1)}
NDK=${NDK%/}
API=${ANDROID_API:-28}
[[ -f $NDK/build/cmake/android.toolchain.cmake ]] || { echo "no NDK toolchain file under $NDK" >&2; exit 1; }
[[ -f $R/third_party/libadrenotools/CMakeLists.txt ]] || { echo "no third_party/libadrenotools" >&2; exit 1; }

mkdir -p "$OUT/lib"
echo "== libadrenotools (arm64-v8a, API $API)"
cmake -S "$R/third_party/libadrenotools" -B "$OUT/build" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$OUT/build" >/dev/null
# the hooks are loaded by name out of the package, so they travel with it; adrenotools itself and
# linkernsbypass are static archives, linked into the shim below rather than shipped
find "$OUT/build" -name 'lib*.so' -exec cp {} "$OUT/lib/" \;
find "$OUT/build" -name 'lib*.a' -exec cp {} "$OUT/" \;

echo "== libvkshim.so"
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang
[[ -x $CC ]] || { echo "no $CC" >&2; exit 1; }
# no -L of our own: adding one ahead of the NDK's puts the sysroot's unversioned directory first and
# the link then finds no libc at all -- the shim built, declared only libc++_shared and liblog, and
# crashed in its own constructor the moment the linker opened it. --no-undefined makes that a build
# error instead of a crash on the device.
"$CC" -O2 -fPIC -shared -Wall -Wextra -Wl,--no-undefined \
      -I"$R/third_party/libadrenotools/include" \
      -o "$OUT/lib/libvkshim.so" "$R/runtime/platform/android/vkshim.c" \
      "$OUT/libadrenotools.a" "$OUT/liblinkernsbypass.a" \
      -lc++_shared -llog -ldl
echo "   $(ls "$OUT/lib" | tr '\n' ' ')"
