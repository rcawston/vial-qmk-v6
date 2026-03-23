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
#include "ws2812.h"
#include "led.h"
#include "rgblight.h"

#include "stdint.h"
#include "quantum.h"

#ifndef LOGIC_INDICATOR_NUM
#define LOGIC_INDICATOR_NUM PHY_INDICATOR_NUM
#endif

#ifndef LED_TYPE
#define LED_TYPE rgb_led_t
#endif

extern rgblight_config_t rgblight_config;
void led_set_user(uint8_t usb_led);
void user_eeconfig_init(void);

static LED_TYPE RGBLIGHT_COLOR_OFF = { .r = 0, .g = 0, .b = 0 };
uint8_t indicator_state = 0;
//save 3 colors
uint8_t indicator_color_config[3];
LED_TYPE indicator_color[3];


LED_TYPE rgbled[PHY_INDICATOR_NUM+RGBLED_NUM];
extern uint8_t rgbinfo_display_on;

void set_rgb_user(uint8_t r, uint8_t g,  uint8_t b)
{
    for (uint8_t i=0; i<(PHY_INDICATOR_NUM+RGBLED_NUM); i++) {
        rgbled[i].r = r;
        rgbled[i].g = g;
        rgbled[i].b = b;
    }
    ws2812_setleds((ws2812_led_t *)rgbled, PHY_INDICATOR_NUM + RGBLED_NUM);
}

void rgblight_user_init(void)
{
#ifdef CONFIG_BOOT_TEST_RGB
    //test all leds
    set_rgb_user(32, 0, 0);
    wait_ms(300);
    set_rgb_user(0, 32, 0);
    wait_ms(300);
    set_rgb_user(0, 0, 32);
    wait_ms(300);
    set_rgb_user(0, 0, 0);
#else
    // all leds off
    set_rgb_user(0, 0, 0);
#endif
}

void rgblight_call_driver(LED_TYPE *start_led, uint8_t num_leds) {
    // keep indicator color
    for (uint8_t i=0; i<PHY_INDICATOR_NUM; i++) {
        if (indicator_state & (1<<i)) {
            rgbled[i] = indicator_color[i];
        } else {
            rgbled[i] = RGBLIGHT_COLOR_OFF;
        }
    }


    memcpy(&rgbled[PHY_INDICATOR_NUM], start_led, RGBLED_NUM*3);
#ifdef RGB_EXTRA_PROCESS_ENABLE
    rgb_extra_process(rgbled);
#endif

    ws2812_setleds((ws2812_led_t *)rgbled, PHY_INDICATOR_NUM + RGBLED_NUM);
}

extern bool bootmagic_checked;

bool led_update_user(led_t usb_led) {
    led_set_user(usb_led.raw);
    return true;
}

void led_set_user(uint8_t usb_led)
{
    indicator_state = 0;
#ifdef INDICATOR_FUNCT
    static uint8_t indicator_funct[LOGIC_INDICATOR_NUM] = INDICATOR_FUNCT;
    for (uint8_t i=0; i<LOGIC_INDICATOR_NUM; i++) {
        if (usb_led & indicator_funct[i]) {
            indicator_state |= (1<<i);
        }
    }

    if (rgblight_config.mode == 1) rgblight_mode_noeeprom(rgblight_config.mode);
    if (bootmagic_checked) rgblight_set(); //set rgb even when rgblight.enable=0
#endif
}
void hook_keyboard_loop(void)
{
    static uint8_t rgb_inited = 0;
    if (rgb_inited == 0 && bootmagic_checked) { 
        user_eeconfig_init();
        rgblight_user_init();
        rgb_inited = 1;
    }
}
