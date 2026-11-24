#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# FM-1 Again VIBE: the sound engine, copied from FM-1 Again (never edited here): app/core.c and what it includes
# (engine/, dsp/, gen/again_chop.h, gen/again_tables.h) and fm1-base's fastmath.h, into the firmware's freestanding/
# (sdk/tools/build.sh puts it on the include path, so its includes resolve as in FM-1 Again's app/).
#   AGAIN=/path/to/FM-1-Again tools/sync_engine.sh     (default: the folder beside this one)
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
AGAIN="${AGAIN:-$HERE/../ClaudeCode - FM-1 Again}"
DST="$HERE/firmware/fm1-again-vibe/freestanding"
[ -f "$AGAIN/app/core.c" ] || { echo "sync_engine: no FM-1 Again at $AGAIN (set AGAIN)"; exit 1; }
mkdir -p "$DST/engine" "$DST/dsp" "$DST/gen"
cp "$AGAIN/app/core.c" "$DST/core.c"
cp "$AGAIN"/app/engine/*.h "$DST/engine/"
cp "$AGAIN"/app/dsp/*.h "$DST/dsp/"
cp "$AGAIN/app/gen/again_chop.h" "$AGAIN/app/gen/again_tables.h" "$AGAIN/app/gen/again_pixfont.h" "$DST/gen/"
cp "$AGAIN/base/include/fastmath.h" "$DST/fastmath.h"
printf 'FM-1 Again %s (%s)\n' "$(git -C "$AGAIN" describe --tags --always 2>/dev/null)" "$(git -C "$AGAIN" rev-parse --short HEAD 2>/dev/null)" > "$DST/ENGINE_FROM"
echo "sync_engine: $(cat "$DST/ENGINE_FROM") -> $DST"
