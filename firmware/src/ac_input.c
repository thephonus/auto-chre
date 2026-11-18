/* SPDX-License-Identifier: GPL-3.0-only
 * auto-chre: the panel. Every control does one thing on every screen:
 *
 *   HOME PATRÓN   EDIT SONIDO   ENV VOZ   SEQ RITMO   FX EFECTOS   SCL AFINAR   GLO SISTEMA   SAVE PRESETS
 *   (a section's button again: its next page; on PRESETS, SAVE saves, twice over a used slot)
 *   PLAY  start / stop        REC  record what the keys play       ARP  MUTAR RITMO        LFO  MUTAR SONIDO
 *   K1 .. K4  the dials       SELECT  the page                     ALGORITHM / OCT- OCT+  the track
 *   PRESETS  the preset slot  OCT- and OCT+ held 5 s: the update mode (main.c)
 *
 * The keys the main loop handles (the audio code plays 1 .. 6, the stutters, CAOS and LLENAR, ac_midi.c):
 *   7 .. 12 mute tracks 1 .. 6, 20 undo the last mutation, 21 .. 26 lock tracks 1 .. 6, 27 tap tempo. */
static const uint8_t BTN_SECTION[NB] = {[B_HOME] = SC_PATRON + 1, [B_EDIT] = SC_SONIDO + 1, [B_ENV] = SC_VOZ + 1,
                                        [B_SEQ] = SC_RITMO + 1, [B_FX] = SC_FX + 1, [B_SCL] = SC_AFINAR + 1,
                                        [B_GLO] = SC_SISTEMA + 1, [B_SAVE] = SC_PRESETS + 1};
static uint32_t kb_prev_m, tap_ms;

static void ui_set_page(uint32_t sc)
{
    ui.page = (uint8_t)sc;
    ui.confirm = 0;
    if (ui.sub[sc] >= ui_npages(sc))
        ui.sub[sc] = 0;
}

static void say_track(const char *what, uint32_t k, int on)
{
    char t[32];
    str_cpy(t, what, sizeof t);
    fmt_int(t + str_len(t), (int32_t)k + 1);
    str_cpy(t + str_len(t), on ? ": s\355" : ": no", 8);
    ui_notice(t);
}

static void ui_turn(const cl_t *cl, int32_t d, int32_t accel)
{
    if (cl->kind == CL_T || cl->kind == CL_G) {
        const ac_pinfo_t *p = cl_info(cl);
        int16_t *v = cl_val(cl);
        int32_t span = p->mx - p->mn, step = p->unit == U_CHOICE || span <= 40 ? 1 : accel;
        *v = (int16_t)clampi(*v + d * step, p->mn, p->mx);
        if (cl->kind == CL_T && (cl->arg == T_LEN || cl->arg == T_PULSES || cl->arg == T_ROT)) {
            if (tp[ui.trk][T_PULSES] > tp[ui.trk][T_LEN])
                tp[ui.trk][T_PULSES] = tp[ui.trk][T_LEN];
            pat_euclid(ui.trk);                          /* the pattern is remade from Euclid */
        }
    } else if (cl->kind == CL_SLOT) {
        ui.slot = (uint8_t)((ui.slot + NPRESET + (uint32_t)(d % NPRESET)) % NPRESET);
        ui.confirm = 0;
    } else if (cl->kind == CL_LOAD && d > 0) {
        char t[28];
        if (preset_load(ui.slot) == 0) {
            str_cpy(t, "Preset ", sizeof t);
            fmt_int(t + 7, (int32_t)ui.slot + 1);
            str_cpy(t + str_len(t), " cargado", 10);
            ui_notice(t);
        } else {
            ui_notice("Ranura vac\355a");
        }
    }
}

static void ui_keys_main(void)
{
    uint32_t cur = fm1_in.notes, on = (cur ^ kb_prev_m) & cur, k;
    kb_prev_m = cur;
    if (!on || ac_kb_hold)
        return;
    for (k = 0; k < NTRK; k++) {
        if ((on >> (6u + k)) & 1u) {
            tp[k][T_MUTE] ^= 1;
            say_track("Silencio ", k, tp[k][T_MUTE]);
        }
        if ((on >> (20u + k)) & 1u) {
            tp[k][T_LOCK] ^= 1;
            say_track("Bloqueo ", k, tp[k][T_LOCK]);
        }
    }
    if ((on >> 19) & 1u)
        ui_notice(undo_restore() == 0 ? "Deshecho" : "Nada que deshacer");
    if ((on >> 26) & 1u) {                               /* tap tempo */
        uint32_t dt = fm1_ms - tap_ms;
        if (dt > 200u && dt < 1500u) {
            char t[20];
            gp[G_TEMPO] = (int16_t)clampi((int32_t)(60000u / dt), GP[G_TEMPO].mn, GP[G_TEMPO].mx);
            str_cpy(t, "Tempo ", sizeof t);
            fmt_int(t + 6, gp[G_TEMPO]);
            ui_notice(t);
        }
        tap_ms = fm1_ms;
    }
}

static void ui_input(void)
{
    uint32_t now = fm1_ms, pressed, released, b, k;
    int32_t d;
    pressed = fm1_input_edges(&released);
    (void)released;
    if (ui.confirm && (uint32_t)(now - ui.confirm_ms) > 3000u)
        ui.confirm = 0;
    ui_keys_main();
    {
        cl_t cl[4];
        ui_cells(ui.page, ui.sub[ui.page], cl);
        for (k = 0; k < 4u; k++) {
            d = panel_enc(EN_K1 + k);
            if (d && cl[k].kind != CL_NONE) {
                int32_t a = d < 0 ? -d : d, accel = a >= 3 ? 8 : a == 2 ? 4 : (now - ui.turned[k]) < 40u ? 2 : 1;
                ui_turn(&cl[k], d, accel);
                ui.turned[k] = now;
            }
        }
    }
    d = panel_enc(EN_ALGO);
    if (d)
        ui.trk = (uint8_t)((ui.trk + NTRK + (uint32_t)(d % NTRK)) % NTRK);
    d = panel_enc(EN_SELECT);
    if (d) {
        uint32_t n = ui_npages(ui.page);
        ui.sub[ui.page] = (uint8_t)((ui.sub[ui.page] + n + (uint32_t)(d % (int32_t)n)) % n);
    }
    d = panel_enc(EN_PRESET);
    if (d) {
        if (ui.page != SC_PRESETS)
            ui_set_page(SC_PRESETS);
        ui.slot = (uint8_t)((ui.slot + NPRESET + (uint32_t)(d % NPRESET)) % NPRESET);
        ui.confirm = 0;
    }
    for (b = 0; b < (uint32_t)NB; b++) {
        if (!(pressed & (1u << PANEL_BTN[b])))
            continue;
        if (b == B_SAVE && ui.page == SC_PRESETS) {
            if (((presets_mask >> ui.slot) & 1) && !ui.confirm) {
                ui.confirm = 1;
                ui.confirm_ms = now;
            } else {
                char t[28];
                ui.confirm = 0;
                if (preset_save(ui.slot) == 0) {
                    str_cpy(t, "Guardado en ", sizeof t);
                    fmt_int(t + 12, (int32_t)ui.slot + 1);
                    ui_notice(t);
                } else {
                    ui_notice("No se pudo guardar");
                }
            }
            continue;
        }
        if (BTN_SECTION[b]) {
            uint32_t sc = (uint32_t)BTN_SECTION[b] - 1u;
            if (ui.page == sc)
                ui.sub[sc] = (uint8_t)((ui.sub[sc] + 1u) % ui_npages(sc));
            else
                ui_set_page(sc);
            continue;
        }
        switch (b) {
        case B_PLAY:
            if (sq.playing)
                sq.stop_req = 1;
            else
                sq.start_req = 1;
            break;
        case B_REC:
            sq.rec = !sq.rec;
            ui_notice(sq.rec ? "Grabando golpes" : "Grabaci\363n fuera");
            break;
        case B_ARP: {
            char t[28];
            mutate_rhythm((uint32_t)gp[G_MUTR]);
            str_cpy(t, "Ritmo mutado ", sizeof t);
            fmt_int(t + str_len(t), gp[G_MUTR]);
            str_cpy(t + str_len(t), "%", 2);
            ui_notice(t);
            break;
        }
        case B_LFO: {
            char t[28];
            mutate_sound((uint32_t)gp[G_MUTS]);
            str_cpy(t, "Sonido mutado ", sizeof t);
            fmt_int(t + str_len(t), gp[G_MUTS]);
            str_cpy(t + str_len(t), "%", 2);
            ui_notice(t);
            break;
        }
        case B_OCTDN: ui.trk = (uint8_t)((ui.trk + NTRK - 1u) % NTRK); break;
        case B_OCTUP: ui.trk = (uint8_t)((ui.trk + 1u) % NTRK); break;
        default: break;
        }
    }
}

/* the LEDs: the section, PLAY while it plays (blinking on the beat), REC, ARP and LFO lit as the mutators they are,
 * the keys of the tracks as they hit */
static void ui_leds(void)
{
    uint32_t b, k, beat = sq.playing && (sq.ticks % 96u) < 24u;
    for (b = 0; b < (uint32_t)NB; b++) {
        int on = (BTN_SECTION[b] && (uint32_t)BTN_SECTION[b] - 1u == ui.page) || (b == B_PLAY && sq.playing && (beat || !(sq.ticks % 96u < 48u))) ||
                 (b == B_REC && sq.rec) || b == B_ARP || b == B_LFO;
        if (b == B_PLAY)
            on = sq.playing && beat ? 0 : sq.playing;
        fm1_led_key(PANEL_BTN[b], on);
    }
    for (k = 0; k < KB_KEYS; k++) {
        int on = (fm1_in.notes >> k) & 1u;
        if (k < NTRK)
            on |= fm1_ms - ui.flash_ms[k] < 80u;
        else if (k >= 6u && k < 12u)
            on |= tp[k - 6u][T_MUTE];
        else if (k >= 20u && k < 26u)
            on |= tp[k - 20u][T_LOCK];
        fm1_led_key(14u + k, on);
    }
}
