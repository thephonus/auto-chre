/* SPDX-License-Identifier: GPL-3.0-only
 * auto-chre on the computer: the voices, the sequencer, the mutations and the effects, rendered and checked, and every
 * screen drawn.
 *   build/host_ac build/demo.wav build/ui
 * Checks: every engine sounds and dies away; the polymeters run their own lengths; mutations change the pattern and
 * undo brings it back; 70 s of the machine playing (with mutations every 8 s, a stutter, chaos) neither clips hard nor
 * goes silent; the time a block takes on this computer. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define memset gh_memset
#define memcpy gh_memcpy
#define memcmp gh_memcmp
static volatile uint32_t fm1_ms = 5000;
static uint16_t fb[240 * 240];
#define FELUCCA_VERSION "auto-chre 0.2"
static void fm1_wdt_feed(void) {}
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240 && y + j < 240) {
                uint16_t v = px[j * w + i];
                fb[(y + j) * 240 + x + i] = (uint16_t)((v >> 8) | (v << 8));
            }
}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240 && y + j < 240) fb[(y + j) * 240 + x + i] = c;
}
#include "libc.c"
#include "gfx.c"
#include "ac_core.h"
#include "ac_voice.c"
#include "ac_seq.c"
#include "ac_fx.c"
/* what the screens need from the rest of the firmware */
static struct { uint32_t notes, buttons; } fm1_in;
static uint32_t audio_cpu_q8 = 90;
static uint8_t flash_ok;
#define ST_PAYLOAD_MAX 3840u
#define NPRESET_OBJ 16u
enum { OBJ_SETTINGS, OBJ_PRESET0 };
static int st_load(uint32_t o, void *d, uint32_t n) { (void)o; (void)d; (void)n; return -1; }
static int st_save(uint32_t o, const void *d, uint32_t n) { (void)o; (void)d; (void)n; return -1; }
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
#undef FELUCCA_FLASH
#define FELUCCA_FLASH 0
#include "ac_state.c"
static volatile uint8_t ac_kb_hold;
#include "ac_ui.c"
static int32_t batt_level(void) { return 2; }
static int32_t batt_pct(void) { return 71; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } else { printf("ok   " __VA_ARGS__); printf("\n"); } } while (0)

static void block(int32_t *l, int32_t *r)
{
    int32_t snd[BLK], rsnd[BLK];
    uint32_t i;
    for (i = 0; i < BLK; i++)
        l[i] = r[i] = snd[i] = rsnd[i] = 0;
    for (i = 0; i < BLK; i += SUB) {
        seq_run();
        voices_render(l + i, r + i, snd + i, rsnd + i, SUB);
    }
    fx_render(l, r, snd, rsnd, BLK);
}

static void save_ppm(const char *dir, const char *name)
{
    char path[256];
    FILE *f;
    int i;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240 * 240; i++) {
        uint16_t v = fb[i];
        unsigned char rgb[3] = {(unsigned char)(((v >> 11) & 31) * 255 / 31), (unsigned char)(((v >> 5) & 63) * 255 / 63), (unsigned char)((v & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *wavp = argc > 1 ? argv[1] : 0, *uidir = argc > 2 ? argv[2] : 0;
    uint32_t k, b, e;
    reverb_init();
    state_defaults();

    /* every engine: a hit alone sounds and dies away */
    for (e = 0; e < EN_COUNT; e++) {
        int32_t l[BLK], r[BLK], snd[BLK], rsnd[BLK];
        double early = 0, late = 0;
        uint32_t i, peak = 0;
        memset(vo, 0, sizeof vo);
        tp[0][T_TYPE] = (int16_t)e;
        if (e == EN_GLITCH)                                       /* something to take a slice of */
            for (i = 0; i < GL_N; i++)
                gl_buf[i] = (int16_t)(8000 * sin(i * 0.05));
        voice_hit(0, 120);
        for (b = 0; b < 3u * 345u; b++) {
            memset(l, 0, sizeof l);
            memset(r, 0, sizeof r);
            memset(snd, 0, sizeof snd);
            memset(rsnd, 0, sizeof rsnd);
            voices_render(l, r, snd, rsnd, BLK);
            for (i = 0; i < BLK; i++) {
                double v = (double)l[i] * l[i];
                if ((uint32_t)abs(l[i]) > peak)
                    peak = (uint32_t)abs(l[i]);
                if (b < 30u)
                    early += v;
                else if (b > 2u * 345u)
                    late += v;
            }
        }
        printf("     %-6s peak %5u  rms first 90 ms %6.0f  after 2 s %5.0f\n", ENGINE_NAMES[e], peak,
               sqrt(early / (30 * BLK)), sqrt(late / (345 * BLK)));
        CHECK(sqrt(early / (30 * BLK)) > 200, "%s sounds", ENGINE_NAMES[e]);
        CHECK(sqrt(late / (345 * BLK)) < 20 || e == EN_GLITCH, "%s dies away", ENGINE_NAMES[e]);
        CHECK(peak < 40000, "%s stays in range (peak %u)", ENGINE_NAMES[e], peak);
    }
    state_defaults();
    memset(vo, 0, sizeof vo);

    /* the notes: a tonal voice with Notas Azar in D Mixolydian-ish (D, Mayor) only plays notes of that scale */
    {
        uint32_t m, h, bad = 0, seen = 0;
        tp[0][T_TYPE] = EN_CHIP;
        tp[0][T_E4] = 0;                                          /* no arpeggio */
        tp[0][T_VAR] = 60;
        tp[0][T_NOTES] = 1;
        tp[0][T_RANGE] = 2;
        gp[G_ROOT] = 2;
        for (m = 1; m <= NMODES; m++) {
            tp[0][T_SCALE] = (int16_t)m;
            for (h = 0; h < 40u; h++) {
                double hz, note;
                int rel, k, in = 0;
                voice_hit(0, 100);
                hz = (double)vo[0].dph[0] * FS / 4294967296.0;
                note = 12.0 * log2(hz / 440.0) + 69.0;
                rel = (((int)lround(note) - 2) % 12 + 12) % 12;
                for (k = 0; k < MODE_N[m - 1]; k++)
                    if (MODE_DEG[m - 1][k] == rel)
                        in = 1;
                if (!in || fabs(note - lround(note)) > 0.05)
                    bad++;
                seen |= 1u << rel;
            }
        }
        CHECK(bad == 0, "Notas: every note in its scale (%u out)", bad);
        CHECK(__builtin_popcount(seen) >= 7, "Notas: the notes move");
        state_defaults();
        memset(vo, 0, sizeof vo);
    }
    /* the reverb: a hit sent to it still rings half a second later */
    {
        int32_t l[BLK], r[BLK];
        double tail = 0;
        uint32_t i;
        gp[G_REVMIX] = 80;
        gp[G_REVDECAY] = 80;
        tp[3][T_RSEND] = 100;
        voice_hit(3, 120);
        for (b = 0; b < 300u; b++) {
            block(l, r);
            if (b > 170u)
                for (i = 0; i < BLK; i++)
                    tail += (double)l[i] * l[i];
        }
        CHECK(sqrt(tail / (129.0 * BLK)) > 30, "the reverb rings on (rms %.0f)", sqrt(tail / (129.0 * BLK)));
        state_defaults();
        memset(vo, 0, sizeof vo);
    }

    /* MIDI clock: start, 24 clocks a beat at 120 BPM */
    {
        uint32_t c, h0;
        gp[G_SYNC] = 1;
        seq_clock_event(0xFAu);
        for (c = 0; c < 24u * 8u; c++) {
            fm1_ms += (c % 24u) == 23u ? 500u - 23u * 20u : 20u;   /* 500 ms a beat */
            seq_clock_event(0xF8u);
            seq_run();
        }
        h0 = ac_hits[0] + ac_hits[1] + ac_hits[2];
        CHECK(sq.playing && h0 > 0, "MIDI start and clock drive the sequencer");
        CHECK(ac_clk_bpm10 >= 1190u && ac_clk_bpm10 <= 1210u && gp[G_TEMPO] == 120, "the clock's tempo is read (%u)", ac_clk_bpm10);
        state_defaults();
        sq.playing = 0;
        memset(vo, 0, sizeof vo);
    }
    /* the polymeters: in 4 bars, a 7-step track at 1/16 comes round 64 / 7 times */
    {
        uint32_t wraps = 0, prev = 0;
        sq.start_req = 1;
        seq_run();
        for (b = 0; b < 4u * 4u * 96u; b++) {
            seq_tick();
            if (tr[5].step < prev)
                wraps++;
            prev = tr[5].step;
        }
        CHECK(wraps == 9u, "track 6 (7 steps) came round %u times in 4 bars", wraps);
        sq.stop_req = 1;
        seq_run();
    }

    /* mutations and undo */
    {
        step_t before[NTRK][NSTEP];
        uint32_t diff = 0, i;
        memcpy(before, pat, sizeof pat);
        tp[2][T_LOCK] = 1;
        mutate_rhythm(80);
        for (k = 0; k < NTRK; k++)
            for (i = 0; i < NSTEP; i++)
                diff += memcmp(&before[k][i], &pat[k][i], sizeof(step_t)) != 0;
        CHECK(diff > 10u, "MUTAR RITMO changed %u steps", diff);
        CHECK(memcmp(before[2], pat[2], sizeof pat[2]) == 0, "a locked track does not change");
        CHECK(undo_restore() == 0 && memcmp(before, pat, sizeof pat) == 0, "undo brings the pattern back");
        tp[2][T_LOCK] = 0;
    }

    /* 70 s of the machine */
    {
        static int16_t out[2u * 70u * FS];
        uint32_t n = 0, blocks = 70u * FS / BLK, silent = 0, i, clip = 0;
        double sum = 0, worst = 0;
        gp[G_STUTP] = 25;
        sq.start_req = 1;
        for (b = 0; b < blocks; b++) {
            int32_t l[BLK], r[BLK];
            uint32_t sec = b * BLK / FS;
            clock_t t0;
            double us, bs = 0;
            if (b % (8u * 345u) == 8u * 344u) {
                mutate_rhythm((uint32_t)gp[G_MUTR]);
                mutate_sound((uint32_t)gp[G_MUTS]);
            }
            sq.stut_key = (int8_t)(sec >= 30u && sec < 32u ? 3 : -1);
            sq.chaos = (uint8_t)(sec >= 40u && sec < 44u);
            sq.fill = (uint8_t)(sec >= 50u && sec < 52u);
            t0 = clock();
            block(l, r);
            us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC;
            sum += us;
            if (us > worst)
                worst = us;
            for (i = 0; i < BLK; i++) {
                int32_t a = sat16(l[i]), c = sat16(r[i]);
                if (abs(l[i]) > 32767 || abs(r[i]) > 32767)
                    clip++;
                bs += (double)a * a;
                out[n++] = (int16_t)a;
                out[n++] = (int16_t)c;
            }
            if (sqrt(bs / BLK) < 30)
                silent++;
        }
        printf("     a block (2.9 ms of sound) takes %.0f us on average here, %.0f at worst\n", sum / blocks, worst);
        CHECK(silent < blocks / 3u, "it keeps playing (%u of %u blocks near silence)", silent, blocks);
        CHECK(clip < blocks * BLK / 200u, "little hard clipping (%u samples)", clip);
        if (wavp) {
            FILE *f = fopen(wavp, "wb");
            uint32_t bytes = n * 2u, hdr[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, FS, FS * 4, 0x00100004, 0x61746164, bytes};
            fwrite(hdr, 4, 11, f);
            fwrite(out, 2, n, f);
            fclose(f);
        }
    }

    /* the screens */
    if (uidir) {
        uint32_t sc, sub, i;
        ui_theme_init();
        dial_init();
        presets_mask = 0x0413;
        for (sc = 0; sc < SC_COUNT; sc++)
            for (sub = 0; sub < ui_npages(sc); sub++) {
                char name[32];
                ui.page = (uint8_t)sc;
                ui.sub[sc] = (uint8_t)sub;
                ui.trk = (uint8_t)(sc % NTRK);
                ui.force = 1;
                ui.turned[sub & 3u] = fm1_ms;
                for (i = 0; i < 40u; i++) {
                    int32_t l[BLK], r[BLK];
                    block(l, r);
                }
                fm1_ms += 30;
                ui_draw();
                snprintf(name, sizeof name, "s%u_%u", sc, sub);
                save_ppm(uidir, name);
            }
    }
    printf(fails ? "%d FAILURES\n" : "all good\n", fails);
    return fails != 0;
}
