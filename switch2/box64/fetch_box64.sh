#!/usr/bin/env bash
# Fetches Box64 v0.4.4 into third_party/box64 and applies the embedded-build patch
# (switch2/box64/patches: split JIT, guest r8 out of x18, no signal-based features).
# Safe to run again: an already patched tree is left as it is.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
dest=third_party/box64
patch=$PWD/switch2/box64/patches/box64-v0.4.4-embed.patch
if [[ ! -d $dest/.git ]]; then
    git clone --quiet --depth 1 --branch v0.4.4 https://github.com/ptitSeb/box64.git "$dest"
fi
if git -C "$dest" apply --reverse --check "$patch" 2>/dev/null; then
    echo "Box64: patch already applied"
else
    git -C "$dest" apply "$patch"
    echo "Box64: patch applied"
fi
