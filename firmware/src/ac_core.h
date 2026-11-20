/* SPDX-License-Identifier: GPL-3.0-only
 * auto-chre: sizes, the parameters, fixed-point helpers. */
#include <stdint.h>
#include "ac_gen.h"

#define FS 44100u                /* the output rate */
#define BLK 128                  /* frames per render block (= HALF_FRAMES) */
#define HALF_FRAMES BLK
#define SUB 32                   /* the sequencer runs every 32 frames (0.73 ms) */
#define NTRK 6                   /* drum tracks */
#define NSTEP 32                 /* steps per track pattern at most */
#define NPRESET 16
#define PPQN 96                  /* the sequencer's ticks per quarter note */

/* ------------------------------------------------------------- math --- */
static inline int32_t mulq15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static inline int32_t mulq16(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t absi(int32_t v) { return v < 0 ? -v : v; }
static inline int32_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* 2^(x / 65536), 0 <= x < 15 octaves: Q16 */
static uint32_t exp2_q16(uint32_t x)
{
    uint32_t ip = x >> 16, idx = (x >> 8) & 255u, f = x & 255u;
    uint32_t v = AC_EXP2[idx] + (((AC_EXP2[idx + 1u] - AC_EXP2[idx]) * f) >> 8);
    return v << ip;
}
/* 2^(x / 65536) for a signed x (-16 .. 15 octaves): Q16 */
static uint32_t exp2s_q16(int32_t x)
{
    if (x >= 0)
        return exp2_q16((uint32_t)x);
    if (x < -(16 << 16))
        return 0;
    {
        uint32_t ip = (uint32_t)(-x + 65535) >> 16, v = exp2_q16((uint32_t)(x + (int32_t)(ip << 16)));
        return v >> ip;
    }
}
/* sine of a 32-bit phase, Q15 */
static inline int32_t sin32(uint32_t ph)
{
    uint32_t i = ph >> 22;
    int32_t a = AC_SINE[i], b = AC_SINE[i + 1u];
    return a + (((b - a) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
}
/* the phase step of hz x 16 (Q4) at FS */
static inline uint32_t ph_of_hz16(uint32_t hz16) { return (uint32_t)(((uint64_t)hz16 << 28) / FS); }

static uint32_t ac_rng = 0x6D2B79F5u;               /* the audio code's */
static inline uint32_t ac_rand(void)
{
    ac_rng ^= ac_rng << 13;
    ac_rng ^= ac_rng >> 17;
    ac_rng ^= ac_rng << 5;
    return ac_rng;
}
static inline int32_t ac_rand_bi(void) { return (int32_t)(ac_rand() >> 16) - 32768; }     /* -32768 .. 32767 */
static inline uint32_t ac_rnd(uint32_t n) { return n ? (uint32_t)(((uint64_t)(ac_rand() >> 8) * n) >> 24) : 0u; }
static uint32_t ui_rng = 0x2545F491u;               /* the main loop's (mutations) */
static inline uint32_t ui_rand(void)
{
    ui_rng ^= ui_rng << 13;
    ui_rng ^= ui_rng >> 17;
    ui_rng ^= ui_rng << 5;
    return ui_rng;
}
static inline uint32_t ui_rnd(uint32_t n) { return n ? (uint32_t)(((uint64_t)(ui_rand() >> 8) * n) >> 24) : 0u; }

/* -------------------------------------------------------- parameters --- */
/* A parameter is a whole number in [mn, mx]; the knobs move it by 1 (faster when turned fast). */
enum { U_NUM, U_PCT, U_CHOICE, U_SEMI, U_BIP, U_BPM, U_CH };
typedef struct {
    const char *name;
    int16_t mn, mx, def;
    uint8_t unit;
    const char *const *labels;
} ac_pinfo_t;

/* the engines of a track */
enum { EN_KICK, EN_METAL, EN_NOISE, EN_CLICK, EN_GLITCH, EN_FM, EN_PLUCK, EN_MODAL, EN_ACID, EN_CHIP, EN_COUNT };
static const char *const ENGINE_NAMES[EN_COUNT] = {"Bombo", "Metal", "Ruido", "Clic", "Glitch", "FM", "Cuerda", "Modal",
                                                   "Ácido", "Chip"};
/* the engines with a real pitch: their Tono is the octave of the root (Afinar) and they play in the scale */
#define EN_TONAL(e) ((e) >= EN_PLUCK)
/* what the six engine settings E1 .. E6 are called on each engine (all 0 .. 100) */
static const char *const ENGINE_P[EN_COUNT][6] = {
    {"Tono", "Ca\355da", "Barrido", "Vel. barr.", "Clic", "Saturar"},
    {"Tono", "Ca\355da", "Inarmon.", "Anillo", "Brillo", "Ruido"},
    {"Corte", "Reso", "Ca\355da", "Tipo", "Barrido", "Granular"},
    {"Tono", "Longitud", "Forma", "Ca\355da tono", "Repetir", "Separac."},
    {"Tono", "Trozo", "Atr\341s", "Reverso", "Repetir", "Forma"},
    {"Tono", "Ratio", "\315ndice", "Ca\355da \355nd.", "Ca\355da", "Realim."},
    {"Octava", "Ca\355da", "Brillo", "Excitar", "Posici\363n", "Saturar"},
    {"Octava", "Ca\355da", "Material", "Dureza", "Golpe", "Inarmon."},
    {"Octava", "Ca\355da", "Corte", "Reso", "Env. filtro", "Onda"},
    {"Octava", "Ca\355da", "Pulso", "Arpegio", "Desliz", "Vibrato"},
};
static const char *const NOTE_MODES[] = {"Fija", "Azar", "Sube", "Baja", "P\351ndulo", "Turing"};
static const char *const DIV_NAMES[] = {"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4"};
static const uint8_t DIV_TICKS[] = {12, 16, 24, 32, 48, 96};
static const char *const STUT_NAMES[] = {"1/4", "1/8", "1/16", "1/32", "1/64"};
static const uint8_t STUT_TICKS[] = {96, 48, 24, 12, 6};
static const char *const NOSI[] = {"No", "S\355"};
static const char *const SCALE_NAMES[] = {"Global", "Crom\341tica", "Mayor", "Menor", "D\363rico", "Frigio", "Penta men.",
                                          "Hirajoshi", "Tonos ent.", "Mixolidio", "Lidio"};
static const char *const SYNC_NAMES[] = {"Interno", "MIDI"};
static const char *const KEY_NAMES[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
#define NMODES 10
static const char *const MODE_NAMES[NMODES] = {"Crom\341tica", "Mayor", "Menor", "D\363rico", "Frigio", "Penta men.",
                                               "Hirajoshi", "Tonos ent.", "Mixolidio", "Lidio"};
static const uint8_t MODE_N[NMODES] = {12, 7, 7, 7, 7, 5, 5, 6, 7, 7};
static const uint8_t MODE_DEG[NMODES][12] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10},
                                             {0, 2, 3, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10}, {0, 3, 5, 7, 10}, {0, 2, 3, 7, 8},
                                             {0, 2, 4, 6, 8, 10}, {0, 2, 4, 5, 7, 9, 10}, {0, 2, 4, 6, 7, 9, 11}};

/* a track's parameters */
enum { T_TYPE, T_E1, T_E2, T_E3, T_E4, T_E5, T_E6, T_VAR, T_LEVEL, T_PAN, T_SEND, T_RSEND, T_TUNE, T_NOTES, T_RANGE,
       T_SCALE, T_BITS, T_DECIM,
       T_LEN, T_DIV, T_PULSES, T_ROT, T_PROB, T_RATCH, T_MICRO, T_TURING, T_MUTE, T_LOCK, T_COUNT };
static const ac_pinfo_t TP[T_COUNT] = {
    {"Motor", 0, EN_COUNT - 1, 0, U_CHOICE, ENGINE_NAMES},
    {"E1", 0, 100, 50, U_NUM, 0}, {"E2", 0, 100, 50, U_NUM, 0}, {"E3", 0, 100, 50, U_NUM, 0},
    {"E4", 0, 100, 50, U_NUM, 0}, {"E5", 0, 100, 50, U_NUM, 0}, {"E6", 0, 100, 50, U_NUM, 0},
    {"Variaci\363n", 0, 100, 20, U_PCT, 0},
    {"Nivel", 0, 100, 80, U_PCT, 0},
    {"Panorama", -50, 50, 0, U_BIP, 0},
    {"Delay", 0, 100, 15, U_PCT, 0},
    {"Reverb", 0, 100, 10, U_PCT, 0},
    {"Afinar", -24, 24, 0, U_SEMI, 0},
    {"Notas", 0, 5, 0, U_CHOICE, NOTE_MODES},
    {"Rango", 1, 3, 1, U_NUM, 0},
    {"Esc. pista", 0, NMODES, 0, U_CHOICE, SCALE_NAMES},
    {"Bits", 2, 16, 16, U_NUM, 0},
    {"Decimar", 1, 32, 1, U_NUM, 0},
    {"Pasos", 1, NSTEP, 16, U_NUM, 0},
    {"Divisi\363n", 0, 5, 2, U_CHOICE, DIV_NAMES},
    {"Pulsos", 0, NSTEP, 4, U_NUM, 0},
    {"Rotaci\363n", 0, NSTEP - 1, 0, U_NUM, 0},
    {"Probab.", 0, 100, 100, U_PCT, 0},
    {"Ratchet", 0, 100, 0, U_PCT, 0},
    {"Microtiempo", 0, 100, 0, U_PCT, 0},
    {"Turing", 0, 100, 0, U_PCT, 0},
    {"Silencio", 0, 1, 0, U_CHOICE, NOSI},
    {"Bloqueo", 0, 1, 0, U_CHOICE, NOSI},
};

/* the shared parameters */
enum { G_TEMPO, G_SWING, G_MUTR, G_MUTS, G_STUTP, G_STUTL, G_COMBHZ, G_COMBFB, G_COMBMIX, G_DLYT, G_DLYFB, G_DLYMIX,
       G_REVMIX, G_REVDECAY, G_REVDAMP,
       G_DRIVE, G_VOLUME, G_ROOT, G_MODE, G_QUANT, G_MIDICH, G_SYNC, G_COUNT };
static const ac_pinfo_t GP[G_COUNT] = {
    {"Tempo", 40, 240, 128, U_BPM, 0},
    {"Swing", 0, 75, 0, U_PCT, 0},
    {"Mutar ritmo", 0, 100, 35, U_PCT, 0},
    {"Mutar sonido", 0, 100, 30, U_PCT, 0},
    {"Stutter auto", 0, 100, 0, U_PCT, 0},
    {"Stutter", 0, 4, 2, U_CHOICE, STUT_NAMES},
    {"Comb", 0, 100, 40, U_NUM, 0},
    {"Comb realim.", 0, 95, 60, U_PCT, 0},
    {"Comb mezcla", 0, 100, 30, U_PCT, 0},
    {"Delay", 0, 5, 3, U_CHOICE, DIV_NAMES},
    {"Delay realim.", 0, 95, 40, U_PCT, 0},
    {"Delay mezcla", 0, 100, 35, U_PCT, 0},
    {"Reverb", 0, 100, 40, U_PCT, 0},
    {"Cola", 0, 95, 70, U_PCT, 0},
    {"Oscurecer", 0, 100, 40, U_PCT, 0},
    {"Saturar", 0, 100, 20, U_PCT, 0},
    {"Volumen", 0, 100, 80, U_PCT, 0},
    {"Ra\355z", 0, 11, 0, U_CHOICE, KEY_NAMES},
    {"Escala", 0, NMODES - 1, 3, U_CHOICE, MODE_NAMES},
    {"Cuantizar", 0, 1, 0, U_CHOICE, NOSI},
    {"Canal MIDI", 0, 15, 9, U_CH, 0},
    {"Reloj", 0, 1, 0, U_CHOICE, SYNC_NAMES},
};

static int16_t tp[NTRK][T_COUNT];                    /* the tracks' values */
static int16_t gp[G_COUNT];                          /* the shared values */

/* a step of a pattern */
typedef struct {
    uint8_t on, vel;             /* vel 1 .. 127 */
    uint8_t ratch;               /* 1 .. 4 hits in the step */
    uint8_t micro;               /* 0 .. 75: % of the step it comes late */
} step_t;
static step_t pat[NTRK][NSTEP];

/* the default sounds: engine and E1 .. E6 of each track */
static const int16_t TRK_DEF[NTRK][8] = {
    /* engine, E1..E6, pulses */
    {EN_KICK, 30, 45, 60, 25, 30, 25, 4},
    {EN_METAL, 55, 15, 60, 40, 70, 20, 5},
    {EN_NOISE, 70, 30, 12, 50, 40, 0, 7},
    {EN_CLICK, 60, 10, 30, 50, 30, 30, 3},
    {EN_GLITCH, 50, 30, 50, 30, 40, 50, 2},
    {EN_MODAL, 45, 45, 30, 50, 30, 20, 5},
};
static const int16_t TRK_LEN[NTRK] = {16, 16, 16, 12, 9, 7};      /* polymeters from the start */

/* ---------------------------------------------------------- colours --- */
/* a dark machine room: graphite ground, one colour per track */
#define UC_BG RGB(0x10, 0x12, 0x16)
#define UC_PANEL RGB(0x22, 0x26, 0x2E)
#define UC_UNLIT RGB(0x3A, 0x40, 0x4C)
#define UC_TEXT RGB(0xEE, 0xF0, 0xF2)
#define UC_MUTED RGB(0x93, 0x9B, 0xA8)
#define UC_SAND RGB(0xFF, 0xB8, 0x4D)
#define UC_CORAL RGB(0xFF, 0x5E, 0x4D)
#define UC_WARN RGB(0xFF, 0x3B, 0x5C)
static const uint16_t TRK_COL[NTRK] = {RGB(0xFF, 0xB8, 0x4D), RGB(0x4D, 0xE1, 0xFF), RGB(0xFF, 0x5C, 0xC8),
                                       RGB(0xB8, 0xFF, 0x4D), RGB(0xFF, 0x7A, 0x3D), RGB(0x9E, 0x8C, 0xFF)};
