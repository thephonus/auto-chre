/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* I2S output (ALNK0 -> external codec) and the audio ISR: per half buffer (BLK = 128 frames, 2.9 ms): MIDI and the
 * keys, the sequencer and the drum voices, the effects, the master level, USB audio, 24-bit stereo. Registers: hal/fm1_audio.h. */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define OUT_SHIFT 7               /* Q15 -> 24-bit, -6 dBFS ceiling */
#define HALF_US (HALF_FRAMES * 1000000u / FS)

static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

/* diagnostics, kept across resets and UBOOT entry: read with `fm1t memr` */
#define DBG_MAGIC 0x44424731u                       /* "DBG1" */
struct felucca_dbg {
    uint32_t magic, halves, max_us, nested, in_audio, late, timer_irqs, ui_frames;
    uint32_t last_us, cpu_q8, boots;
    uint32_t stage, page, home;           /* where the main loop is (breadcrumbs) */
    uint32_t prev_stage, prev_page, prev_home, prev_rst, prev_frames;   /* as found at boot */
} felucca_dbg __attribute__((section(".noinit")));
static volatile uint32_t audio_halves, audio_max_us;
static volatile uint32_t t5_nested_ticks;              /* TIMER4 ticks TIMER5 spent nested in this ISR (main.c) */
static uint32_t audio_cpu_q8, audio_cpu_rem;           /* the load, 256 = 100 % of a half */

static void audio_block(int32_t *out)                   /* one block, interleaved Q15 -> 24 bit */
{
    int32_t l[BLK], r[BLK], snd[BLK], rsnd[BLK], mg = gr_master_q12;
    uint32_t i;
    ac_midi_block();
    ac_keys_block();
    for (i = 0; i < BLK; i++)
        l[i] = r[i] = snd[i] = rsnd[i] = 0;
    for (i = 0; i < BLK; i += SUB) {                    /* the sequencer every 32 frames, the voices between */
        seq_run();
        voices_render(l + i, r + i, snd + i, rsnd + i, SUB);
    }
    fx_render(l, r, snd, rsnd, BLK);
    for (i = 0; i < BLK; i++) {
        out[2u * i] = (sat16(l[i]) * mg) >> 12;
        out[2u * i + 1u] = (sat16(r[i]) * mg) >> 12;
    }
#if FELUCCA_UAC
    uac_tap(out, BLK);                                  /* the USB audio input: the same master output */
#endif
    for (i = 0; i < 2u * BLK; i++)
        out[i] *= 1 << OUT_SHIFT;
}

void fm1_alnk0_irq(void)                       /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p;
    uint32_t t0;
    felucca_dbg.in_audio = 1;                   /* first: TIMER5 nests from here on (main.c) */
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    t5_nested_ticks = 0;
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), us, all;
#if FELUCCA_UAC
        uac_render_start();
#endif
        audio_block(&abuf[half * HALF_WORDS]);
        fm1_audio_ack_half();
        audio_halves++;
        all = fm1_ticks() - t0;
        us = (all - t5_nested_ticks) / FM1_TICKS_PER_US;     /* the render alone */
        if (us > audio_max_us)
            audio_max_us = us;
        {   /* the grain cap follows the load, the render plus TIMER5 nested in it */
            uint32_t q8 = (all / FM1_TICKS_PER_US) * 256u / HALF_US;
            audio_cpu_q8 = (audio_cpu_q8 * 15u + q8 + 8u) / 16u;
        }
        if (fm1_audio_free_half() != half)
            felucca_dbg.late++;                         /* the DMA moved on while we rendered */
        felucca_dbg.halves++;
        felucca_dbg.last_us = us;
        if (us > felucca_dbg.max_us)
            felucca_dbg.max_us = us;
        felucca_dbg.cpu_q8 = audio_cpu_q8;
    }
    felucca_dbg.in_audio = 0;
}
extern void isr_alnk0(void);

static void audio_init(void)                   /* codec and ALNK0 bring-up (fm1_audio.h) */
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}
