#!/bin/sh
# Package a debug APK from build/android-arm64/libkrait.so.
# Produces build/android-arm64/krait.apk (signed + zipaligned, installable).
#
# Environment:
#   ANDROID_HOME / ANDROID_SDK_ROOT - SDK root (auto-detected if unset)
set -eu

KCWD=$(cd "$(dirname "$0")/.." && pwd)
cd "$KCWD"

SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
BT=$(ls -d "$SDK"/build-tools/* 2>/dev/null | tail -1)
PLATFORM=$(ls -d "$SDK"/platforms/android-* 2>/dev/null | tail -1)/android.jar
SO=build/android-arm64/libkrait.so
if [ ! -f "$SO" ]; then
    echo "error: $SO missing. Run scripts/build-android-native.sh first." >&2
    exit 1
fi
if [ ! -f "$PLATFORM" ]; then
    echo "error: android.jar not found under $SDK/platforms. Install a platform via sdkmanager." >&2
    exit 1
fi

OUT=build/android-arm64
MANIFEST=droid/app/src/main/AndroidManifest.xml

# --- debug keystore -------------------------------------------------------
KSTORE=$HOME/.android/debug.keystore
if [ ! -f "$KSTORE" ]; then
    mkdir -p "$HOME/.android"
    keytool -genkeypair -v -keystore "$KSTORE" -storepass android \
        -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 \
        -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi

# --- stage native lib -----------------------------------------------------
STAGE=$OUT/apk-stage
rm -rf "$STAGE" && mkdir -p "$STAGE/lib/arm64-v8a"
cp "$SO" "$STAGE/lib/arm64-v8a/"

# --- aapt: base APK + native lib -----------------------------------------
$BT/aapt package -f -M "$MANIFEST" -I "$PLATFORM" -F $OUT/krait-unsigned.apk
( cd "$STAGE" && $BT/aapt add ../krait-unsigned.apk lib/arm64-v8a/libkrait.so )

# --- sign + align ---------------------------------------------------------
jarsigner -keystore "$KSTORE" -storepass android -keypass android \
    -sigalg SHA256withRSA -digestalg SHA-256 $OUT/krait-unsigned.apk androiddebugkey >/dev/null 2>&1
$BT/zipalign -f 4 $OUT/krait-unsigned.apk $OUT/krait.apk

echo "done: $OUT/krait.apk ($(ls -la $OUT/krait.apk | awk '{print $5}') bytes)"
echo "install with: ${SDK}/platform-tools/adb install -r $OUT/krait.apk"
