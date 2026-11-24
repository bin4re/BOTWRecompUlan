#!/bin/sh
# BOTWRecompUlan launcher: Mesa 27 panvk if deps/ has it, zero-copy when udmabuf is available.
cd "$(dirname "$0")" || exit 1
ROOT=$(pwd)

# A prebuilt release ships deps/mesa/libvulkan_panfrost.so; write its ICD on first run.
if [ ! -f "$ROOT/deps/mesa-icd/panfrost_devel.json" ] && [ -f "$ROOT/deps/mesa/libvulkan_panfrost.so" ]; then
    mkdir -p "$ROOT/deps/mesa-icd"
    cat > "$ROOT/deps/mesa-icd/panfrost_devel.json" <<JSON
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "$ROOT/deps/mesa/libvulkan_panfrost.so",
    "api_version": "1.4.0"
  }
}
JSON
fi

if [ -f "$ROOT/deps/mesa-icd/panfrost_devel.json" ]; then
    export VK_DRIVER_FILES="$ROOT/deps/mesa-icd/panfrost_devel.json"
else
    echo "warning: deps/mesa-icd/panfrost_devel.json not found; falling back to the system Vulkan driver." >&2
    echo "         Mesa 26.0.8 cannot run BOTW; see README / scripts/build-mesa.sh." >&2
fi

if [ -r /dev/udmabuf ] && [ -w /dev/udmabuf ]; then
    export LARECOMP_VK_MEMFD=1
else
    echo "warning: /dev/udmabuf missing or not writable; using copy mode (slow, may stutter)." >&2
    echo "         See README section '零拷贝' for how to enable it." >&2
    export LARECOMP_VK_MEMFD=0
fi

export LARECOMP_LOG_PACE=1
export LARECOMP_SHADER_WORKERS="${LARECOMP_SHADER_WORKERS:-8}"

BIN="$ROOT/build/botwrecomp"
[ -x "$BIN" ] || BIN="$ROOT/botwrecomp"
[ -x "$BIN" ] || { echo "error: botwrecomp not found (source tree: run make; release: check the package)." >&2; exit 1; }
exec "$BIN" "$@"
