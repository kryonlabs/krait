#!/bin/sh
# Build the native arm64-v8a library for Krait on Android: libraylib.a,
# libkryon.a, the Krait objects, and the final libkrait.so that NativeActivity
# loads. Produces build/android-arm64/libkrait.so.
#
# Environment:
#   ANDROID_NDK_HOME / ANDROID_NDK  - NDK root (auto-detected if unset)
#   ANDROID_API                     - API level (default 29)
#
# The kryon symbol-rename layer is reused: raylib is built with the generated
# raylib_backend_rename.h (-include), kryon + krait are built without it and go
# through kryon_raylib_wrappers.c. Audio is disabled in raylib to avoid the
# PlaySound/Win32 rename edge case (the IDE does not use audio).
set -eu

KCWD=$(cd "$(dirname "$0")/.." && pwd)
cd "$KCWD"

# --- resolve NDK -----------------------------------------------------------
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}
if [ -z "$NDK" ]; then
    SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
    NDK=$(ls -d "$SDK"/ndk/* 2>/dev/null | head -1 || true)
fi
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo "error: Android NDK not found. Set ANDROID_NDK_HOME or install via sdkmanager 'ndk;28.2.13676358'." >&2
    exit 1
fi
API=${ANDROID_API:-29}
PRE=$NDK/toolchains/llvm/prebuilt/linux-x86_64
CC=$PRE/bin/aarch64-linux-android$API-clang
AR=$PRE/bin/llvm-ar
GLUE=$NDK/sources/android/native_app_glue

# --- paths ----------------------------------------------------------------
KRYON=vendor/kryon
GEN=$(pwd)/$KRYON/build/linux-x86_64/generated
RENAME=$GEN/raylib_backend_rename.h
WRAPPERS=$GEN/kryon_raylib_wrappers.c
if [ ! -f "$RENAME" ] || [ ! -f "$WRAPPERS" ]; then
    echo "error: kryon generated headers missing ($RENAME). Run 'make' first to build kryon." >&2
    exit 1
fi

OUT=build/android-arm64
RAYLIB_OUT=$OUT/raylib
KRYON_OUT=$OUT/kryon
KRAIT_OUT=$OUT/krait
mkdir -p "$RAYLIB_OUT" "$KRYON_OUT/obj" "$KRAIT_OUT/obj"

DEFS="-DPLATFORM_ANDROID -DGRAPHICS_API_OPENGL_ES2 -DSUPPORT_FILEFORMAT_TTF=1 -DSUPPORT_FILEFORMAT_JPG=1 -DSUPPORT_FILEFORMAT_OGG=1 -DSUPPORT_FILEFORMAT_MP3=1"

# --- 1. raylib (arm64, with kryon symbol rename, no audio) ----------------
echo "==> raylib (arm64)"
RBD=$OUT/raylib-src
rm -rf "$RBD" && mkdir -p "$RBD"
cp -R "$KRYON"/vendor/raylib/src/. "$RBD"/
make -j4 -C "$RBD" \
    PLATFORM=PLATFORM_ANDROID ANDROID_NDK="$NDK" \
    ANDROID_ARCH=arm64 ANDROID_API_VERSION=$API \
    RAYLIB_LIBTYPE=STATIC GRAPHICS=GRAPHICS_API_OPENGL_ES2 \
    RAYLIB_RELEASE_PATH=../raylib \
    RAYLIB_MODULE_AUDIO=FALSE RAYLIB_MODULE_MODELS=TRUE \
    CUSTOM_CFLAGS="-include $RENAME -Os -ffunction-sections -fdata-sections"

# --- 2. kryon (arm64, NO rename; uses wrappers) ---------------------------
echo "==> kryon (arm64)"
KINC="-I$KRYON/include -I$KRYON/src/ui -I$KRYON/vendor/clay -I$GEN -I$GLUE"
objs=""
for f in $(find "$KRYON/src" -type f -name '*.c' | LC_ALL=C sort | grep -v 'ksync_account.c'); do
    o=$KRYON_OUT/obj/$(echo "$f" | tr '/' '_').o
    $CC -c -std=c99 -O1 -fPIC -w $DEFS $KINC "$f" -o "$o"
    objs="$objs $o"
done
$CC -c -std=c99 -O1 -fPIC -w $DEFS $KINC "$WRAPPERS" -o $KRYON_OUT/obj/wrappers.o
objs="$objs $KRYON_OUT/obj/wrappers.o"
# Embedded assets (themes + the regular UI font), baked into libkryon.a so the
# app has its font/theme with no filesystem. Keys are paths relative to kryon
# root, matching what EnsureUIDefaultFont / RegisterUIFontFileSource look up.
EMBED_C=$KRYON_OUT/obj/embedded_asset_data.c
( cd "$KRYON" && sh scripts/embed-assets.sh "$KCWD/$EMBED_C" themes fonts/noto/NotoSans-Regular.ttf )
$CC -c -std=c99 -O1 -fPIC -w $DEFS $KINC "$EMBED_C" -o $KRYON_OUT/obj/embedded_asset_data.o
objs="$objs $KRYON_OUT/obj/embedded_asset_data.o"
$AR rcs $KRYON_OUT/libkryon.a $objs

# --- 3. krait sources (arm64) ---------------------------------------------
echo "==> krait (arm64)"
KRAIT_INC="-I$KRYON/include -I$KRYON/src/ui -I$KRYON/vendor/clay -Ibuild/gen -Isrc"
kobjs=""
for f in src/*.c; do
    o=$KRAIT_OUT/obj/$(basename "$f" .c).o
    $CC -c -std=c99 -O1 -fPIC -w $DEFS $KRAIT_INC "$f" -o "$o"
    kobjs="$kobjs $o"
done
for f in build/gen/ide/*.c build/gen/modules/*/*.c; do
    [ -f "$f" ] || continue
    o=$KRAIT_OUT/obj/gen_$(echo "$f" | tr '/' '_').o
    extra=""
    [ "$(basename "$f")" = "app.c" ] && extra="-Dmain=krait_generated_main"
    $CC -c -std=c99 -O1 -fPIC -w $DEFS $KRAIT_INC $extra "$f" -o "$o"
    kobjs="$kobjs $o"
done
echo "$kobjs" > $KRAIT_OUT/obj/list.txt

# --- 4. link libkrait.so ---------------------------------------------------
echo "==> linking libkrait.so"
$CC -shared -fPIC -Wl,-soname,libkrait.so -o $OUT/libkrait.so \
    $kobjs \
    -Wl,--whole-archive $KRYON_OUT/libkryon.a -Wl,--no-whole-archive \
    -Wl,-u,ANativeActivity_onCreate $RAYLIB_OUT/libraylib.a \
    -landroid -lEGL -lGLESv2 -llog -lOpenSLES -ldl -lm

echo "done: $OUT/libkrait.so ($(ls -la $OUT/libkrait.so | awk '{print $5}') bytes)"
