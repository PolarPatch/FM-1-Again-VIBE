/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* The MIDI basics (after Dinghy 0.6's kit_midi.h, zvenson, GPL-3.0: written anew): pitch bend (+-2 semitones) and the
 * mod wheel (a vibrato, 5.5 Hz, up to a third of a semitone) on KEYS, VOX and CHOP; the sustain pedal (CC 64: note
 * offs held until it lets go); volume (CC 7); all notes off (CC 120, 123). mb_cc / mb_bend from the audio interrupt
 * (the MIDI in), mb_block once a block before the voices: mb_pitch is what their steps are multiplied by.
 * Part of core.c. */

static struct {
    float bend;                                  /* -1 .. 1 */
    float mod;                                   /* 0 .. 1 */
    float vol;                                   /* CC 7: 0 .. 1 */
    float vib;                                   /* the vibrato's phase, turns */
    uint8_t sus;                                 /* the pedal down */
    uint32_t held[4];                            /* note offs the pedal holds: a bit a note */
} mb = {0.0f, 0.0f, 1.0f, 0.0f, 0, {0, 0, 0, 0}};
static float mb_pitch = 1.0f;                    /* this block's: the bend and the vibrato */

static void mb_bend(uint32_t v14) { mb.bend = ((float)v14 - 8192.0f) / 8192.0f; }

static void mb_block(uint32_t n)
{
    float semis = 2.0f * mb.bend;
    mb.vib += 5.5f * (float)n / SR;
    mb.vib -= mb.vib >= 1.0f ? 1.0f : 0.0f;
    if (mb.mod > 0.0f)
        semis += 0.33f * mb.mod * fm_sin_turns(mb.vib);
    mb_pitch = semis != 0.0f ? fm_exp2f(semis / 12.0f) : 1.0f;
}
