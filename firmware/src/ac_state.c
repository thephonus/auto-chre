/* SPDX-License-Identifier: GPL-3.0-only
 * auto-chre: the state in flash: the current state (restored at power-on, saved a few seconds after the last change
 * while the sequencer is stopped and nothing sounds: a flash write stops the audio for a moment) and 16 presets.
 * A state is the shared values, the six tracks' values and their patterns. */
#define ST_VER 0xC1u                             /* auto-chre 0.1 */
#define BLOB_N (2u + G_COUNT * 2u + NTRK * T_COUNT * 2u + NTRK * NSTEP * 4u)
_Static_assert(BLOB_N <= ST_PAYLOAD_MAX, "a state must fit a storage object");
static uint8_t blob[BLOB_N], pbuf[BLOB_N];
static uint16_t presets_mask;

static uint32_t state_pack(uint8_t *b)
{
    uint32_t k = 0, i, j;
    b[k++] = ST_VER;
    b[k++] = 1;
    for (i = 0; i < G_COUNT; i++) {
        b[k++] = (uint8_t)gp[i];
        b[k++] = (uint8_t)((uint16_t)gp[i] >> 8);
    }
    for (j = 0; j < NTRK; j++)
        for (i = 0; i < T_COUNT; i++) {
            b[k++] = (uint8_t)tp[j][i];
            b[k++] = (uint8_t)((uint16_t)tp[j][i] >> 8);
        }
    for (j = 0; j < NTRK; j++)
        for (i = 0; i < NSTEP; i++) {
            b[k++] = pat[j][i].on;
            b[k++] = pat[j][i].vel;
            b[k++] = pat[j][i].ratch;
            b[k++] = pat[j][i].micro;
        }
    return k;
}

static int16_t clamp_p(const ac_pinfo_t *p, int32_t v) { return (int16_t)clampi(v, p->mn, p->mx); }

static int state_unpack(const uint8_t *b, uint32_t n)
{
    uint32_t k = 2, i, j;
    if (n != BLOB_N || b[0] != ST_VER)
        return -1;
    for (i = 0; i < G_COUNT; i++, k += 2u)
        gp[i] = clamp_p(&GP[i], (int16_t)(b[k] | b[k + 1] << 8));
    for (j = 0; j < NTRK; j++)
        for (i = 0; i < T_COUNT; i++, k += 2u)
            tp[j][i] = clamp_p(&TP[i], (int16_t)(b[k] | b[k + 1] << 8));
    for (j = 0; j < NTRK; j++)
        for (i = 0; i < NSTEP; i++, k += 4u) {
            pat[j][i].on = b[k] & 1u;
            pat[j][i].vel = (uint8_t)clampi(b[k + 1], 1, 127);
            pat[j][i].ratch = (uint8_t)clampi(b[k + 2], 1, 4);
            pat[j][i].micro = (uint8_t)clampi(b[k + 3], 0, 75);
        }
    return 0;
}

static void state_defaults(void)
{
    uint32_t k, i;
    for (i = 0; i < G_COUNT; i++)
        gp[i] = GP[i].def;
    for (k = 0; k < NTRK; k++) {
        for (i = 0; i < T_COUNT; i++)
            tp[k][i] = TP[i].def;
        tp[k][T_TYPE] = TRK_DEF[k][0];
        for (i = 0; i < 6u; i++)
            tp[k][T_E1 + i] = TRK_DEF[k][1 + i];
        tp[k][T_PULSES] = TRK_DEF[k][7];
        tp[k][T_LEN] = TRK_LEN[k];
        pat_euclid(k);
    }
    tp[0][T_SEND] = 5;
    tp[3][T_DIV] = 1;                             /* the clicks in triplets */
    pat_euclid(3);
    tp[4][T_PROB] = 70;
    tp[5][T_TURING] = 20;
    tp[1][T_RATCH] = 15;
}

/* --------------------------------------------------------- presets --- */
static void presets_scan(void)
{
    uint32_t o, mask = 0;
    for (o = 0; o < NPRESET_OBJ; o++)
        if (st_load(OBJ_PRESET0 + o, pbuf, sizeof pbuf) == (int)BLOB_N && pbuf[0] == ST_VER)
            mask |= 1u << o;
    presets_mask = (uint16_t)mask;
}

static int preset_load(uint32_t slot)
{
    int n = flash_ok ? st_load(OBJ_PRESET0 + slot, pbuf, sizeof pbuf) : -1;
    if (n < 2 || state_unpack(pbuf, (uint32_t)n))
        return -1;
    return 0;
}

static int preset_save(uint32_t slot)
{
    uint32_t len;
    if (!flash_ok)
        return -1;
    len = state_pack(blob);
    if (st_save(OBJ_PRESET0 + slot, blob, len))
        return -1;
    presets_mask |= (uint16_t)(1u << slot);
    return 0;
}

/* ------------------------------------------------ autosave and boot --- */
static uint32_t as_hash, as_dirty_ms, as_check_ms, as_quiet_ms;

static uint32_t blob_hash(const uint8_t *b, uint32_t n)
{
    uint32_t h = 2166136261u, i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static void state_boot(void)
{
    int n = flash_ok ? st_load(OBJ_SETTINGS, blob, sizeof blob) : -1;
    state_defaults();
    if (n <= 0 || state_unpack(blob, (uint32_t)n))
        state_defaults();
    as_hash = blob_hash(blob, state_pack(blob));
    if (flash_ok)
        presets_scan();
}

/* a flash write stops the audio for a moment: only while stopped and silent */
static void autosave_poll(void)
{
    uint32_t now = fm1_ms, h, len, k, busy = sq.playing || fm1_in.notes != 0u;
    if ((uint32_t)(now - as_check_ms) < 400u)
        return;
    as_check_ms = now;
    for (k = 0; k < NTRK; k++)
        busy |= vo[k].on;
    if (busy)
        as_quiet_ms = now ? now : 1u;
    len = state_pack(blob);
    h = blob_hash(blob, len);
    if (h == as_hash) {
        as_dirty_ms = 0;
        return;
    }
    if (!as_dirty_ms)
        as_dirty_ms = now ? now : 1u;
    if ((uint32_t)(now - as_dirty_ms) < 3000u || busy || (uint32_t)(now - as_quiet_ms) < 2000u)
        return;
    if (!flash_ok || st_save(OBJ_SETTINGS, blob, len) == 0)
        as_hash = h;
    as_dirty_ms = 0;
}

static void persist_boot(void)
{
#if FELUCCA_FLASH
    uint32_t f = irq_save();
    flash_ok = FL_JEDEC_OK(FL_FAR(fl_jedec_ram)());
    irq_restore(f);
    if (flash_ok)
        fl_plain_window_init();
#endif
}
