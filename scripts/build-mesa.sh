#!/bin/sh
# Build Mesa 27 panvk (panfrost Vulkan) into deps/ and write the ICD JSON.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

MESA_COMMIT=6c2135ab76
SRC="$ROOT/deps/mesa-src"
BUILD="$ROOT/deps/mesa/build"
ICD="$ROOT/deps/mesa-icd"

for t in git meson ninja clang; do
    command -v "$t" >/dev/null || { echo "error: $t not found (run scripts/setup.sh --system)" >&2; exit 1; }
done

mkdir -p "$ROOT/deps"
if [ ! -d "$SRC/.git" ]; then
    git clone --filter=blob:none https://gitlab.freedesktop.org/mesa/mesa.git "$SRC"
fi
git -C "$SRC" fetch --filter=blob:none origin
git -C "$SRC" checkout "$MESA_COMMIT"

if [ -d "$BUILD" ]; then
    meson setup --reconfigure "$BUILD" "$SRC" \
        --buildtype=release -Dplatforms=wayland \
        -Dvulkan-drivers=panfrost -Dgallium-drivers=panfrost -Dglx=disabled
else
    meson setup "$BUILD" "$SRC" \
        --buildtype=release -Dplatforms=wayland \
        -Dvulkan-drivers=panfrost -Dgallium-drivers=panfrost -Dglx=disabled
fi
ninja -C "$BUILD" src/panfrost/vulkan/libvulkan_panfrost.so

mkdir -p "$ICD"
cat > "$ICD/panfrost_devel.json" <<JSON
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "$BUILD/src/panfrost/vulkan/libvulkan_panfrost.so",
    "api_version": "1.4.0"
  }
}
JSON

echo
echo "Mesa 27 built: $BUILD/src/panfrost/vulkan/libvulkan_panfrost.so"
echo "ICD written:  $ICD/panfrost_devel.json"
