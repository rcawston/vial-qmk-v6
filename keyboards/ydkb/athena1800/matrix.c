/*
Copyright 2022 YANG <drk@live.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* rough code */

#include "ch.h"
#include "hal.h"

/*
 * scan matrix
 */
#include "eeconfig.h"
#include "action.h"
#include "debug.h"
#include "timer.h"
#include "util.h"
#include "matrix.h"
#include "debounce_pk.h"
#include "wait.h"
#include "switch_board.h"
#include "c1.h"


#define SHOW_BOUNCE_DN
#define SHOW_BOUNCE_UP

extern debug_config_t debug_config;

bool bootmagic_checked = 0;
uint16_t kb_idle_timer = 2;

static matrix_row_t matrix[MATRIX_ROWS] = {0};
static uint8_t matrix_debouncing[MATRIX_ROWS][MATRIX_COLS] = {0};
uint8_t now_debounce_dn_mask = DEBOUNCE_DN_MASK;
uint8_t now_debounce_up_mask = DEBOUNCE_UP_MASK;

static void select_key(uint8_t mode);
static uint8_t get_key(void);
static void init_cols(void);
void hook_keyboard_loop(void);
void raw_hid_send_bouncing_key(uint8_t row, uint8_t col);
void reboot(bool bootloader);
__attribute__ ((weak))
void matrix_scan_user(void) {}

__attribute__ ((weak))
void matrix_scan_kb(void)
{
    matrix_scan_user();
    hook_keyboard_loop();
}

bool is_sc_leds_mcu = 0;
extern user_eeconfig_t user_eeconfig;

void matrix_init(void)
{

    user_eeconfig.raw = eeconfig_read_user();
    user_eeconfig_sanitize();

    //debug_config.enable = 1;
    //debug_config.matrix = 1;

    //user_eeconfig_init();
    init_cols();
}

#ifdef PREVENT_KEYIO_GND
static bool process_key_press = 0;
bool should_process_keypress(void) {
    return process_key_press;
}
#endif

uint8_t matrix_scan(void)
{
#ifdef PREVENT_KEYIO_GND
    uint8_t matrix_keys_idle = 0;
#endif

    matrix_scan_kb(); // use this to run hook_keyboard_loop()

    select_key(0);
    for (uint8_t row=0; row<MATRIX_ROWS; row++) {
        for (uint8_t col=0; col<MATRIX_COLS; col++) {
            uint8_t *debounce = &matrix_debouncing[row][col];

            uint8_t key = get_key();
            *debounce = (*debounce >> 1) | key;
            //select next key
            select_key(1);
            if (1) {
                matrix_row_t *p_row = &matrix[row];
                matrix_row_t col_mask = ((matrix_row_t)1 << col);

                if        (*debounce > now_debounce_dn_mask) {  //debounce KEY DOWN 
                    *p_row |=  col_mask;
                    kb_idle_timer = 0;
                } else if (*debounce < now_debounce_up_mask) { //debounce KEY UP
                    *p_row &= ~col_mask;
                  #ifdef PREVENT_KEYIO_GND
                    matrix_keys_idle++;
                  #endif
                }

                // A simple judgment to determine if the key is bouncing.
                bool bouncing = 0;
              #ifdef SHOW_BOUNCE_DN
                bouncing = (*debounce >= 0b10001000 && *debounce <= 0b10111111);
              #endif
              #ifdef SHOW_BOUNCE_UP
                uint8_t db_up = ~(*debounce);
                bouncing |= (db_up >= 0b10001000 && db_up <= 0b10111111);
              #endif
                if (bouncing) {
                    xprintf("\nKey(%d,%d) bounce %08b!", row, col, *debounce);
                    raw_hid_send_bouncing_key(row, col);
                }
            }
        }
    }

#ifdef PREVENT_KEYIO_GND
    // to avoid all the keys being down in some cases like KEY is connected to GND.
    process_key_press = (matrix_keys_idle > 0);
#endif

    static uint16_t scan_speed = 0;
    scan_speed++;
    static uint16_t half_second_timer = 0;
    if (half_second_timer != timer_read() && timer_elapsed(half_second_timer) >= 500) {
        half_second_timer = timer_read();
        kb_idle_timer++;

        dprintf("\nScan: %d/s, idle: %d", scan_speed*2, kb_idle_timer);
        scan_speed = 0;
    }



    return 1;
}

inline
bool matrix_is_on(uint8_t row, uint8_t col)
{
    return (matrix[row] & ((matrix_row_t)1<<col));
}

inline
matrix_row_t matrix_get_row(uint8_t row)
{
    return matrix[row];
}

void matrix_print(void)
{

}

uint8_t matrix_key_count(void)
{
    return 0;
}

static void init_cols(void)
{
    // 595 | 5020 pin
    palSetLineMode(6U, GPIO_OUTPUT_MODE);
    palSetLineMode(7U, GPIO_OUTPUT_MODE);
}

 
static uint8_t get_key(void)
{
    return palReadLine(7U)? 0 : 0x80;
}

static void select_key(uint8_t mode)
{
    select_key_ready();
    if (mode == 0) {
        KEY_SDI_OFF();
        for (uint8_t i = 0; i < MATRIX_ROWS * MATRIX_COLS; i++) {
            CLOCK_PULSE();
        }
        KEY_SDI_ON();
        CLOCK_PULSE();
    } else {
        KEY_SDI_OFF();
        CLOCK_PULSE();
    }
    //KEYS_LATCH();
    get_key_ready();
    //wait_us(5);
}

#include "eeprom.h"
#include "via.h"

void bootmagic_scan(void)
{
    for (uint8_t i=0; i < (DEBOUNCE_DN * 2); i++) {
        matrix_scan();
        wait_ms(2);
    }

    //check result
    uint8_t keys_down_pos[3] = {0xff, 0xff, 0xff};
    uint8_t i = 0;
    for (uint8_t row=0; row<MATRIX_ROWS; row++) {
        for (uint8_t col=0; col<MATRIX_COLS; col++) {
            if (matrix_get_row(row) & (1<<col)) {
                keys_down_pos[i] = row * MATRIX_COLS + col;
                if (i < 2) i++;
            }
        }
    }

    if (keys_down_pos[0] == 0) { 
        if (keys_down_pos[1] == 0xff) {
            // only esc down
            // enter_bootloader();
            reboot(1);
        } else if (keys_down_pos[2] == 0xff) {
            //two keys down. if the other key is KC_E, clear eeprom.
            if (eeprom_read_byte((const uint8_t *)(uintptr_t)(VIA_EEPROM_CONFIG_END + 1 + keys_down_pos[1] * 2)) == KC_E) {
                eeconfig_init_via();
            }
        }
    }
    bootmagic_checked = 1;
}
