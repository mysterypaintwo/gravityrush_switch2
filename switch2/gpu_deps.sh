#!/bin/sh
# The GPU library's dependencies for the Switch 2 build (out/switch2-deps), which the console's
# portlibs do not have:
# - header-only libraries (Boost, fmt, tsl-robin-map) from Homebrew, linked, not copied: they
#   are the versions the Mac build uses;
# - Zydis and Zycore (the shader recompiler decodes guest x86 code), cloned at the Mac build's
#   versions and cross-compiled with the Switch 2 toolchain.
# Vulkan, FFmpeg, freetype and xxhash come from the devkit's portlibs.
# Run once (from anywhere); the host build finds the result through GR_SWITCH2_DEPS.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS="$ROOT/out/switch2-deps"
BREW=${HOMEBREW_PREFIX:-/opt/homebrew}
DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
ZYDIS_TAG=v4.1.1
ZYCORE_TAG=v1.5.2

mkdir -p "$DEPS/include" "$DEPS/src" "$DEPS/lib"
for h in boost fmt tsl; do
    [ -e "$BREW/include/$h" ] || { echo "missing $BREW/include/$h (brew install boost fmt robin-map)"; exit 1; }
    ln -sfn "$BREW/include/$h" "$DEPS/include/$h"
done

# Vulkan: OunceVK's C headers have no vulkan.hpp, and the C++ bindings must match the C headers'
# version, so the GPU library compiles against Homebrew's complete set (newer than the driver's;
# every function is loaded at run time, so that is harmless).
mkdir -p "$DEPS/vulkan/include"
for h in vulkan vk_video; do
    [ -e "$BREW/include/$h" ] || { echo "missing $BREW/include/$h (brew install vulkan-headers)"; exit 1; }
    ln -sfn "$BREW/include/$h" "$DEPS/vulkan/include/$h"
done

fetch() { # name url tag
    if [ ! -d "$DEPS/src/$1" ]; then
        git clone --depth 1 --branch "$3" "$2" "$DEPS/src/$1"
    fi
}
fetch zycore https://github.com/zyantific/zycore-c.git "$ZYCORE_TAG"
fetch zydis https://github.com/zyantific/zydis.git "$ZYDIS_TAG"

TOOLCHAIN="$DEVKITPRO/libounce/cmake/Switch2.cmake"
# Zycore's process API (unused by the decoder) calls msync, which Horizon does not have.
cat > "$DEPS/zycore_horizon.h" <<'H'
#pragma once
#include <stddef.h>
#define MS_INVALIDATE 2
#define MS_SYNC 4
static inline int msync(void* addr, size_t len, int flags) { (void)addr; (void)len; (void)flags; return 0; }
H
cmake -S "$DEPS/src/zycore" -B "$DEPS/build/zycore" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release -DZYCORE_BUILD_SHARED_LIB=OFF -DCMAKE_INSTALL_PREFIX="$DEPS" \
    "-DCMAKE_C_FLAGS=-D__posix -I$ROOT/switch2/box64/horizon -include $DEPS/zycore_horizon.h" # newlib: POSIX code
cmake --build "$DEPS/build/zycore" --target install
cmake -S "$DEPS/src/zydis" -B "$DEPS/build/zydis" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release -DZYDIS_BUILD_SHARED_LIB=OFF -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF \
    -DZYDIS_BUILD_TESTS=OFF -DZYAN_SYSTEM_ZYCORE=ON -DCMAKE_PREFIX_PATH="$DEPS" -DCMAKE_FIND_ROOT_PATH="$DEPS" \
    -DCMAKE_INSTALL_PREFIX="$DEPS" "-DCMAKE_C_FLAGS=-I$DEPS/include"
cmake --build "$DEPS/build/zydis" --target install
echo "GPU dependencies ready in $DEPS"
