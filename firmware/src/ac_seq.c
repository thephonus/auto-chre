/* SPDX-License-Identifier: GPL-3.0-only
 * auto-chre: the sequencer. Six tracks, each with its own length (1 .. 32 steps) and division (1/32 .. 1/4, triplets
 * too), so they drift against each other (5 against 7 against 16). A step has a velocity, ratchets (1 .. 4 hits) and
 * a lateness; the track adds its probability, random ratchets, random lateness (Microtiempo) and a Turing machine that
 * flips steps each time the track comes round. The clock is 96 ticks per quarter note, internal or MIDI.
 *
 * Runs in the audio code: seq_run() every 32 frames. The main loop changes the patterns (Euclid, the mutations, the
 * keys) and only sets flags here. */

static struct {
    volatile uint8_t playing, start_req, stop_req, rec;
    volatile int8_t stut_key;                /* a STUTTER key held: its length (0 .. 4), -1 none */
    volatile uint8_t fill, chaos;            /* the FILL and CHAOS keys held */
    uint32_t acc;                            /* the fraction of a tick, Q16 */
    uint32_t ticks;                          /* since the start */
    volatile uint32_t ext;                   /* MIDI clock ticks to run (4 per MIDI clock) */
    int32_t stut_left;                       /* an automatic stutter: ticks left (0 = none) */
    int8_t stut_auto;                        /* its length (0 .. 4) */
} sq = {0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0};

static struct {
    uint32_t in_step;                        /* ticks into the current step */
    volatile uint8_t step;                   /* the step that plays */
} tr[NTRK];

#define NPEND 12u
static struct { uint32_t due; uint8_t vel; } pend[NTRK][NPEND];     /* hits to come: due tick, velocity (0 = free) */

static inline uint32_t step_ticks(uint32_t k) { return DIV_TICKS[clampi(tp[k][T_DIV], 0, 5)]; }

static void pend_add(uint32_t k, uint32_t due, uint32_t vel)
{
    uint32_t i, j = 0, oldest = 0xFFFFFFFFu;
    for (i = 0; i < NPEND; i++) {
        if (!pend[k][i].vel) {
            j = i;
            break;
        }
        if (pend[k][i].due < oldest) {
            oldest = pend[k][i].due;
            j = i;
        }
    }
    pend[k][j].due = due;
    pend[k][j].vel = (uint8_t)clampi((int32_t)vel, 1, 127);
}

/* the track reaches step s: what it plays */
static void seq_step(uint32_t k, uint32_t s)
{
    const step_t *st = &pat[k][s];
    const int16_t *p = tp[k];
    uint32_t n, j, len = step_ticks(k), base = sq.ticks, late, vel;
    if (!st->on || p[T_MUTE])
        return;
    if (ac_rnd(100u) >= (uint32_t)p[T_PROB])
        return;
    n = st->ratch ? st->ratch : 1u;
    if (ac_rnd(100u) < (uint32_t)p[T_RATCH] || (sq.fill && ac_rnd(3u)))
        n = 2u + ac_rnd(3u);                                       /* 2 .. 4 */
    late = (uint32_t)st->micro * len / 100u;
    if (p[T_MICRO] || sq.chaos)
        late += ac_rnd((uint32_t)(sq.chaos ? 100 : p[T_MICRO]) * len / 150u + 1u);
    if ((s & 1u) && gp[G_SWING])                                   /* the swing: every other step later */
        late += (uint32_t)gp[G_SWING] * len / 200u;
    vel = st->vel ? st->vel : 100u;
    for (j = 0; j < n; j++)
        pend_add(k, base + late + j * len / n, vel - j * 10u * (n > 2u));
}

/* the track came round: the Turing machine flips some of its steps */
static void seq_wrap(uint32_t k)
{
    uint32_t i, t = (uint32_t)tp[k][T_TURING], len = (uint32_t)tp[k][T_LEN];
    if (!t || tp[k][T_LOCK])
        return;
    for (i = 0; i < len; i++)
        if (ac_rnd(400u) < t) {
            pat[k][i].on ^= 1u;
            if (pat[k][i].on)
                pat[k][i].vel = (uint8_t)(60u + ac_rnd(68u));
        }
}

static void seq_tick(void)
{
    uint32_t k, i;
    for (k = 0; k < NTRK; k++) {
        uint32_t len = (uint32_t)tp[k][T_LEN];
        if (tr[k].in_step == 0u)
            seq_step(k, tr[k].step);
        if (++tr[k].in_step >= step_ticks(k)) {
            tr[k].in_step = 0;
            tr[k].step = (uint8_t)((tr[k].step + 1u) % (len ? len : 1u));
            if (tr[k].step == 0u)
                seq_wrap(k);
        }
        for (i = 0; i < NPEND; i++)                                   /* the hits that are due */
            if (pend[k][i].vel && (int32_t)(sq.ticks - pend[k][i].due) >= 0) {
                voice_hit(k, pend[k][i].vel);
                pend[k][i].vel = 0;
            }
    }
    if (sq.ticks % 96u == 0u && !sq.stut_left && gp[G_STUTP] && ac_rnd(100u) < (uint32_t)gp[G_STUTP] / 3u) {
        sq.stut_auto = (int8_t)gp[G_STUTL];                            /* an automatic stutter on the beat */
        sq.stut_left = 48 + (int32_t)ac_rnd(2u) * 48;
    }
    if (sq.stut_left)
        sq.stut_left--;
    sq.ticks++;
}

static void seq_reset(void)
{
    uint32_t k, i;
    sq.ticks = 0;
    sq.acc = 0;
    sq.stut_left = 0;
    for (k = 0; k < NTRK; k++) {
        tr[k].in_step = 0;
        tr[k].step = 0;
        for (i = 0; i < NPEND; i++)
            pend[k][i].vel = 0;
    }
}

/* every SUB frames */
static void seq_run(void)
{
    if (sq.stop_req) {
        sq.stop_req = 0;
        sq.playing = 0;
        sq.stut_left = 0;
    }
    if (sq.start_req) {
        sq.start_req = 0;
        seq_reset();
        sq.playing = 1;
    }
    if (!sq.playing)
        return;
    if (gp[G_SYNC]) {                                                  /* MIDI clock */
        while (sq.ext) {
            sq.ext--;
            seq_tick();
        }
        return;
    }
    sq.acc += (uint32_t)((uint64_t)SUB * (uint32_t)gp[G_TEMPO] * PPQN * 65536u / (FS * 60u));
    while (sq.acc >= 65536u) {
        sq.acc -= 65536u;
        seq_tick();
    }
}

/* the stutter that plays now: a length 0 .. 4 (1/4 .. 1/64), -1 none */
static int32_t seq_stutter(void)
{
    if (sq.stut_key >= 0)
        return sq.stut_key;
    return sq.playing && sq.stut_left ? sq.stut_auto : -1;
}

/* the ticks per second, Q8 (the effects' tempo-synced times) */
static uint32_t seq_samples_per_tick_q8(void)
{
    return (uint32_t)((uint64_t)FS * 60u * 256u / ((uint32_t)gp[G_TEMPO] * PPQN));
}

/* MIDI realtime (gr_midi): clock, start, continue, stop */
static volatile uint32_t ac_clk_n, ac_clk_last_ms, ac_clk_bpm10, ac_rt_last;   /* the screen: is a clock coming? */
static uint32_t clk_mark_ms, clk_mark_n;
static void seq_clock_event(uint32_t status)
{
    ac_rt_last = status;
    if (status == 0xF8u) {
        ac_clk_n++;
        ac_clk_last_ms = fm1_ms;
        if (ac_clk_n - clk_mark_n >= 24u) {                            /* the tempo over a beat */
            uint32_t dt = fm1_ms - clk_mark_ms;
            if (dt > 100u && dt < 3000u) {
                ac_clk_bpm10 = 600000u / dt;
                if (gp[G_SYNC])
                    gp[G_TEMPO] = (int16_t)clampi((int32_t)((ac_clk_bpm10 + 5u) / 10u), GP[G_TEMPO].mn, GP[G_TEMPO].mx);
            }
            clk_mark_ms = fm1_ms;
            clk_mark_n = ac_clk_n;
        }
    }
    if (!gp[G_SYNC])
        return;
    if (status == 0xF8u && sq.playing)
        sq.ext += PPQN / 24u;
    else if (status == 0xFAu)
        sq.start_req = 1;
    else if (status == 0xFBu)
        sq.playing = 1;
    else if (status == 0xFCu)
        sq.stop_req = 1;
}

/* a live hit on track k (a key, MIDI): plays now, and with REC on while playing it goes into the pattern at the
 * nearest step */
static void seq_live_hit(uint32_t k, uint32_t vel)
{
    voice_hit(k, vel);
    if (sq.rec && sq.playing) {
        uint32_t len = (uint32_t)tp[k][T_LEN], s = tr[k].step;
        if (tr[k].in_step * 2u >= step_ticks(k))
            s = (s + 1u) % (len ? len : 1u);
        pat[k][s].on = 1;
        pat[k][s].vel = (uint8_t)vel;
        pat[k][s].ratch = 1;
        pat[k][s].micro = 0;
    }
}

/* ------------------------------------------------- main loop: patterns --- */
/* Euclid: `pulses` hits spread as evenly as they go over `len` steps, turned by `rot` */
static void pat_euclid(uint32_t k)
{
    uint32_t i, len = (uint32_t)tp[k][T_LEN], n = (uint32_t)clampi(tp[k][T_PULSES], 0, (int32_t)len), rot = (uint32_t)tp[k][T_ROT];
    for (i = 0; i < NSTEP; i++) {
        uint32_t j = (i + len - rot % (len ? len : 1u)) % (len ? len : 1u);
        pat[k][i].on = (uint8_t)(i < len && n && (j * n) % len < n);
        pat[k][i].vel = (uint8_t)(j == 0u ? 120u : 96u);
        pat[k][i].ratch = 1;
        pat[k][i].micro = 0;
    }
}

/* the undo of the last mutation */
static step_t undo_pat[NTRK][NSTEP];
static int16_t undo_tp[NTRK][T_COUNT];
static uint8_t undo_ok;
static void undo_save(void)
{
    memcpy(undo_pat, pat, sizeof pat);
    memcpy(undo_tp, tp, sizeof tp);
    undo_ok = 1;
}
static int undo_restore(void)
{
    if (!undo_ok)
        return -1;
    memcpy(pat, undo_pat, sizeof pat);
    memcpy(tp, undo_tp, sizeof tp);
    undo_ok = 0;
    return 0;
}

/* MUTAR RITMO: the unlocked tracks change by `a` (0 .. 100): steps flip, ratchets and lateness appear, velocities move;
 * a big mutation also changes the lengths (new polymeters) and the divisions */
static void mutate_rhythm(uint32_t a)
{
    static const uint8_t LENS[] = {5, 6, 7, 9, 10, 11, 12, 13, 15, 16, 16, 24};
    uint32_t k, i;
    undo_save();
    for (k = 0; k < NTRK; k++) {
        uint32_t len;
        if (tp[k][T_LOCK])
            continue;
        if (a > 55u && ui_rnd(200u) < a - 55u)
            tp[k][T_LEN] = LENS[ui_rnd(sizeof LENS)];
        if (a > 70u && ui_rnd(300u) < a - 70u)
            tp[k][T_DIV] = (int16_t)clampi(tp[k][T_DIV] + (ui_rnd(2u) ? 1 : -1), 0, 4);
        len = (uint32_t)tp[k][T_LEN];
        for (i = 0; i < len; i++) {
            step_t *st = &pat[k][i];
            if (ui_rnd(250u) < a)
                st->on ^= 1u;
            if (ui_rnd(500u) < a)
                st->ratch = (uint8_t)(ui_rnd(3u) ? 1u : 2u + ui_rnd(3u));
            if (ui_rnd(400u) < a)
                st->micro = (uint8_t)ui_rnd(a / 2u + 1u);
            if (ui_rnd(300u) < a)
                st->vel = (uint8_t)(50u + ui_rnd(78u));
        }
    }
}

/* MUTAR SONIDO: the unlocked tracks' engine settings move by up to +-a/2; a big one may change an engine */
static void mutate_sound(uint32_t a)
{
    uint32_t k, i;
    undo_save();
    for (k = 0; k < NTRK; k++) {
        if (tp[k][T_LOCK])
            continue;
        for (i = 0; i < 6u; i++)
            tp[k][T_E1 + i] = (int16_t)clampi(tp[k][T_E1 + i] + (int32_t)ui_rnd(a + 1u) - (int32_t)a / 2, 0, 100);
        if (ui_rnd(100u) < a / 3u)
            tp[k][T_BITS] = (int16_t)(ui_rnd(3u) ? 16 : 4 + (int32_t)ui_rnd(9u));
        if (ui_rnd(100u) < a / 3u)
            tp[k][T_DECIM] = (int16_t)(ui_rnd(3u) ? 1 : 2 + (int32_t)ui_rnd(10u));
        if (a > 80u && ui_rnd(100u) < (a - 80u) * 2u)
            tp[k][T_TYPE] = (int16_t)ui_rnd(EN_COUNT);
    }
}
