/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* The clock: sixteenths at TEMPO with SWING (the odd sixteenths late: the even ones longer, the odd ones shorter by
 * as much, so a beat keeps its length). It always runs, so the waveform keeps moving while nothing plays; RUN (PLAY)
 * lets the steps sound. ck_frame, once a frame in the audio interrupt, says when a step begins. Part of app.c. */

static struct {
    volatile uint8_t run;                        /* the groove playing */
    float bpm;                                   /* TEMPO */
    float swing;                                 /* 0 .. 0.33: how much of a sixteenth the odd ones come late */
    uint32_t step;                               /* the step now (since PLAY: step 0 is the first bar's first) */
    float left, len;                             /* frames left in it, and its length */
} ck = {0, 124.0f, 0.0f, 0, 0.0f, 1.0f};

static inline float ck_step_len(uint32_t step)
{
    float l = SR * 15.0f / ck.bpm;               /* a sixteenth */
    return step & 1u ? l * (1.0f - ck.swing) : l * (1.0f + ck.swing);
}

/* the main loop: PLAY. The next frame is the first bar's first step */
static void ck_start(void)
{
    ck.step = 0xFFFFFFFFu;
    ck.left = 0.0f;
    ck.run = 1;
}

/* the audio interrupt, once a frame: 1 when a new step begins (ck.step is it) */
static inline int ck_frame(void)
{
    ck.left -= 1.0f;
    if (ck.left > 0.0f)
        return 0;
    ck.step++;
    ck.len = ck_step_len(ck.step);
    ck.left += ck.len;
    return 1;
}

/* where in the 16 bars the clock is: 0 .. 256 sixteenths */
static inline float ck_pos256(void) { return (float)(ck.step & 255u) + 1.0f - ck.left / ck.len; }
