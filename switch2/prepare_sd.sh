#!/usr/bin/env bash
# Prepares the Switch 2 port's SD card files from your own unpacked copy of the game.
# Usage: switch2/prepare_sd.sh <game folder> <output folder>      (macOS, Linux; Windows: prepare_sd.ps1)
#   <game folder>    the unpacked game (eboot.bin, sce_module, sce_sys, arc), see switch2/SETUP.txt
#   <output folder>  the SD card's root (or any folder to copy to the SD card's root later)
# Writes <output>/switch/gravityrush_remastered/: the boot files made from your game (prep/), your game's
# files (game/) and the NRO when it is built (out/gr_switch/gravityrush_remastered.nro). Nothing is
# downloaded and nothing from the game leaves your computer except to that folder.
set -euo pipefail
game=${1:?usage: $0 <game folder> <output folder>}
out=${2:?usage: $0 <game folder> <output folder>}
# Absolute paths first: the scripts run from the repository folder.
[[ -d "$game" ]] || { echo "error: no folder $game"; exit 1; }
game=$(cd -- "$game" && pwd)
mkdir -p -- "$out"
out=$(cd -- "$out" && pwd)
cd -- "$(dirname -- "$0")/.."
PY=$(command -v python3 || command -v python) || { echo "error: Python 3 is needed"; exit 1; }
"$PY" -c 'import sys; sys.exit(sys.version_info < (3, 8))' || { echo "error: $PY is not Python 3.8 or newer"; exit 1; }
for need in eboot.bin sce_module sce_sys arc; do
    [[ -e "$game/$need" ]] || { echo "error: $game has no $need (see switch2/SETUP.txt)"; exit 1; }
done
dest="$out/switch/gravityrush_remastered"
mkdir -p "$dest/prep" "$dest/game" "$dest/user"

echo "== boot files (made from your eboot.bin and modules)"
prep=$(mktemp -d)
trap 'rm -rf -- "$prep"' EXIT
"$PY" scripts/prepare.py "$game" --out "$prep"
"$PY" scripts/link_libc.py "$game" --out "$prep"
"$PY" scripts/link_modules.py "$game" --out "$prep" --modules libc.prx libSceFios2.prx
"$PY" scripts/content_profile.py "$game" --out "$prep" --sku full
"$PY" scripts/patches.py --out "$prep" --game-dir "$game" --patches-dir patches --patches-config patches/patches.json
cp "$prep/boot-linked.bin" "$prep/content.bin" "$prep/patches.bin" "$dest/prep/"

echo "== game files (about 9.3 GB; files already copied with the same size are skipped)"
copy_tree() { # src... dest: rsync when there is one (macOS, most Linux), else cp
    if command -v rsync >/dev/null 2>&1; then
        rsync -rtL --size-only "$@"   # -L: copy what links point to (SD cards hold no links)
    else
        cp -RL "$@"
    fi
}
copy_tree "$game/eboot.bin" "$game/sce_module" "$game/sce_sys" "$game/arc" "$dest/game/"
# args.txt holds the game's launch arguments ("-region E" for the European release): without it the
# game picks the Japanese region and shows Japanese text whatever its language setting.
if [[ -f "$game/args.txt" ]]; then
    cp "$game/args.txt" "$dest/game/"
else
    echo "warning: no args.txt in $game; the game will start in Japanese (gravityrush_remastered.nro writes one for the European release)"
fi

# The homebrew menu icon: your game's own icon (sce_sys/icon0.png) as a 256x256 JPEG, put into the
# NRO copied to the SD card (the built NRO has none: no game art is in the repository or the build).
# Needs elf2nro (devkitPro tools) and sips (macOS), ImageMagick or Python's Pillow; without them the
# NRO is copied as built.
make_icon() {
    local png="$game/sce_sys/icon0.png" jpg="$1"
    [[ -f "$png" ]] || return 1
    if command -v sips >/dev/null; then
        sips -s format jpeg -s formatOptions 90 -z 256 256 "$png" --out "$jpg" >/dev/null 2>&1 && return 0
    fi
    if command -v magick >/dev/null; then magick "$png" -resize 256x256! -quality 90 "$jpg" && return 0; fi
    if command -v convert >/dev/null; then convert "$png" -resize 256x256! -quality 90 "$jpg" && return 0; fi
    "$PY" -I -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).convert("RGB").resize((256, 256), Image.LANCZOS).save(sys.argv[2], quality=90)' "$png" "$jpg" 2>/dev/null
}
elf2nro=$(command -v elf2nro || echo "${DEVKITPRO:-/opt/devkitpro}/tools/bin/elf2nro")
built=out/gr_switch/gravityrush_remastered
if [[ -f $built.nro ]]; then
    icon=$(mktemp -d)/icon.jpg
    if [[ -f $built.elf && -f $built.nacp && -x "$elf2nro" ]] && make_icon "$icon" &&
        "$elf2nro" "$built.elf" "$dest/gravityrush_remastered.nro" --icon="$icon" --nacp="$built.nacp" >/dev/null; then
        echo "== gravityrush_remastered.nro copied, with your game's icon"
    else
        cp $built.nro "$dest/"
        echo "== gravityrush_remastered.nro copied (without an icon: see switch2/SETUP.txt)"
    fi
    rm -rf -- "$(dirname -- "$icon")"
else
    echo "== gravityrush_remastered.nro is not built yet: copy it to $dest/ when it is"
fi
echo "Done: copy the 'switch' folder in $out to the root of the SD card (merge with the one there)."
