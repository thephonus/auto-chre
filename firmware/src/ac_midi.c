/* SPDX-License-Identifier: GPL-3.0-only
 * auto-chre: MIDI in (USB and the TRS jack share one ring, usb.c) and the keys, drained by the audio code once a
 * block, and the main loop's commands.
 *   MIDI: notes 36 .. 41 on Canal MIDI play tracks 1 .. 6 (any octave: note % 6 from 36); the clock, start and stop
 *   drive the sequencer when Reloj is MIDI.
 *   The keys (27, F3 .. G5) in the audio code: 1 .. 6 play the tracks (and write them with REC), 13 .. 17 hold a
 *   STUTTER (1/4 .. 1/64), 18 CAOS, 19 LLENAR. The others are the main loop's (ac_input.c). */
#define KB_KEYS 27u
static volatile uint32_t ac_midi_activity;
static volatile uint8_t ac_kb_hold;                  /* no key does anything (boot) */
static uint32_t kb_prev_a;

static void ac_midi_block(void)
{
    if (midi_in_overflow) {
        mi_r = mi_w;
        RING_PUBLISH();
        midi_in_overflow = 0;
    }
    while (mi_r != mi_w) {
        uint32_t at = mi_r % MQ, pkt = midi_in_q[at], status = (pkt >> 8) & 0xFFu;
        uint32_t d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        mi_r++;
        if (status >= 0xF8u) {
            seq_clock_event(status);
            continue;
        }
        if ((status & 0x0Fu) != (uint32_t)gp[G_MIDICH])
            continue;
        if ((status & 0xF0u) == 0x90u && d2 && d1 >= 24u) {
            seq_live_hit((d1 - 36u + 60u) % NTRK, d2);
            ac_midi_activity++;
        }
    }
}

static void ac_keys_block(void)
{
    uint32_t cur = fm1_in.notes, ch = cur ^ kb_prev_a, k;
    int32_t stut = -1;
    if (ac_kb_hold) {
        kb_prev_a = cur;
        return;
    }
    for (k = 0; k < NTRK; k++)
        if (((ch & cur) >> k) & 1u)
            seq_live_hit(k, 110u);
    for (k = 0; k < 5u; k++)                          /* the shortest held stutter wins */
        if ((cur >> (12u + k)) & 1u)
            stut = (int32_t)k;
    sq.stut_key = (int8_t)stut;
    sq.chaos = (uint8_t)((cur >> 17) & 1u);
    sq.fill = (uint8_t)((cur >> 18) & 1u);
    kb_prev_a = cur;
}
