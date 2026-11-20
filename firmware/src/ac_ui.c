/* SPDX-License-Identifier: GPL-3.0-only
 * The screens of auto-chre, like amb-iguos's: a status bar, a large live picture, the page dots and the four dials
 * K1 .. K4 turn.
 *
 *   status bar   0 .. 23    the track (1 .. 6 in its colour), the page, the tempo, PLAY, REC, the battery
 *   picture     24 .. 151
 *   page dots  152 .. 159
 *   four dials 160 .. 239
 *
 * HOME PATRÓN (the six tracks running against each other), EDIT SONIDO (the track's engine, its sound drawn),
 * ENV VOZ (level, pan, send, tuning, crushing), SEQ RITMO (the track's pattern, big), FX EFECTOS, SCL AFINAR,
 * GLO SISTEMA, SAVE PRESETS. */

enum { SC_PATRON, SC_SONIDO, SC_VOZ, SC_RITMO, SC_FX, SC_AFINAR, SC_SISTEMA, SC_PRESETS, SC_COUNT };
static const char *const SC_TITLE[SC_COUNT] = {"Patr\363n", "Sonido", "Voz", "Ritmo", "Efectos", "Afinar", "Sistema",
                                               "Presets"};
#define STATUS_H 24
#define PIC_Y 24
#define PIC_H 128
#define DOTS_Y 152
#define CELL_Y 160
#define CELL_W 60
#define CELL_H 80

/* a cell: a track value, a shared value, or a preset action */
enum { CL_NONE, CL_T, CL_G, CL_SLOT, CL_LOAD, CL_CLEAR, CL_NEW };
typedef struct { uint8_t kind, arg; } cl_t;

static struct {
    uint8_t page, trk, sub[SC_COUNT], slot, confirm;
    uint32_t turned[4], confirm_ms, notice_ms;
    char notice[32];
    uint32_t cell_key[4], status_key, pic_key, dots_key;
    uint8_t force, last_page, last_trk, last_sub, uboot;
    uint32_t frame, hits_seen[NTRK], flash_ms[NTRK];
    uint32_t prev_key;                      /* (main.c compatibility) */
} ui;

static int32_t batt_raw;
static int32_t batt_level(void);
static int32_t batt_pct(void);

static void ui_notice(const char *s)
{
    str_cpy(ui.notice, s, sizeof ui.notice);
    ui.notice_ms = fm1_ms ? fm1_ms : 1u;
}

/* ------------------------------------------------------- primitives --- */
static inline uint16_t px_get(int32_t x, int32_t y)
{
    y += cv_oy;
    if ((uint32_t)x >= cv_w || (uint32_t)y >= cv_h)
        return cv_bg;
    return (uint16_t)(((cv_px[y * (int32_t)cv_w + x] >> 8) & 0xFFu) | ((cv_px[y * (int32_t)cv_w + x] & 0xFFu) << 8));
}
static void cv_dot(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t x, y, inner = r * r - r, outer = r * r + r;
    for (y = cy - r - 1; y <= cy + r + 1; y++)
        for (x = cx - r - 1; x <= cx + r + 1; x++) {
            int32_t d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (x < 0 || x >= (int32_t)cv_w || d2 >= outer)
                continue;
            if (d2 <= inner)
                cv_pset(x, y, c);
            else
                cv_pset(x, y, ux_mix(px_get(x, y), c, (outer - d2) * 100 / (2 * r)));
        }
}
static void cv_glow(int32_t cx, int32_t cy, int32_t r, uint16_t c, int32_t pct)
{
    int32_t x, y, r2 = r * r;
    for (y = cy - r; y <= cy + r; y++)
        for (x = cx - r; x <= cx + r; x++) {
            int32_t d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d2 >= r2 || x < 0 || x >= (int32_t)cv_w)
                continue;
            cv_pset(x, y, ux_mix(px_get(x, y), c, pct * (r2 - d2) / r2));
        }
}
static uint32_t hash_add(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t hash_str(uint32_t h, const char *s)
{
    while (*s)
        h = hash_add(h, (uint8_t)*s++);
    return h;
}
static inline uint16_t trk_col(uint32_t k) { return TRK_COL[k % NTRK]; }

/* ------------------------------------------------------------- dials --- */
#define DIAL_DOTS 17
#define DIAL_R 19
static int8_t dial_dx[DIAL_DOTS], dial_dy[DIAL_DOTS];
static void dial_init(void)
{
    int32_t i;
    for (i = 0; i < DIAL_DOTS; i++) {
        uint32_t ph = (uint32_t)(0x60000000u + (uint32_t)(0xC0000000u / (DIAL_DOTS - 1)) * (uint32_t)i);
        int32_t c = sin32(ph + 0x40000000u), s = sin32(ph);
        dial_dx[i] = (int8_t)((c * DIAL_R + (c < 0 ? -16384 : 16384)) >> 15);
        dial_dy[i] = (int8_t)((s * DIAL_R + (s < 0 ? -16384 : 16384)) >> 15);
    }
}
static void draw_dial(int32_t cx, int32_t cy, int32_t norm, int bipolar, uint16_t lit)
{
    int32_t from = bipolar ? 32768 : 0, low = norm < from ? norm : from, high = norm < from ? from : norm, i;
    const int32_t slot = 65536 / (2 * (DIAL_DOTS - 1));
    for (i = 0; i < DIAL_DOTS; i++) {
        int32_t at = i * 65536 / (DIAL_DOTS - 1), amount = 0;
        if (at + slot <= high && at - slot >= low)
            amount = 100;
        else if (at >= low - slot && at <= high + slot) {
            int32_t a = at - slot > low ? at - slot : low, b = at + slot < high ? at + slot : high;
            amount = clampi((b - a) * 100 / (2 * slot), 0, 100);
        }
        cv_dot(cx + dial_dx[i], cy + dial_dy[i], amount > 50 ? 3 : 1, ux_mix(UC_UNLIT, lit, amount));
    }
    if (bipolar)
        cv_rect(cx, cy - DIAL_R - 6, 1, 3, UC_MUTED);
    {
        uint32_t ph = 0x60000000u + (uint32_t)(((uint64_t)(uint32_t)norm * 0xC0000000u) >> 16);
        cv_dot(cx + ((sin32(ph + 0x40000000u) * 11) >> 15), cy + ((sin32(ph) * 11) >> 15), 2, lit);
    }
}
static void draw_choice_dots(int32_t cx, int32_t cy, int32_t count, int32_t index, uint16_t lit)
{
    int32_t gap = count > 8 ? 4 : count > 5 ? 7 : 10, x0 = cx - (count - 1) * gap / 2, i;
    if (count > 13) {
        cv_rect(cx - 24, cy, 48, 1, UC_UNLIT);
        cv_dot(cx - 24 + index * 48 / (count - 1), cy, 3, lit);
        return;
    }
    for (i = 0; i < count; i++)
        cv_dot(x0 + i * gap, cy, i == index ? 3 : 1, i == index ? lit : UC_UNLIT);
}

/* ------------------------------------------------------------- pages --- */
#define T_(x) {CL_T, x}
#define G_(x) {CL_G, x}
#define NONE_ {CL_NONE, 0}
static const cl_t PG_PATRON[2][4] = {{G_(G_TEMPO), G_(G_SWING), G_(G_MUTR), G_(G_MUTS)},
                                     {T_(T_MUTE), T_(T_LOCK), T_(T_PROB), T_(T_VAR)}};
static const cl_t PG_SONIDO[3][4] = {{T_(T_TYPE), T_(T_E1), T_(T_E2), T_(T_E3)}, {T_(T_E4), T_(T_E5), T_(T_E6), T_(T_VAR)},
                                     {T_(T_NOTES), T_(T_RANGE), T_(T_SCALE), T_(T_TUNE)}};
static const cl_t PG_VOZ[2][4] = {{T_(T_LEVEL), T_(T_PAN), T_(T_SEND), T_(T_RSEND)}, {T_(T_BITS), T_(T_DECIM), T_(T_TUNE), NONE_}};
static const cl_t PG_RITMO[2][4] = {{T_(T_LEN), T_(T_DIV), T_(T_PULSES), T_(T_ROT)},
                                    {T_(T_PROB), T_(T_RATCH), T_(T_MICRO), T_(T_TURING)}};
static const cl_t PG_FX[4][4] = {{G_(G_COMBHZ), G_(G_COMBFB), G_(G_COMBMIX), G_(G_DLYMIX)},
                                 {G_(G_DLYT), G_(G_DLYFB), G_(G_DRIVE), G_(G_VOLUME)},
                                 {G_(G_REVMIX), G_(G_REVDECAY), G_(G_REVDAMP), T_(T_RSEND)},
                                 {G_(G_STUTP), G_(G_STUTL), NONE_, NONE_}};
static const cl_t PG_AFINAR[1][4] = {{G_(G_ROOT), G_(G_MODE), G_(G_QUANT), T_(T_SCALE)}};
static const cl_t PG_SISTEMA[1][4] = {{G_(G_MIDICH), G_(G_SYNC), NONE_, NONE_}};
static const cl_t PG_PRESETS[1][4] = {{{CL_SLOT, 0}, {CL_LOAD, 0}, {CL_CLEAR, 0}, {CL_NEW, 0}}};

static const char *ui_page_name(uint32_t sc, uint32_t sub)
{
    switch (sc) {
    case SC_PATRON: return sub ? "Pista" : "Patr\363n";
    case SC_SONIDO: return sub == 0 ? "Sonido" : sub == 1 ? "Sonido 2" : "Notas en escala";
    case SC_VOZ: return sub ? "Voz: destrozo" : "Voz";
    case SC_RITMO: return sub ? "Ritmo: azar" : "Ritmo";
    case SC_FX: return sub == 0 ? "Comb" : sub == 1 ? "Delay y salida" : sub == 2 ? "Reverb" : "Stutter";
    default: return SC_TITLE[sc];
    }
}

static uint32_t ui_cells(uint32_t sc, uint32_t sub, cl_t *c)
{
    const cl_t(*t)[4] = 0;
    uint32_t n = 1, i;
    switch (sc) {
    case SC_PATRON: t = PG_PATRON; n = 2; break;
    case SC_SONIDO: t = PG_SONIDO; n = 3; break;
    case SC_VOZ: t = PG_VOZ; n = 2; break;
    case SC_RITMO: t = PG_RITMO; n = 2; break;
    case SC_FX: t = PG_FX; n = 4; break;
    case SC_AFINAR: t = PG_AFINAR; break;
    case SC_SISTEMA: t = PG_SISTEMA; break;
    default: t = PG_PRESETS; break;
    }
    if (sub >= n)
        sub = 0;
    for (i = 0; i < 4u; i++)
        c[i] = t[sub][i];
    return n;
}
static uint32_t ui_npages(uint32_t sc)
{
    cl_t c[4];
    return ui_cells(sc, 0, c);
}

/* a value as text */
static void fmt_val(char *d, const ac_pinfo_t *p, int32_t v)
{
    d[0] = 0;
    switch (p->unit) {
    case U_CHOICE: str_cpy(d, p->labels[clampi(v, p->mn, p->mx) - p->mn], 24); break;
    case U_PCT: fmt_int(d, v); str_cpy(d + str_len(d), "%", 4); break;
    case U_SEMI:
    case U_BIP:
        if (v > 0) {
            d[0] = '+';
            fmt_int(d + 1, v);
        } else
            fmt_int(d, v);
        if (p->unit == U_SEMI)
            str_cpy(d + str_len(d), " st", 6);
        break;
    case U_CH: fmt_int(d, v + 1); break;
    default: fmt_int(d, v); break;
    }
}

/* -------------------------------------------------------------- cells --- */
typedef struct {
    const char *name;
    char value[28];
    int32_t norm;
    uint8_t kind, count, index, edit, empty;
} cell_t;
enum { CK_DIAL, CK_BIDIAL, CK_CHOICE, CK_ACTION, CK_BIG };

static const ac_pinfo_t *cl_info(const cl_t *cl) { return cl->kind == CL_T ? &TP[cl->arg] : &GP[cl->arg]; }
static int16_t *cl_val(const cl_t *cl) { return cl->kind == CL_T ? &tp[ui.trk][cl->arg] : &gp[cl->arg]; }

static void describe(const cl_t *cl, cell_t *c)
{
    memset(c, 0, sizeof *c);
    if (cl->kind == CL_T || cl->kind == CL_G) {
        const ac_pinfo_t *p = cl_info(cl);
        int32_t v = *cl_val(cl);
        c->name = p->name;
        if (cl->kind == CL_T && cl->arg >= T_E1 && cl->arg <= T_E6)
            c->name = ENGINE_P[clampi(tp[ui.trk][T_TYPE], 0, EN_COUNT - 1)][cl->arg - T_E1];
        fmt_val(c->value, p, v);
        c->norm = (v - p->mn) * 65535 / (p->mx - p->mn ? p->mx - p->mn : 1);
        c->kind = p->unit == U_CHOICE ? CK_CHOICE : p->unit == U_CH || p->unit == U_BPM ? CK_BIG : p->unit == U_BIP || p->unit == U_SEMI ? CK_BIDIAL : CK_DIAL;
        if (cl->kind == CL_T && cl->arg == T_LEN)
            c->kind = CK_BIG;
        c->count = (uint8_t)(p->mx - p->mn + 1);
        c->index = (uint8_t)(v - p->mn);
    } else if (cl->kind == CL_SLOT) {
        c->name = "Ranura";
        c->kind = CK_BIG;
        fmt_int(c->value, (int32_t)ui.slot + 1);
    } else if (cl->kind == CL_LOAD) {
        c->name = "Cargar";
        c->kind = CK_ACTION;
        c->empty = !((presets_mask >> ui.slot) & 1);
        str_cpy(c->value, c->empty ? "vac\355a" : "gira >", sizeof c->value);
    } else if (cl->kind == CL_CLEAR) {
        c->name = "Vaciar";
        c->kind = CK_ACTION;
        str_cpy(c->value, "gira >", sizeof c->value);
    } else if (cl->kind == CL_NEW) {
        c->name = "Nuevo";
        c->kind = CK_ACTION;
        str_cpy(c->value, "gira >", sizeof c->value);
    } else {
        c->empty = 1;
        c->name = "";
    }
}

static void draw_cell(int32_t slot, const cell_t *c)
{
    uint32_t key = 2166136261u;
    uint16_t lit = c->edit ? UC_TEXT : trk_col(ui.trk);
    int32_t w = CELL_W, h = CELL_H, x = slot * CELL_W;
    char v[28];
    key = hash_str(key, c->name);
    key = hash_str(key, c->value);
    key = hash_add(key, (uint32_t)(c->norm >> 8) + ((uint32_t)c->kind << 12));
    key = hash_add(key, (uint32_t)c->edit + 2u * c->empty + 8u * ui.trk + 64u * c->index);
    if (ui.cell_key[slot] == key)
        return;
    ui.cell_key[slot] = key;
    cv_begin((uint32_t)w, (uint32_t)h, UC_BG);
    if (c->empty && c->kind != CK_ACTION) {
        cv_dot(w / 2, 40, 1, UC_UNLIT);
        cv_blit((uint32_t)x, CELL_Y);
        return;
    }
    if (c->edit)
        cv_rrect(1, 0, w - 2, h - 1, 9, UC_PANEL, UC_BG);
    {
        uint16_t under = c->edit ? UC_PANEL : UC_BG;
        text_fit(v, sizeof v, c->name, &AF_S, w - 4);
        cv_text_in(0, 2, w, &AF_S, v, c->edit ? UC_TEXT : UC_MUTED, under);
        switch (c->kind) {
        case CK_DIAL: draw_dial(w / 2, 40, c->norm, 0, lit); break;
        case CK_BIDIAL: draw_dial(w / 2, 40, c->norm, 1, lit); break;
        case CK_CHOICE: draw_choice_dots(w / 2, 40, c->count, c->index, lit); break;
        case CK_BIG:
            cv_text_in(0, 24, w, &AF_L, c->value, lit, under);
            cv_blit((uint32_t)x, CELL_Y);
            return;
        case CK_ACTION: {
            uint16_t face = c->empty ? UC_PANEL : ux_mix(UC_BG, trk_col(ui.trk), 40);
            cv_rrect(6, 26, w - 12, 30, 10, face, under);
            cv_text_in(6, 26 + CAP_IN(S, 30), w - 12, &AF_S, c->value, c->empty ? UC_UNLIT : UC_TEXT, face);
            cv_blit((uint32_t)x, CELL_Y);
            return;
        }
        default: break;
        }
        if (text_w(&AF_M, c->value) <= w - 4)
            cv_text_in(0, h - 21, w, &AF_M, c->value, c->edit ? UC_TEXT : UC_TEXT, under);
        else {
            text_fit(v, sizeof v, c->value, &AF_S, w - 4);
            cv_text_in(0, h - 19, w, &AF_S, v, UC_TEXT, under);
        }
    }
    cv_blit((uint32_t)x, CELL_Y);
}

static void draw_cells(void)
{
    cl_t cl[4];
    uint32_t i;
    ui_cells(ui.page, ui.sub[ui.page], cl);
    for (i = 0; i < 4u; i++) {
        cell_t c;
        describe(&cl[i], &c);
        c.edit = cl[i].kind != CL_NONE && (fm1_ms - ui.turned[i]) < 600u;
        draw_cell((int32_t)i, &c);
    }
}

static void draw_page_dots(void)
{
    uint32_t n = ui_npages(ui.page), sub = ui.sub[ui.page], key = n * 16u + sub + 256u * ui.trk + 1u, i;
    int32_t x0;
    if (key == ui.dots_key)
        return;
    ui.dots_key = key;
    cv_begin(240, 8, UC_BG);
    x0 = 120 - ((int32_t)n - 1) * 7;
    if (n > 1u)
        for (i = 0; i < n; i++)
            cv_dot(x0 + (int32_t)i * 14, 4, i == sub ? 3 : 1, i == sub ? trk_col(ui.trk) : UC_UNLIT);
    cv_blit(0, DOTS_Y);
}

/* ------------------------------------------------------------ status --- */
static void draw_battery(int32_t x, int32_t y)
{
    int32_t lv = batt_level(), i;
    uint16_t edge = lv == 0 ? UC_WARN : UC_MUTED;
    if (lv == 4) {
        cv_rect(x + 1, y + 3, 8, 6, UC_MUTED);
        cv_rect(x + 3, y, 1, 3, UC_MUTED);
        cv_rect(x + 6, y, 1, 3, UC_MUTED);
        cv_rect(x + 4, y + 9, 2, 3, UC_MUTED);
        cv_text(x + 11, y - 1, &AF_S, "USB", UC_MUTED);
        return;
    }
    cv_rrect(x, y, 22, 12, 3, edge, UC_BG);
    cv_rrect(x + 1, y + 1, 20, 10, 2, UC_BG, edge);
    cv_rect(x + 22, y + 4, 2, 4, edge);
    for (i = 0; i < 3; i++)
        if (lv > i)
            cv_rect(x + 3 + i * 6, y + 3, 4, 6, lv == 1 ? UC_CORAL : UC_TEXT);
}

static void draw_status(void)
{
    uint32_t key = 2166136261u, beat = sq.playing ? (sq.ticks / 96u) & 1u : 0u;
    char det[40], t[12];
    uint16_t acc = trk_col(ui.trk);
    if (ui.notice_ms && (uint32_t)(fm1_ms - ui.notice_ms) > 2200u)
        ui.notice_ms = 0;
    str_cpy(det, ui.notice_ms ? ui.notice : ui_page_name(ui.page, ui.sub[ui.page]), sizeof det);
    fmt_int(t, gp[G_TEMPO]);
    key = hash_str(key, det);
    key = hash_str(key, t);
    key = hash_add(key, ui.trk + 8u * sq.playing + 16u * sq.rec + 32u * (uint32_t)(batt_level() + 1) + 256u * beat +
                            512u * (ui.notice_ms != 0) + 1024u * (uint32_t)gp[G_SYNC]);
    if (key == ui.status_key)
        return;
    ui.status_key = key;
    cv_begin(240, STATUS_H, UC_BG);
    {
        char l[2] = {(char)('1' + ui.trk), 0};
        cv_rrect(3, 3, 20, 18, 6, acc, UC_BG);
        cv_text_in(3, 3 + CAP_IN(M, 18), 20, &AF_M, l, UC_BG, acc);
    }
    cv_text_fit(29, 3, &AF_M, det, ui.notice_ms ? UC_SAND : UC_TEXT, UC_BG, 108);
    if (gp[G_SYNC])
        str_cpy(t, "MIDI", sizeof t);
    cv_text_r(170, 6, &AF_S, t, UC_MUTED, UC_BG);
    if (sq.playing) {
        uint16_t c = beat ? UC_TEXT : UC_MUTED;
        int32_t i;
        for (i = 0; i < 7; i++)
            cv_rect(176 + i, 6 + i / 2, 1, 12 - (i / 2) * 2, c);
    }
    if (sq.rec)
        cv_dot(195, 12, 5, UC_WARN);
    draw_battery(212, 6);
    cv_blit(0, 0);
}

/* ---------------------------------------------------------- pictures --- */
static void pic_begin(int32_t top)
{
    cv_begin(240, 64, UC_BG);
    cv_oy = -top;
}
static void pic_end(int32_t top)
{
    cv_oy = 0;
    cv_blit(0, (uint32_t)(PIC_Y + top));
}

/* PATRÓN: the six tracks, each across the whole width whatever its length (so the polymeters show), the steps in
 * the track's colour (taller: louder; dots: ratchets), the step that plays lit, a flash on each hit; muted tracks dim,
 * locked tracks with a bar at the left. The selected track is framed. */
static void patron_strip(int32_t top)
{
    uint32_t k, i;
    pic_begin(top);
    for (k = 0; k < NTRK; k++) {
        int32_t y0 = 2 + (int32_t)k * 21, h = 19, len = tp[k][T_LEN], x0 = 8, w = 230;
        uint16_t col = trk_col(k), dim = ux_mix(UC_BG, col, 30);
        int muted = tp[k][T_MUTE];
        if (k == ui.trk)
            cv_rrect(4, y0 - 1, 235, h + 2, 4, UC_PANEL, UC_BG);
        if (tp[k][T_LOCK])
            cv_rect(1, y0 + 2, 2, h - 4, UC_TEXT);
        for (i = 0; i < (uint32_t)len; i++) {
            int32_t xa = x0 + (int32_t)i * w / len, xb = x0 + (int32_t)(i + 1u) * w / len - 1, sh;
            const step_t *st = &pat[k][i];
            int playing = sq.playing && tr[k].step == i;
            uint16_t c;
            if (!st->on) {
                cv_rect(xa, y0 + h / 2, xb - xa, 1, playing ? UC_MUTED : UC_UNLIT);
                continue;
            }
            sh = 4 + (h - 4) * st->vel / 127;
            c = muted ? dim : playing ? UC_TEXT : col;
            cv_rect(xa + (int32_t)st->micro * (xb - xa) / 150, y0 + (h - sh) / 2, xb - xa > 2 ? xb - xa - 1 : 1, sh, c);
            if (st->ratch > 1)
                cv_rect(xa, y0 + h - 2, xb - xa, 1, UC_TEXT);
        }
        if (fm1_ms - ui.flash_ms[k] < 120u && !muted)
            cv_glow(x0 + (int32_t)tr[k].step * w / (len ? len : 1), y0 + h / 2, 12, col, 45);
    }
    pic_end(top);
}

/* SONIDO: the track's sound, drawn from the engine itself (one hit, no variation, ~0.5 s) */
static int16_t prev_pk[240];
static uint32_t prev_key;
static voice_t prev_v;
static void sound_preview(uint32_t k)
{
    uint32_t key = 2166136261u, i, x, per = FS / 2u / 240u;
    for (i = 0; i < T_COUNT; i++)
        key = hash_add(key, (uint32_t)tp[k][i]);
    key = hash_add(key, k);
    if (key == prev_key)
        return;
    prev_key = key;
    prev_v.ks = ks_mem[NTRK];
    voice_setup(&prev_v, tp[k], 110u, 0, 0);
    for (x = 0; x < 240u; x++) {
        int32_t pk = 0;
        for (i = 0; i < per; i++) {
            int32_t s = 0;
            if (prev_v.on) {
                s = absi(mulq15(voice_sample(&prev_v), prev_v.amp >> 15));
                prev_v.amp = env_step(prev_v.amp, prev_v.adec);
                prev_v.t++;
            }
            if (s > pk)
                pk = s;
        }
        prev_pk[x] = (int16_t)pk;
        if ((x & 31u) == 0u)
            fm1_wdt_feed();
    }
}

static void sonido_strip(int32_t top)
{
    uint32_t k = ui.trk;
    int32_t x, y;
    uint16_t col = trk_col(k);
    pic_begin(top);
    for (x = 0; x < 240; x += 2) {
        int32_t h = clampi(prev_pk[x] * 50 / 32768, 0, 52);
        for (y = 0; y <= h; y += 2) {
            uint16_t c = ux_mix(UC_BG, col, 100 - 55 * y / (h + 1));
            cv_rect(x, 74 - y, 2, 1, c);
            if (y)
                cv_rect(x, 74 + y, 2, 1, c);
        }
    }
    cv_text(6, 4, &AF_L, ENGINE_NAMES[clampi(tp[k][T_TYPE], 0, EN_COUNT - 1)], UC_TEXT);
    cv_text_r(234, 112, &AF_S, "0.5 s", UC_UNLIT, UC_BG);
    pic_end(top);
}

/* VOZ: where the track sits (pan, level, send) and how crushed it is */
static void voz_strip(int32_t top)
{
    uint32_t k = ui.trk;
    int32_t x, bits = tp[k][T_BITS], dec = tp[k][T_DECIM];
    uint16_t col = trk_col(k);
    pic_begin(top);
    {   /* a sine, crushed as the track is */
        int32_t q = 1 << (16 - bits), held = 0;
        for (x = 0; x < 240; x++) {
            int32_t s;
            if (x % dec == 0)
                held = (sin32((uint32_t)x * 0x01111111u * 2u) / q) * q;
            s = held * 34 / 32768;
            cv_rect(x, 44 - s, 1, 2, col);
        }
    }
    {   /* the stage: pan left-right, level up */
        int32_t px = 120 + tp[k][T_PAN] * 2, py = 120 - tp[k][T_LEVEL] * 32 / 100;
        cv_rect(20, 120, 200, 1, UC_UNLIT);
        cv_dot(px, py, 6, col);
        cv_glow(px, py, 6 + tp[k][T_SEND] / 6, col, 40);
    }
    pic_end(top);
}

/* RITMO: the track's pattern, big: two rows of 16, velocity as height, ratchets as stacked bars, lateness as a shift */
static void ritmo_strip(int32_t top)
{
    uint32_t k = ui.trk, i, len = (uint32_t)tp[k][T_LEN];
    uint16_t col = trk_col(k);
    pic_begin(top);
    for (i = 0; i < NSTEP; i++) {
        int32_t cx = 4 + (int32_t)(i % 16u) * 14, cy = 6 + (int32_t)(i / 16u) * 60, h = 50;
        const step_t *st = &pat[k][i];
        int playing = sq.playing && tr[k].step == i;
        if (i >= len) {
            cv_rect(cx + 4, cy + h / 2, 4, 1, UC_PANEL);
            continue;
        }
        cv_rrect(cx, cy, 12, h, 3, playing ? UC_UNLIT : UC_PANEL, UC_BG);
        if (st->on) {
            int32_t vh = 6 + (h - 8) * st->vel / 127, r, rh = vh / (st->ratch ? st->ratch : 1);
            for (r = 0; r < (st->ratch ? st->ratch : 1); r++)
                cv_rect(cx + 2 + st->micro / 25, cy + h - 1 - vh + r * rh, 8, rh - 1 > 1 ? rh - 1 : 1, playing ? UC_TEXT : col);
        }
    }
    pic_end(top);
}

/* EFECTOS: the comb's teeth, the delay's echoes, and the stutter when it loops */
static void fx_strip(int32_t top)
{
    int32_t x, i, st = fx_stutter_now;
    pic_begin(top);
    {
        int32_t teeth = 4 + gp[G_COMBHZ] / 4, fb = gp[G_COMBFB];
        for (x = 0; x < 240; x += 2) {
            int32_t ph = (x * teeth) % 240, d = absi(ph - 120), y = 40 - clampi((120 - d) * fb / 300 * gp[G_COMBMIX] / 60, 0, 34);
            cv_rect(x, y, 2, 40 - y + 1, ux_mix(UC_BG, UC_SAND, 60));
        }
    }
    {
        int32_t step = 20 + gp[G_DLYT] * 14, lv = 70 + gp[G_DLYMIX] * 30 / 100;
        for (i = 0, x = 12; x < 236 && lv > 5; x += step, i++) {
            cv_dot(x, (i & 1) ? 96 : 80, 1 + lv * 6 / 100, ux_mix(UC_BG, UC_TEXT, lv));
            lv = lv * (30 + gp[G_DLYFB] * 70 / 95) / 100;
        }
    }
    {   /* the reverb: a cloud that grows with the tail, as bright as the mix */
        int32_t rr = 6 + gp[G_REVDECAY] / 4, a = 15 + gp[G_REVMIX] / 2;
        cv_glow(206, 64, rr, ux_mix(UC_SAND, UC_TEXT, 100 - gp[G_REVDAMP]), a);
    }
    if (st >= 0)
        cv_text_in(0, 106, 240, &AF_M, STUT_NAMES[st], UC_WARN, UC_BG);
    pic_end(top);
}

/* AFINAR: the root and the scale the pitches snap to */
static void afinar_strip(int32_t top)
{
    static const uint8_t BLACK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    uint32_t root = (uint32_t)gp[G_ROOT], m = (uint32_t)trk_mode(tp[ui.trk]), i, k;
    int32_t lit = gp[G_QUANT] || tp[ui.trk][T_NOTES];
    char name[32];
    pic_begin(top);
    str_cpy(name, KEY_NAMES[root], sizeof name);
    str_cpy(name + str_len(name), " ", 2);
    str_cpy(name + str_len(name), MODE_NAMES[m], 20);
    cv_text_in(0, 10, 240, &AF_M, name, lit ? UC_TEXT : UC_UNLIT, UC_BG);
    for (i = 0; i < 12u; i++) {
        uint32_t note = (root + i) % 12u, in = 0;
        int32_t x = 6 + (int32_t)i * 19, h = BLACK[note] ? 46 : 62;
        for (k = 0; k < MODE_N[m]; k++)
            if (MODE_DEG[m][k] == i)
                in = 1;
        cv_rrect(x, 50, 16, h, 4, in ? (i == 0u ? UC_CORAL : ux_mix(UC_BG, UC_SAND, lit ? 85 : 40)) : UC_PANEL, UC_BG);
        cv_text_in(x - 2, 114, 20, &AF_S, KEY_NAMES[note], in ? UC_TEXT : UC_UNLIT, UC_BG);
    }
    pic_end(top);
}

/* SISTEMA: the battery, the load, and what the keys do */
static void sistema_strip(int32_t top)
{
    char b[32];
    int32_t lv = batt_level(), pct = batt_pct();
    pic_begin(top);
    cv_text(8, 2, &AF_M, "auto-chre", UC_TEXT);
    cv_text_r(234, 6, &AF_S, FELUCCA_VERSION, UC_UNLIT, UC_BG);
    if (lv == 4)
        str_cpy(b, "Bater\355a: USB", sizeof b);
    else {
        str_cpy(b, "Bater\355a: ", sizeof b);
        if (pct >= 0) {
            fmt_int(b + str_len(b), pct);
            str_cpy(b + str_len(b), " %", 4);
        }
    }
    cv_text(8, 24, &AF_S, b, lv == 0 ? UC_WARN : UC_MUTED);
    str_cpy(b, "CPU ", sizeof b);
    fmt_int(b + 4, (int32_t)(audio_cpu_q8 * 100u / 256u));
    str_cpy(b + str_len(b), " %", 4);
    cv_text_r(234, 24, &AF_S, b, UC_MUTED, UC_BG);
    cv_text(8, 46, &AF_S, "Teclas 1-6 tocar  7-12 silenciar", UC_TEXT);
    cv_text(8, 62, &AF_S, "13-17 stutter  18 caos  19 llenar", UC_TEXT);
    cv_text(8, 78, &AF_S, "20 deshacer  21-26 bloquear  27 tap", UC_TEXT);
    if (fm1_ms - ac_clk_last_ms < 500u && ac_clk_n) {             /* the MIDI clock: arriving, and its tempo */
        str_cpy(b, "Reloj MIDI: llega, ", sizeof b);
        fmt_int(b + str_len(b), (int32_t)((ac_clk_bpm10 + 5u) / 10u));
        str_cpy(b + str_len(b), " BPM", 6);
        cv_text(8, 100, &AF_S, b, gp[G_SYNC] ? UC_SAND : UC_WARN);
        if (!gp[G_SYNC])
            cv_text_r(234, 100, &AF_S, "(Reloj: Interno)", UC_WARN, UC_BG);
    } else {
        cv_text(8, 100, &AF_S, "Reloj MIDI: no llega", gp[G_SYNC] ? UC_WARN : UC_UNLIT);
    }
    cv_text(8, 114, &AF_S, "OCT- y OCT+ 5 s: actualizar", UC_UNLIT);
    pic_end(top);
}

static void presets_strip(int32_t top)
{
    uint32_t i;
    uint16_t acc = trk_col(ui.trk);
    pic_begin(top);
    for (i = 0; i < NPRESET; i++) {
        int32_t cx = 30 + (int32_t)(i % 4u) * 60, cy = 18 + (int32_t)(i / 4u) * 26, filled = (presets_mask >> i) & 1;
        char num[4];
        fmt_int(num, (int32_t)i + 1);
        if (i == ui.slot) {
            cv_rrect(cx - 26, cy - 11, 52, 22, 8, filled ? acc : UC_PANEL, UC_BG);
            cv_text_in(cx - 26, cy - 11 + CAP_IN(M, 22), 52, &AF_M, num, filled ? UC_BG : acc, filled ? acc : UC_PANEL);
        } else {
            cv_text_in(cx - 26, cy - 11 + CAP_IN(S, 22), 52, &AF_S, num, filled ? UC_TEXT : UC_UNLIT, UC_BG);
        }
    }
    cv_text_in(0, 114, 240, &AF_S, ui.confirm ? "SAVE otra vez: sobrescribir" : "SAVE: guardar aqu\355",
               ui.confirm ? UC_SAND : UC_MUTED, UC_BG);
    pic_end(top);
}

static uint32_t pic_key(void)
{
    uint32_t k = 2166136261u, i, t;
    k = hash_add(k, ui.page + 16u * ui.trk + 256u * ui.sub[ui.page]);
    switch (ui.page) {
    case SC_PATRON:
    case SC_RITMO:
        for (t = 0; t < NTRK; t++) {
            k = hash_add(k, tr[t].step + 64u * (fm1_ms - ui.flash_ms[t] < 120u));
            for (i = 0; i < T_COUNT; i++)
                k = hash_add(k, (uint32_t)tp[t][i]);
            for (i = 0; i < NSTEP; i++)
                k = hash_add(k, pat[t][i].on + 2u * pat[t][i].vel + 256u * pat[t][i].ratch + 2048u * pat[t][i].micro);
        }
        return hash_add(k, sq.playing);
    case SC_SONIDO:
        sound_preview(ui.trk);
        return hash_add(k, prev_key);
    case SC_FX:
        for (i = 0; i < G_COUNT; i++)
            k = hash_add(k, (uint32_t)gp[i]);
        return hash_add(k, (uint32_t)(fx_stutter_now + 1));
    case SC_SISTEMA: return hash_add(k, (fm1_ms >> 10) + (uint32_t)batt_raw * 7u);
    case SC_PRESETS: return hash_add(k, presets_mask + 65536u * ui.slot + 77u * ui.confirm);
    default:
        for (i = 0; i < T_COUNT; i++)
            k = hash_add(k, (uint32_t)tp[ui.trk][i]);
        for (i = 0; i < G_COUNT; i++)
            k = hash_add(k, (uint32_t)gp[i]);
        return k;
    }
}

static void draw_picture(void)
{
    uint32_t key = pic_key();
    int32_t top;
    if (key == ui.pic_key)
        return;
    ui.pic_key = key;
    for (top = 0; top < PIC_H; top += 64) {
        switch (ui.page) {
        case SC_PATRON: patron_strip(top); break;
        case SC_SONIDO: sonido_strip(top); break;
        case SC_VOZ: voz_strip(top); break;
        case SC_RITMO: ritmo_strip(top); break;
        case SC_FX: fx_strip(top); break;
        case SC_AFINAR: afinar_strip(top); break;
        case SC_SISTEMA: sistema_strip(top); break;
        default: presets_strip(top); break;
        }
    }
}

static void ui_draw(void)
{
    uint32_t k;
    ui.frame++;
    for (k = 0; k < NTRK; k++)
        if (ac_hits[k] != ui.hits_seen[k]) {
            ui.hits_seen[k] = ac_hits[k];
            ui.flash_ms[k] = fm1_ms;
        }
    if (ui.force || ui.page != ui.last_page || ui.trk != ui.last_trk || ui.sub[ui.page] != ui.last_sub) {
        lcd_fill(0, 0, 240, 240, UC_BG);
        memset(ui.cell_key, 0, sizeof ui.cell_key);
        ui.status_key = ui.pic_key = ui.dots_key = 0;
        ui.force = 0;
        ui.last_page = ui.page;
        ui.last_trk = ui.trk;
        ui.last_sub = ui.sub[ui.page];
    }
    draw_status();
    draw_picture();
    draw_page_dots();
    draw_cells();
}

static void ui_theme_init(void)
{
    palette_set(0);
    ux.mono = 0;
    ux.style = ST_FLAT;
}
