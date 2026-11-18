/* SPDX-License-Identifier: GPL-3.0-only
 * The panel of the FM-1: which matrix button and encoder carries which printed label (measured on hardware by
 * Felucca / Mini-Tone). The labels are used by Mini-Grain as follows (gr_ui.c):
 *   K1..K4      the four dials of the current row        ALGORITHM   the row (or the generator type)
 *   SELECT      the page                                 PRESETS     the preset slot
 *   HOME GRAIN, ENV VOICE, FX EFFECTS, LFO FILTER/LFO, EDIT GENERATE, SCL SAMPLES, SAVE PRESETS, GLO SETTINGS
 *   ARP         the part (1 / 2)                          OCT- OCT+   the keyboard's octave
 *   PLAY, REC   the page's action (generate, load, save) SEQ         reserved for the sequencer */
enum { B_FX, B_SCL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC, B_OCTDN, B_OCTUP, NB };
enum { EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K2, EN_K3, EN_K4, NE };

/* matrix id of each label (hal/fm1_input.h) and of each encoder role */
static const uint8_t PANEL_BTN[NB] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1};
static const uint8_t PANEL_ENC[NE] = {0, 1, 6, 2, 3, 4, 5};

static uint32_t panel_btn_of(uint32_t matrix_id)        /* label of a matrix button, NB if none */
{
    uint32_t b;
    for (b = 0; b < NB; b++)
        if (PANEL_BTN[b] == matrix_id)
            return b;
    return NB;
}

/* detent steps of a role since the last call, + = clockwise */
static int32_t panel_enc(uint32_t role) { return fm1_enc_take(PANEL_ENC[role]); }
