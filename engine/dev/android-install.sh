#!/usr/bin/env bash
# Build a game for Android and install it on a connected device, with its files beside it.
#
#   dev/android-install.sh <title> [adb serial]        e.g. dev/android-install.sh PCSE00397
#   APK_ONLY=1 dev/android-install.sh <title>          build and install the package, push no files
#
# There is no Gradle here: the package is a manifest, one dex (SDL's Java classes and the activity in
# android/) and three .so files, assembled with the SDK's own aapt2, d8 and apksigner. The game's code
# is libmain.so -- this project's runtime with the recompiled game linked into it, built by
# `TARGET=android vitarecomp build` -- and the game's DATA is pushed to the app's external files
# directory, never into the package. Nothing distributable here carries any of the game.
#
# Needs an Android SDK (ANDROID_HOME, default ~/Android/Sdk) with build-tools and a platform, an NDK,
# and the SDL3 that runtime/platform/android/setup-toolchain.sh builds.
set -euo pipefail
TITLE=${1:?usage: android-install.sh <title> [adb serial]}
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
# One package per game. A single package id for every title meant installing one REPLACED the last --
# Super Stardust Delta went on top of TxK, taking its saves and settings with it. The Java classes stay
# in com.crownparkcomputing.nativ (the manifest names the activity in full); only the application id
# and the launcher label are per game.
PKG=com.crownparkcomputing.nativ.$(echo "$TITLE" | tr '[:upper:]' '[:lower:]')
ADB=(adb); [[ ${2:-} ]] && ADB=(adb -s "$2")
PY=$R/.venv/bin/python; [[ -x $PY ]] || PY=python3

BT=$(ls -1d "$SDK"/build-tools/*/ | sort -V | tail -1)
PLAT=$(ls -1d "$SDK"/platforms/android-*/ | sort -V | tail -1)
AT=${ATIV_ANDROID_TOOLCHAIN:-$R/toolchain-android}
NDK=${ANDROID_NDK:-$(ls -1d "$SDK"/ndk/*/ | sort -V | tail -1)}
[[ -f $AT/SDL3/lib/libSDL3.so ]] || { echo "no Android toolchain at $AT: run runtime/platform/android/setup-toolchain.sh" >&2; exit 1; }
[[ -f $PLAT/android.jar ]] || { echo "no android.jar under $PLAT" >&2; exit 1; }

echo "== building $TITLE for arm64-v8a"
TARGET=android "$PY" "$R/pipeline/vitarecomp.py" build "$TITLE"
eval "$("$PY" "$R/dev/android_launch_info.py" "$TITLE")"
echo "   $GAMENAME: $IMAGE entry $ENTRY, modules: ${MODS:-none}"

# ---- the package: manifest -> aapt2, java -> d8, then the libraries zipped in and the whole thing signed
B=$R/build/android/$TITLE
rm -rf "$B"; mkdir -p "$B/classes" "$B/lib/arm64-v8a"
SDLJAVA=$AT/sdl-src/android-project/app/src/main/java/org/libsdl/app
echo "== compiling the activity"
# no -bootclasspath: a current JDK refuses it alongside -target, and android.jar on the classpath is
# what these sources actually need (d8 desugars the rest against --lib below)
javac -nowarn -source 17 -target 17 -classpath "$PLAT/android.jar" \
      -d "$B/classes" "$SDLJAVA"/*.java "$R"/android/java/com/crownparkcomputing/nativ/*.java
mapfile -t CLASSES < <(find "$B/classes" -name '*.class')
[[ ${#CLASSES[@]} -gt 0 ]] || { echo "no classes compiled" >&2; exit 1; }
"$BT/d8" --release --min-api 28 --lib "$PLAT/android.jar" --output "$B" "${CLASSES[@]}"
[[ -f $B/classes.dex ]] || { echo "d8 produced no classes.dex" >&2; exit 1; }

cp "$AT/SDL3/lib/libSDL3.so" "$B/lib/arm64-v8a/"
cp "$OUT/libmain.so" "$B/lib/arm64-v8a/"
cp "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$B/lib/arm64-v8a/"

# TURNIP_DRIVER=<libvulkan_freedreno.so | a Mesa Turnip .zip>: install that driver for the game to
# use instead of the system Vulkan. The stock Adreno driver on a Retroid Pocket Flip2 reports Vulkan
# 1.1; Turnip reports 1.3. The driver is a third-party binary supplied by you and is not part of this
# project.
#
# It is pushed to drivers/ BESIDE the game's files, not built into the package: the player picks it
# in the menu (F10 -> Display -> Vulkan driver, gfx/vkdriver.c), and a newer Turnip is one `adb push`
# into that folder away rather than a reinstall. The shim and its hooks, which must sit in the app's
# nativeLibraryDir, do go in the package -- always, so the menu has something to select with even
# when no driver was given here.
#
# NOT ENOUGH ON ITS OWN, and measured as such: SDL dlopens that path in the app's own linker
# namespace, which may load only the platform's public libraries, and Turnip needs libhardware.so,
# which is not one of them (/system/etc/public.libraries*.txt lists libnativewindow, libsync, libz,
# liblog, libm, libdl, libc -- all of its other NEEDEDs -- but not libhardware). The load fails and
# SDL_GPU reports "No supported SDL_GPU backend found". Loading it takes libadrenotools'
# linkernsbypass, in a namespace that can see /system/lib64 and /vendor/lib64; since SDL takes a path
# and dlopens it itself, the way to keep SDL stock is a small shim library in the package that does
# that load and re-exports vkGetInstanceProcAddr.
TURNIP_SO=
if [[ -n ${TURNIP_DRIVER:-} ]]; then
  if [[ $TURNIP_DRIVER == *.zip ]]; then
    TD=$B/turnip; mkdir -p "$TD"; unzip -o -q "$TURNIP_DRIVER" -d "$TD"
    TURNIP_SO=$(find "$TD" -name 'libvulkan_*.so' | head -1)
  else TURNIP_SO=$TURNIP_DRIVER; fi
  [[ -f ${TURNIP_SO:-} ]] || { echo "no driver .so in $TURNIP_DRIVER" >&2; exit 1; }
fi
# the shim that opens a driver, with adrenotools' hooks beside it: they must sit in the app's
# nativeLibraryDir, which is this directory once the package is installed. Always shipped, so the
# menu can select a driver pushed later.
if bash "$R/runtime/platform/android/build-turnip.sh" >/dev/null 2>&1; then
  cp "$R"/toolchain-android/turnip/lib/*.so "$B/lib/arm64-v8a/"
else
  echo "   note: the Vulkan driver shim did not build -- the menu will offer the system driver only" >&2
fi

echo "== packaging"
# the game's own icon on the launcher: every Vita title carries one in sce_sys/icon0.png (128x128).
# Without it the package has no resources at all and Android shows its default robot for every game.
ICONSRC=$(readlink -f "$OUT/fs/app0")/sce_sys/icon0.png
RES=() ICONATTR=
if [[ -f $ICONSRC ]]; then
  mkdir -p "$B/res/mipmap-xxhdpi" "$B/res/mipmap-anydpi-v26"
  # the art under TWO names: ic_launcher.png is the icon itself for Android before 26, and
  # ic_launcher_art.png is what the adaptive icon's background layer points at. They must differ:
  # pointing the layer at @mipmap/ic_launcher resolves, on 26 and later, to the adaptive XML itself --
  # a circular reference, so the icon fails to inflate and the launcher falls back to Android's robot.
  cp "$ICONSRC" "$B/res/mipmap-xxhdpi/ic_launcher.png"
  cp "$ICONSRC" "$B/res/mipmap-xxhdpi/ic_launcher_art.png"
  # ...as an ADAPTIVE icon, with the art as the background layer. A legacy icon is wrapped by the
  # launcher in a white rounded backdrop and shrunk inside it; as the background layer the art is
  # scaled to the whole 108dp canvas and the launcher's mask cuts the shape straight out of it, so it
  # fills the icon edge to edge with no white border. The foreground layer is empty.
  cat > "$B/res/mipmap-anydpi-v26/ic_launcher.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@mipmap/ic_launcher_art" />
    <foreground android:drawable="@android:color/transparent" />
</adaptive-icon>
XML
  "$BT/aapt2" compile --dir "$B/res" -o "$B/res.zip" >/dev/null
  RES=("$B/res.zip"); ICONATTR=' android:icon="@mipmap/ic_launcher"'
  echo "   icon: $ICONSRC (adaptive, full bleed)"
else
  echo "   no sce_sys/icon0.png: the launcher will show Android's default"
fi
# the manifest, per game: its own application id, its own name under the launcher and that icon
XNAME=$(printf '%s' "$GAMENAME" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/"/\&quot;/g')
sed -e "s|package=\"com.crownparkcomputing.nativ\"|package=\"$PKG\"|" \
    -e "s|android:label=\"Nativ\"|android:label=\"$XNAME\"|g" \
    -e "s|<application|<application$ICONATTR|" \
    "$R/android/AndroidManifest.xml" > "$B/AndroidManifest.xml"
"$BT/aapt2" link -I "$PLAT/android.jar" --manifest "$B/AndroidManifest.xml" \
    --min-sdk-version 28 --target-sdk-version 35 -o "$B/base.apk" "${RES[@]}" >/dev/null
(cd "$B" && zip -q -r base.apk classes.dex lib)
"$BT/zipalign" -f 4 "$B/base.apk" "$B/aligned.apk"
"$BT/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android \
    --ks-key-alias androiddebugkey --key-pass pass:android --out "$B/nativ.apk" "$B/aligned.apk"
echo "   $(du -h "$B/nativ.apk" | cut -f1) $B/nativ.apk"

echo "== installing"
"${ADB[@]}" install -r "$B/nativ.apk"

FILES=/sdcard/Android/data/$PKG/files
GDIR=$FILES/$TITLE
"${ADB[@]}" shell mkdir -p "$GDIR/fs/savedata0" "$GDIR/shader_cache"
if [[ -z ${APK_ONLY:-} ]]; then
  echo "== pushing the game (this is the slow part over wireless adb)"
  for f in "$IMAGE" $MODS; do "${ADB[@]}" push -q "$OUT/$f" "$GDIR/$f" >/dev/null; done
  # fs/app0 is a link on the desktop; its target is what the device needs
  "${ADB[@]}" push "$(readlink -f "$OUT/fs/app0")" "$GDIR/fs/app0"
fi

# ---- launch.ini: the arguments a desktop passes on the command line (runtime/platform/android/android_main.c)
MODPATHS=$(for f in $MODS; do printf '%s,' "$GDIR/$f"; done); MODPATHS=${MODPATHS%,}
{
  echo "# written by dev/android-install.sh -- the launch runtime/platform/android/android_main.c reads"
  echo "image=$GDIR/$IMAGE"
  echo "entry=$ENTRY"
  echo "budget=100000000000"
  echo "RECOMP_FS=$GDIR/fs"
  [[ -n $MODPATHS ]] && echo "RECOMP_MODULES=$MODPATHS"
  echo "RECOMP_SETTINGS=$GDIR/settings.ini"
  echo "RECOMP_SHADER_CACHE=$GDIR/shader_cache"
  echo "RECOMP_TITLE=$TITLEID"
  echo "RECOMP_GAME_NAME=$GAMENAME"
  echo "RECOMP_HITCH_LOG=50"
  for kv in $RUNENV; do echo "$kv"; done
} > "$B/launch.ini"
"${ADB[@]}" push -q "$B/launch.ini" "$FILES/launch.ini" >/dev/null
# the drivers the menu offers: beside the game's files, so another can be added without a reinstall
if [[ -n ${TURNIP_SO:-} ]]; then
  "${ADB[@]}" shell mkdir -p "$FILES/drivers" >/dev/null
  "${ADB[@]}" push -q "$TURNIP_SO" "$FILES/drivers/$(basename "$TURNIP_SO")" >/dev/null
  echo "   driver: $(basename "$TURNIP_SO") ($(du -h "$TURNIP_SO" | cut -f1)) -> $FILES/drivers; pick it in the menu (Display > Vulkan driver)"
fi
echo "== ready: $GAMENAME on $PKG"
echo "   start it:  adb shell monkey -p $PKG 1"
echo "   watch it:  adb logcat -s nativ"
