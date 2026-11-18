/* SPDX-License-Identifier: GPL-3.0-only
 * auto-chre: a generative IDM drum machine for the M-Vave FM-1 (JieLi AC79 / WL82): six synthesized drum voices,
 * polymetric tracks with probability, ratchets, lateness and Turing mutation, rhythm and sound mutators, a comb, a
 * delay and a stutter. One compilation unit (the HAL is header-only); each file uses what the files above it define.
 * The platform (HAL, USB-MIDI / USB audio, TRS MIDI, flash storage, the M-UPGRADE entry) is Felucca / Mini-Tone by
 * Leo Kuroshita (@kurogedelic, Hügelton Instruments), GPL-3.0, as Mini-Grain and amb-iguos use it. */

/* ---------------------------------------------------- build options --- */
/* tools/build.py: GRAIN_<NAME>=0 / 1 in the environment overrides these (the old FELUCCA_ names still work). */
#ifndef FELUCCA_FLASH
#define FELUCCA_FLASH 1          /* flash driver + storage.c: the saved state and the presets */
#endif
#ifndef FELUCCA_OTA
#define FELUCCA_OTA 1            /* the M-UPGRADE update entry; needs FELUCCA_FLASH */
#endif
#ifndef FELUCCA_OTA_DRYRUN
#define FELUCCA_OTA_DRYRUN 0
#endif
#ifndef FELUCCA_CDC
#define FELUCCA_CDC 0            /* USB serial console (not used) */
#endif
#ifndef FELUCCA_CDC_DEFAULT
#define FELUCCA_CDC_DEFAULT 0
#endif
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 1            /* USB audio: the master output to the computer, 44.1 or 48 kHz */
#endif
#ifndef FELUCCA_UAC_TONE
#define FELUCCA_UAC_TONE 0
#endif
#ifndef FELUCCA_UART
#define FELUCCA_UART 1           /* TRS MIDI IN on UART1 / PH8 */
#endif
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_947"    /* package identity (build.py: the .fwsc marker string) */
#endif
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "auto-chre 0.2"
#endif
#if FELUCCA_OTA && !FELUCCA_FLASH
#error "FELUCCA_OTA needs FELUCCA_FLASH"
#endif

/* ------------------------------------------------------------- HAL --- */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"

/* ----------------------------------------------------------- system --- */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* slot store before the index update */
static volatile uint32_t fm1_ms;  /* milliseconds since boot (TIMER4-based, TIMER5 ISR in main.c) */
/* boot-loop guard (main.c): two boots in a row that die in the first 30 s -> UBOOT */
#define BOOTGUARD_MAGIC 0x42475244u
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));
#define GR_YIELD() fm1_wdt_feed()

/* ----------------------------------------------------- base, display --- */
#include "libc.c"
#include "lcd.c"
#include "gfx.c"

/* ------------------------------------------------------------- core --- */
#include "ac_core.h"
static volatile int32_t gr_master_q12 = 4096;     /* the MASTER pot (master.c), 4096 = unity */

/* ------------------------------------------------------- USB, MIDI --- */
#include "usb.c"
#if FELUCCA_UART
#include "midi_uart.c"
#endif

/* ----------------------------------------------------------- sound --- */
#include "ac_voice.c"
#include "ac_seq.c"
#include "ac_fx.c"
#include "ac_midi.c"
#include "audio.c"

/* -------------------------------------------------------- storage, update --- */
#if FELUCCA_FLASH
#include "fm1_flash.h"
#include "storage_hw.c"
#include "storage.c"
#endif
#if FELUCCA_OTA
#include "ota.c"
#include "ota_hw.c"
#endif

/* ---------------------------------------------------- UI, state, panel --- */
#include "panel.c"
#include "ac_state.c"
#include "ac_ui.c"
#include "ac_input.c"
#include "master.c"
#include "main.c"
