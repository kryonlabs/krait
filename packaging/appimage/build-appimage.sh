#!/bin/sh
# Build the Krait Linux AppImage.
#
# Locally this stages the AppDir (binary, icon, desktop file, fonts, AppRun).
# When linuxdeploy + the appimage plugin are on PATH (as in CI) it also
# bundles the shared libraries and produces the final .AppImage in
# build/dist/. Requires: KRAIT built via `make krait`.
#
# Environment:
#   APPIMAGE_EXTRACT_AND_RUN=1  needed on hosts without FUSE (CI runners)
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
platform_dir="linux-x86_64"
bin="${KRAIT_BIN:-$root/build/$platform_dir/bin/krait}"
version=$(sed -n 's/^#define KRAIT_VERSION_STRING "\(.*\)"/\1/p' \
    "$root/src/version.h")
[ -n "$version" ] || { echo "cannot read version from src/version.h"; exit 1; }
[ -x "$bin" ] || { echo "krait binary missing: $bin (run make krait)"; exit 1; }

dist="$root/build/dist"
appdir="$dist/krait.AppDir"
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" \
    "$appdir/usr/share/applications" \
    "$appdir/usr/share/icons/hicolor/512x512/apps" \
    "$appdir/usr/share/krait/fonts/noto"

cp "$bin" "$appdir/usr/bin/krait"
cp "$root/packaging/appimage/AppRun" "$appdir/AppRun"
chmod +x "$appdir/AppRun"
cp "$root/packaging/appimage/krait.desktop" \
    "$appdir/usr/share/applications/krait.desktop"
cp "$root/packaging/appimage/krait.desktop" "$appdir/krait.desktop"
cp "$root/vendor/kryon/icons/kryon.png" "$appdir/krait.png"
cp "$root/vendor/kryon/icons/kryon.png" \
    "$appdir/usr/share/icons/hicolor/512x512/apps/krait.png"
cp "$root/vendor/kryon/fonts/noto/NotoSans-Regular.ttf" \
    "$appdir/usr/share/krait/fonts/noto/"
cp "$root/vendor/kryon/fonts/noto/LICENSE.txt" \
    "$appdir/usr/share/krait/fonts/noto/" 2>/dev/null || true

echo "Staged $appdir"

if ! command -v linuxdeploy >/dev/null 2>&1; then
    echo "linuxdeploy not found; AppDir staged only (CI bundles libraries)"
    exit 0
fi

command -v linuxdeploy-plugin-appimage >/dev/null 2>&1 || {
    echo "linuxdeploy-plugin-appimage is missing"; exit 1;
}

OUTPUT="$dist"
export OUTPUT
linuxdeploy --appdir "$appdir" --plugin gtk --plugin appimage \
    --output appimage

appimage=$(find "$dist" -maxdepth 1 -name '*.AppImage' -print -quit)
[ -n "$appimage" ] || { echo "linuxdeploy produced no AppImage"; exit 1; }
final="$dist/krait-$version-x86_64.AppImage"
mv "$appimage" "$final"
(cd "$dist" && sha256sum "$(basename "$final")" > "$(basename "$final").sha256")
echo "Built $final"
