#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

SHA_TAG="${1:-local}"
BUILD_DIR="${BUILD_DIR:-build-linux}"
APPDIR="${APPDIR:-AppDir}"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Missing required command: $1" >&2
    exit 1
  }
}

need_cmd cmake
need_cmd cpack
need_cmd wget
need_cmd chmod
need_cmd cp

if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  if ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=${ROOT_DIR}" "$BUILD_DIR/CMakeCache.txt"; then
    echo "[package-linux] Removing stale build dir: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
  fi
fi

mkdir -p dist

# Build .deb via CPack
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --config Release -j "${JOBS:-$(nproc)}"
(
  cd "$BUILD_DIR"
  cpack -G DEB
)
DEB_FILE="$(find "$BUILD_DIR" -maxdepth 1 -type f -name '*.deb' | head -n1)"
cp -v "$DEB_FILE" "dist/worldsim3-${SHA_TAG}.deb"

# Build AppImage via linuxdeploy + appimagetool
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/scalable/apps"
cp -v "$BUILD_DIR/worldsim3" "$APPDIR/usr/bin/worldsim3"
cp -v packaging/linux/worldsim3.desktop "$APPDIR/usr/share/applications/worldsim3.desktop"
cp -v packaging/linux/worldsim3.svg "$APPDIR/usr/share/icons/hicolor/scalable/apps/worldsim3.svg"

wget -q -O linuxdeploy-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
wget -q -O appimagetool-x86_64.AppImage https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
chmod +x linuxdeploy-x86_64.AppImage appimagetool-x86_64.AppImage

export OUTPUT="worldsim3-${SHA_TAG}-x86_64.AppImage"
export APPIMAGE_EXTRACT_AND_RUN=1
./linuxdeploy-x86_64.AppImage \
  --appdir "$APPDIR" \
  --desktop-file packaging/linux/worldsim3.desktop \
  --icon-file packaging/linux/worldsim3.svg \
  --executable "$APPDIR/usr/bin/worldsim3" \
  --output appimage

APPIMAGE_FILE="$(find . -maxdepth 1 -type f -name 'worldsim3-*-x86_64.AppImage' | head -n1)"
cp -v "$APPIMAGE_FILE" "dist/worldsim3-${SHA_TAG}.AppImage"

echo "Produced:"
echo "  dist/worldsim3-${SHA_TAG}.deb"
echo "  dist/worldsim3-${SHA_TAG}.AppImage"
