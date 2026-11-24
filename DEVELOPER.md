# FM-1 Again VIBE: developer notes

FM-1 Again as a native VIBE FM firmware: one `.wasm` that runs as a VST3 / AU / CLAP plugin (VIBE FM VST), in the
browser (the Playground, the SDK's emulator) and on the VIBE FM hardware. Our entry for **the VIBE Box Challenge** on
The Firmware Hub (submissions close **15 December 2026**). Firmware id (the folder's name, permanent): `fm1-again-vibe`.

## How it is built

- **The engine is FM-1 Again's**, never edited here: `tools/sync_engine.sh` copies `app/core.c` (the sound, apart from
  the FM-1) and what it includes from the FM-1 Again repository beside this one into `firmware/fm1-again-vibe/freestanding/`
  (on the include path, so its includes resolve as in FM-1 Again's `app/`). `freestanding/ENGINE_FROM` names the commit.
- **The port** (`main.c`, `port/pages.c`): the engine at its own 44.1 kHz in blocks of 256, a ring read at the host's
  rate (cubic Hermite, as Arctic Scream VIBE); two flows (VIBE_PARALLEL_AUDIO): the control flow is the one writer of the
  settings and hands the sound flow notes, PLAY, BUILD, the tricks and "apply" through a queue.
- **The controls** (house style): keys = what ALGORITHM (encoder A) chose; encoders 1-4 the page's values, B its main
  choice; K1-K6 SOUND (KEYS, VOX, CHOP), PUMP, GROOVE (GROOVE, RISE), BUILD, FX (SPACE, ECHO, TAPE), MIX (MIX, SETUP);
  RETURN home; HOME the controls; F1 HOME, F2 BUILD/DROP, F3 what the keys play, F4 LOOP/SONG, F5 SAVE, F6 the page's
  main choice on; pads 1-13 PERFORM's tricks, 14-16 BUILD, PLAY, STOP (CHOP on the keys: its slices); FOCUS on the
  four values and F1-F6. Every FM-1 Again parameter is a DAW parameter (ids in `P_ID`, append only).
- **The DAW**: its tempo and PLAY lead when it plays (`vibe_transport`); the settings are kept in the project
  (`vibe_store`, `VIBE_EV_SAVE`).

## Build, check, play

    git clone https://github.com/Vibe-FM-SDK/vibe-fm-sdk sdk      # once (Apache-2.0, not in git)
    tools/sync_engine.sh                                          # the engine from ../ClaudeCode - FM-1 Again (AGAIN=...)
    tools/build.sh check                                          # build/fm1-again-vibe.{wasm,zip} + vibe-check (PASSED)
    tools/build.sh emu                                            # and the emulator at http://127.0.0.1:8766
    node sdk/tools/wasmtest.mjs build/fm1-again-vibe.wasm tests/tour.json build/tour    # audio + screens

Needs Zig 0.13.0 (`~/.local/zig/...` is found by tools/build.sh). The SDK's build.sh cannot take a path with spaces:
tools/build.sh builds through a link in `~/.cache`.

## What the FM-1 cannot, and what this does with it (the challenge's "VIBE features")

1. Done: the 480 x 320 display in FM-1 Again's poster look, the 16 pads (tricks, CHOP's slices), the DAW (parameters,
   tempo, PLAY, the project keeps the settings), two flows.
2. Next: **sound in straight into CHOP** (REC: record IN 1, cut it at its onsets, play it on the keys and pads, kept in
   the project); **long samples** (minutes, not 8 s; WAVs from the host's media); VIBE LINK (the pink FM-1 as its
   controller: the page and the encoders are reported already); the stage (the screensaver's crowd); the boot screen.

## Rules (the SDK's and ours)

- `vibe-check` PASSED before anything is called done. The SDK's host is not ours: never patch VIBE FM VST or the
  emulator; what the host lacks is an issue in Vibe-FM-SDK/vibe-fm-sdk.
- "Built with the VIBE FM SDK" is fine; "VIBE FM compatible" or its logo need permission.
- No artist's name or recording in the firmware or its texts (FM-1 Again's rule).
