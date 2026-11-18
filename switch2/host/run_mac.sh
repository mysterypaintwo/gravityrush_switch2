#!/usr/bin/env bash
# Runs the ARM64 host build on the Mac (development).
# Usage: switch2/host/run_mac.sh <game folder with eboot.bin, sce_module, sce_sys, arc>
# Builds out/gr_host if needed and prepares the boot files into out/arm64-prep once.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
game=${1:?usage: $0 <game folder>}
[[ -d third_party/box64/.git ]] || switch2/box64/fetch_box64.sh
if [[ ! -f out/gr_host/build.ninja ]]; then
    cmake -S switch2/host -B out/gr_host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=/opt/homebrew
fi
ninja -C out/gr_host gr_host
prep=out/arm64-prep
if [[ ! -f $prep/boot-linked.bin ]]; then
    mkdir -p "$prep"
    python3 scripts/prepare.py "$game" --out "$prep"
    python3 scripts/link_libc.py "$game" --out "$prep"
    python3 scripts/link_modules.py "$game" --out "$prep" --modules libc.prx libSceFios2.prx
    python3 scripts/content_profile.py "$game" --out "$prep" --sku full
    python3 scripts/patches.py --out "$prep" --game-dir "$game" --patches-dir patches --patches-config patches/patches.json
fi
mkdir -p out/arm64-user
exec env BOX64_NOBANNER=1 out/gr_host/gr_host "$prep/boot-linked.bin" --content-profile "$prep/content.bin" \
    --patches "$prep/patches.bin" --app0 "$game" --user out/arm64-user
