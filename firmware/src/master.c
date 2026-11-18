/* SPDX-License-Identifier: GPL-3.0-only
 * The MASTER pot, read before the audio starts (so the first block plays at its level) and followed in the main
 * loop. master_knob: the pot x16, smoothed per UI frame. The gain is a square law, 4096 = unity. */
static int32_t master_knob = 512 * 16;
static int32_t master_of_pot(uint32_t a) { return (int32_t)((a * a) >> 8) > 4096 ? 4096 : (int32_t)((a * a) >> 8); }
static void batt_poll(void);
static void master_poll(void)
{
    batt_poll();
    int32_t a = fm1_adc_read(FM1_ADC_MASTER);
    if (a >= 0) {
        master_knob += (a * 16 - master_knob) / 8;
        gr_master_q12 = master_of_pot((uint32_t)(master_knob / 16));
    }
}
/* power-on, before audio_init: the knob as it stands, no glide */
static void master_boot(void)
{
    uint32_t i;
    (void)fm1_adc_read(FM1_ADC_MASTER);                 /* (the first conversion after the pin setup: dropped) */
    for (i = 0; i < 3u; i++) {
        int32_t a = fm1_adc_read(FM1_ADC_MASTER);
        if (a >= 0) {
            master_knob = a * 16;
            gr_master_q12 = master_of_pot((uint32_t)a);
            return;
        }
    }
}

/* amb-iguos: the battery, as the stock firmware reads it (fm1-x0x, melodee): ADC ch3 through its divider, a slow
 * average; 591 / 561 / 531 are the stock firmware's thresholds for 3 / 2 / 1 bars. There is no charger line, so a USB
 * host means power. batt_level: 0 .. 3 bars, 4 = USB power, -1 = not read yet */
static int32_t batt_raw;
static uint32_t batt_ms;
static void batt_poll(void)
{
    int32_t b;
    if ((uint32_t)(fm1_ms - batt_ms) < 50u)
        return;
    batt_ms = fm1_ms;
    b = fm1_adc_read(FM1_ADC_BATT);
    if (b > 0)
        batt_raw = batt_raw ? batt_raw + (b - batt_raw) / 32 : b;
}
static int32_t batt_level(void)
{
    if (usb.config && !usb.suspended)
        return 4;
    if (!batt_raw)
        return -1;
    return batt_raw >= 591 ? 3 : batt_raw >= 561 ? 2 : batt_raw >= 531 ? 1 : 0;
}
/* a rough charge, 0 .. 100 %, between the empty and the full thresholds (for the System page) */
static int32_t batt_pct(void) { return batt_raw ? clampi((batt_raw - 521) * 100 / (601 - 521), 0, 100) : -1; }
