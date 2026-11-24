#!/bin/sh
# Install the build dependencies and create tools/venv.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

if [ "$1" = "--system" ]; then
    SUDO=""
    [ "$(id -u)" -ne 0 ] && SUDO="sudo"
    $SUDO apt-get update
    $SUDO apt-get install -y \
        build-essential clang lld pkg-config meson ninja-build python3-venv git \
        libsdl3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
        libvulkan-dev libshaderc-dev \
        libdrm-dev libwayland-dev libwayland-egl-backend-dev libdisplay-info-dev \
        llvm-21-dev libllvmspirvlib-21-dev python3-mako
fi

command -v clang >/dev/null || { echo "error: clang not found (run $0 --system)" >&2; exit 1; }
command -v meson >/dev/null || { echo "error: meson not found (run $0 --system)" >&2; exit 1; }

python3 -m venv tools/venv
tools/venv/bin/pip install --upgrade pip
tools/venv/bin/pip install capstone lz4 unicorn Pillow

echo
echo "tools/venv ready. Next:"
echo "  ./scripts/build-mesa.sh      # or unpack a prebuilt Mesa 27 into deps/"
echo "  make gen -j\$(nproc) && make -j\$(nproc)"
echo "  ./run-botw.sh"
