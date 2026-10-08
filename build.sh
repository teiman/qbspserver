#!/usr/bin/env bash
# Builds ericw-tools (Release) on Linux. The Linux counterpart of build.bat.
#   ./build.sh            -> everything
#   ./build.sh light      -> a single target (light, qbsp, vis, bspinfo...)
# Embree and TBB are downloaded into ericw-tools/deps/linux on the first run.
# The GPU backend (LIGHT_ENABLE_VULKAN_GPU) is enabled when the Vulkan headers and
# glslangValidator are found; set VKLIGHT_GPU=ON or OFF to force it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$ROOT/ericw-tools"
BUILD="$SRC/build-linux"
DEPS="$SRC/deps/linux"
EMBREE_VER=4.4.0
TBB_VER=2021.11.0

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "[build] '$1' not found. Install the build tools first, e.g.:"
        echo "  Debian/Ubuntu: sudo apt install build-essential cmake ninja-build curl libvulkan-dev glslang-tools"
        echo "  Fedora:        sudo dnf install gcc-c++ cmake ninja-build curl vulkan-loader-devel vulkan-headers glslang"
        echo "  Arch:          sudo pacman -S base-devel cmake ninja curl vulkan-icd-loader vulkan-headers glslang"
        exit 1
    }
}
need cmake
need c++
need curl
need tar

download() { # url dest-dir
    local tmp
    tmp="$(mktemp)"
    echo "[build] Downloading $1"
    curl -fsSL "$1" -o "$tmp"
    mkdir -p "$2"
    tar xzf "$tmp" -C "$2"
    rm -f "$tmp"
}
if [ ! -d "$DEPS/embree-$EMBREE_VER" ]; then
    download "https://github.com/RenderKit/embree/releases/download/v$EMBREE_VER/embree-$EMBREE_VER.x86_64.linux.tar.gz" \
        "$DEPS/embree-$EMBREE_VER"
fi
if [ ! -d "$DEPS/oneapi-tbb-$TBB_VER" ]; then
    download "https://github.com/uxlfoundation/oneTBB/releases/download/v$TBB_VER/oneapi-tbb-$TBB_VER-lin.tgz" "$DEPS"
fi

# Vulkan GPU backend for vklight (used unless -cpu) if the Vulkan headers and shader compiler are installed
GPU="${VKLIGHT_GPU:-}"
if [ -z "$GPU" ]; then
    GPU=OFF
    if [ -n "${VULKAN_SDK:-}" ]; then
        GPU=ON
        export PATH="$VULKAN_SDK/bin:$PATH"
    elif command -v glslangValidator >/dev/null 2>&1 && [ -f /usr/include/vulkan/vulkan.h ]; then
        GPU=ON
    fi
fi
if [ "$GPU" = OFF ] && [ -z "${VKLIGHT_GPU:-}" ]; then
    echo "[build] No Vulkan headers/glslangValidator: building without the GPU backend (vklight will run on the CPU)."
fi

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    GENERATOR="Unix Makefiles"
    command -v ninja >/dev/null 2>&1 && GENERATOR=Ninja
    echo "[build] Not configured yet. Configuring (GPU=$GPU, $GENERATOR)..."
    cmake -S "$SRC" -B "$BUILD" -G "$GENERATOR" -DCMAKE_BUILD_TYPE=Release \
        -Dembree_DIR="$DEPS/embree-$EMBREE_VER/lib/cmake/embree-$EMBREE_VER" \
        -DTBB_DIR="$DEPS/oneapi-tbb-$TBB_VER/lib/cmake/tbb" \
        -DLIGHT_ENABLE_VULKAN_GPU="$GPU" \
        -DENABLE_LIGHTPREVIEW=OFF -DDISABLE_DOCS=ON -DDISABLE_TESTS=ON
fi

TARGET=()
if [ -n "${1:-}" ]; then TARGET=(--target "$1"); fi

T0="$(date +%T)"
RC=0
cmake --build "$BUILD" --parallel "$(nproc 2>/dev/null || echo 4)" ${TARGET[@]+"${TARGET[@]}"} || RC=$?
echo
if [ "$RC" -ne 0 ]; then
    echo "[build] FAILED (code $RC)  start $T0  end $(date +%T)"
else
    echo "[build] OK  start $T0  end $(date +%T)"
fi
exit "$RC"
