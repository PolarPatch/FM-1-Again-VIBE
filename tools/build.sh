#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# FM-1 Again VIBE: build, check, (optionally) play.  tools/build.sh [check|emu]
#   sdk/       the VIBE FM SDK, cloned (not in git): git clone https://github.com/Vibe-FM-SDK/vibe-fm-sdk sdk
#   ZIG        Zig 0.13.0 (default: on the PATH, or ~/.local/zig/zig-macos-aarch64-0.13.0/zig)
# The SDK's build.sh takes the firmware's folder by a path without spaces: a link to it in ~/.cache does that.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
[ -x "$HOME/.local/zig/zig-macos-aarch64-0.13.0/zig" ] && PATH="$HOME/.local/zig/zig-macos-aarch64-0.13.0:$PATH"
LINK="$HOME/.cache/fm1-again-vibe-build"
mkdir -p "$LINK"
ln -sfn "$HERE/firmware/fm1-again-vibe" "$LINK/fm1-again-vibe"
cd "$HERE/sdk"
sh tools/build.sh "$LINK/fm1-again-vibe"
mkdir -p "$HERE/build"
cp build/fm1-again-vibe.wasm build/fm1-again-vibe.zip "$HERE/build/"
[ "${1:-}" = check ] || [ "${1:-}" = emu ] && node tools/vibe-check.mjs build/fm1-again-vibe.wasm
[ "${1:-}" = emu ] && exec python3 tools/emulator.py
exit 0
