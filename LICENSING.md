# Licensing

FM-1 Again VIBE is free software under the **GNU General Public License v3.0 or later** (`LICENSE`).

| Part | Where | Licence |
| --- | --- | --- |
| The port: `main.c`, `port/`, `tools/`, `tests/` | this repository | GPL-3.0-or-later, PolarPatch |
| FM-1 Again's sound engine: `freestanding/core.c`, `engine/`, `dsp/`, `gen/` | copied unchanged from [FM-1 Again](https://github.com/PolarPatch/FM-1-Again) by `tools/sync_engine.sh` | GPL-3.0 (FM-1 Again's LICENSING.md names every part: SPACE, ECHO and TAPE from Arctic Scream, which took them from Felucca and GR!MM) |
| `freestanding/gen/again_pixfont.h` (the boot screen's letters) | Press Start 2P (the Press Start 2P Project Authors), rasterised by Arctic Scream p0larpatch as the "pixfont" | SIL OFL 1.1 |
| `freestanding/fastmath.h` | fm1-base (Charles Vestal) | MPL-2.0 |
| `freestanding/string.h` | after Arctic Scream VIBE's (PolarPatch) | GPL-3.0-or-later |
| The VIBE FM SDK (`sdk/`, cloned at build time, not in this repository) | Sven Trogus (zvenson) | Apache-2.0 |
| `freestanding/gen/again_film.h` (the screensaver's film, inside the `.wasm`), `freestanding/film.h` (its decoder) | the film: FM-1 Again's `app/media/again_saver.bin`, footage from the USB002 tour's creative drop, dithered; not under the GPL, not this project's branding. The decoder: FM-1 Again, GPL-3.0-or-later | as the drop's terms allow / GPL-3.0-or-later |
| `icon.png` | the screensaver's film: footage from the USB002 tour's creative drop (see FM-1 Again's LICENSING.md), dithered; not under the GPL, not this project's branding | as the drop's terms allow |

Built with the VIBE FM SDK. FM-1 Again is not made, endorsed or approved by any artist.
