/*
Copyright 2023 YANG <drk@live.com>

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

#include "hal.h"
#include "ch.h"
#include "stdint.h"
#include "c1.h"
#include "quantum.h"
#include "pico/bootrom.h" 
#include "hardware/watchdog.h"


void reboot(bool bootloader)
{
    if (bootloader) {
        reset_usb_boot(0, 0); 
    } else {
        watchdog_reboot(0, 0, 10);
        while (1) {
            continue;
        }
    }
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    //static uint8_t mod_keys_registered;
    //uint8_t pressed_mods = get_mods();
    switch (keycode) {
        case 0x5c00: // via/vial reset to bootloader
            if (record->event.pressed) {
                reboot(1);
            }
            return false;
#if 0
        case 0x7820 ... 0x7833:
            if (record->event.pressed) {
                rgbinfo_display_on = 30;
            }
            return true;
#endif
        default:
            return true; // Process all other keycodes normally
    }
}

/* LShift+RShift+LCtrl+B to Bootloader */
#include "command.h"

bool command_extra(uint8_t code)
{
    uint8_t pressed_mods = get_mods();
    clear_keyboard();
    switch (code) {
        case KC_B:
            ;
            wait_us(500*1000);
            reboot(pressed_mods & MOD_BIT(KC_LCTRL));
            break;
        case KC_O:
            display_power_toggle();
            return true;
        case KC_G:
            next_gif_id();
            return true;

        default:
            return false;   // yield to default command
    }
    return true;
}

void restart_usb_driver(USBDriver *usbp) {
    reboot(0);
}

// Snap Tap / SOCD
static const uint8_t SOCD_KEY[2][2] = {
    { KC_W, KC_S },
    { KC_A, KC_D }
};

bool socd_key_state[2][2] = { {0,0},{0,0}};

void post_process_record_user(uint16_t keycode, keyrecord_t *record)
{
    if (keycode >= 0x7e00 && keycode <= 0x7e03) {
        uint8_t key = keycode - 0x7e00;
        uint8_t k_group = key&1;
        uint8_t k_num = key>>1;
        uint8_t k_op_num = k_num?0:1;
        if (record->event.pressed) {
            socd_key_state[k_group][k_num] = 1;
            if (socd_key_state[k_group][k_op_num]) {
                unregister_code(SOCD_KEY[k_group][k_op_num]);
            }
            register_code(SOCD_KEY[k_group][k_num]);
        } else {
            socd_key_state[k_group][k_num] = 0;
            unregister_code(SOCD_KEY[k_group][k_num]);
            if (socd_key_state[k_group][k_op_num]) {
                register_code(SOCD_KEY[k_group][k_op_num]);
            }
        }
    }
    if (keycode == 0x7e04 && record->event.pressed) {
        command_extra(KC_G); //display gif
    }
    if (keycode == 0x7e05 && record->event.pressed) {
        command_extra(KC_O); //display ON / OFF
    }
}
