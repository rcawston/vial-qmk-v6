#include "qp.h"
#include "qp_comms.h"
#include "c1.h"

#include "qp_gc9107_opcodes.h"
#include "qgf.h"
#include "gfx/boot.qgf.h"
#include "gfx/boot2.qgf.h"

#include "gfx/robotomono20.qff.h"

#include "color.h"
#include "config.h"
#include "eeconfig.h"
#include "timer.h"

painter_device_t display;
static deferred_token my_anim;
static bool gif_started = 0;
static uint8_t prev_gif_id = 99;
static uint8_t now_gif_id = 1;
static bool now_lcd_off = 0;

painter_font_handle_t my_font;
painter_image_handle_t playing_gif;
static uint8_t boot_displaying = 1;


/* rgb info */
extern rgblight_config_t rgblight_config;
extern uint16_t kb_idle_timer;
extern uint8_t indicator_state;


user_eeconfig_t user_eeconfig;

static const uint32_t runtime_gif_addr[] = {
    (0x1040 << 16),
    (0x1050 << 16),
    (0x1060 << 16),
    (0x1080 << 16),
    (0x10A0 << 16),
    (0x10C0 << 16),
};

static user_eeconfig_t make_default_user_eeconfig(void) {
    user_eeconfig_t config = {.raw = 0};
    config.lcd_off         = false;
    config.gif_id          = 1;
    return config;
}

void eeconfig_init_user(void) {
    user_eeconfig = make_default_user_eeconfig();
    eeconfig_update_user(user_eeconfig.raw);
}

void user_eeconfig_sanitize(void) {
    bool changed = false;

    if (user_eeconfig.raw == 0xFFFFFFFFu) {
        user_eeconfig = make_default_user_eeconfig();
        changed       = true;
    }

    if (user_eeconfig.gif_id == 0 || user_eeconfig.gif_id > 5) {
        user_eeconfig.gif_id = make_default_user_eeconfig().gif_id;
        changed              = true;
    }

    if (changed) {
        eeconfig_update_user(user_eeconfig.raw);
    }
}

static bool qgf_buffer_looks_valid(const void *buffer) {
    if (buffer == NULL) {
        return false;
    }

    const qgf_graphics_descriptor_v1_t *descriptor = (const qgf_graphics_descriptor_v1_t *)buffer;
    if (descriptor->header.type_id != QGF_GRAPHICS_DESCRIPTOR_TYPEID || descriptor->header.neg_type_id != (uint8_t)~QGF_GRAPHICS_DESCRIPTOR_TYPEID) {
        return false;
    }

    if (descriptor->magic != QGF_MAGIC || descriptor->qgf_version != 1) {
        return false;
    }

    if (descriptor->total_file_size < sizeof(qgf_graphics_descriptor_v1_t) || descriptor->total_file_size > (2 * 1024 * 1024)) {
        return false;
    }

    if (descriptor->neg_total_file_size != ~descriptor->total_file_size) {
        return false;
    }

    return descriptor->image_width > 0 && descriptor->image_height > 0 && descriptor->frame_count > 0;
}

static bool set_active_runtime_image(uint8_t gif_id) {
    painter_image_handle_t next_image = NULL;

    if (gif_id < (sizeof(runtime_gif_addr) / sizeof(runtime_gif_addr[0])) && qgf_buffer_looks_valid((const void *)runtime_gif_addr[gif_id])) {
        next_image = qp_load_image_mem((const void *)runtime_gif_addr[gif_id]);
    }

    qp_stop_animation(my_anim);
    if (playing_gif != NULL) {
        qp_close_image(playing_gif);
    }

    playing_gif  = next_image;
    gif_started  = false;
    kb_idle_timer = 0;
    return playing_gif != NULL;
}

void display_power_toggle(void) {
    user_eeconfig.lcd_off ^= 1;
    eeconfig_update_user(user_eeconfig.raw);
    now_lcd_off = user_eeconfig.lcd_off;
    if (now_lcd_off) {
        qp_stop_animation(my_anim);
        prev_gif_id = 99;
        palSetLine(17U); //power off
    } else {
        palClearLine(17U); //power on
    }
}

void next_gif_id(void) {
    now_gif_id++;
    if (now_gif_id > 5) now_gif_id = 1;
    user_eeconfig.gif_id = now_gif_id;
    eeconfig_update_user(user_eeconfig.raw);
}

//user config end

typedef struct animation_state_t {
    painter_device_t       device;
    uint16_t               x;
    uint16_t               y;
    painter_image_handle_t image;
    qp_pixel_t             fg_hsv888;
    qp_pixel_t             bg_hsv888;
    uint16_t               frame_number;
    deferred_token         defer_token;
} animation_state_t;

extern deferred_executor_t animation_executors[QUANTUM_PAINTER_CONCURRENT_ANIMATIONS];
extern animation_state_t   animation_states[QUANTUM_PAINTER_CONCURRENT_ANIMATIONS];

void qp_stop_animation_frame(deferred_token anim_token) {
    for (int i = 0; i < QUANTUM_PAINTER_CONCURRENT_ANIMATIONS; ++i) {
        if (animation_states[i].defer_token == anim_token) {
            if (animation_states[i].device != NULL && animation_states[i].frame_number == 1) {
                cancel_deferred_exec_advanced(animation_executors, QUANTUM_PAINTER_CONCURRENT_ANIMATIONS, anim_token);
                animation_states[i].device = NULL;
                gif_started = 0;
            }
            return;
        }
    }
}

void display_init(void)
{
    // LCD Power
    palSetLineMode(17U, PAL_MODE_OUTPUT_PUSHPULL | PAL_RP_PAD_DRIVE12);
    palSetLine(17U); //power off to reset the lcd
    wait_ms(300);
    palClearLine(17U); //power on and wait
    wait_ms(300);

    // Display Init
    display = qp_gc9107_make_spi_device(LCD_HEIGHT, LCD_WIDTH, LCD_CS_PIN, LCD_DC_PIN, LCD_RST_PIN, LCD_SPI_DIVISOR, SPI_MODE);
    qp_init(display, LCD_ROTATION);

    // Display offset
    qp_set_viewport_offsets(display, LCD_OFFSET_X, LCD_OFFSET_Y);

    // Power on display, RGB Test
    // qp_rect(painter_device_t device, uint16_t left, uint16_t top, uint16_t right, uint16_t bottom, uint8_t hue, uint8_t sat, uint8_t val, bool filled);
    qp_power(display, 1);
    qp_rect(display, 0, 0, LCD_HEIGHT, LCD_WIDTH, 0, 0, 0, 1); //default black
    // font
    my_font = qp_load_font_mem(font_robotomono20);

    #if 0
    // testing
    if (my_font != NULL) {
        qp_drawtext(display, 0, 0, my_font, "Hello");
        qp_drawtext(display, 0, 30, my_font, "From");
        qp_drawtext(display, 0, 60, my_font, "QMK!");
        wait_ms(200);
        qp_drawtext(display, 0, 90, my_font, "Booting");
        wait_ms(200);
        qp_drawtext(display, 0, 90, my_font, "Booting.");
        wait_ms(200);
        qp_drawtext(display, 0, 90, my_font, "Booting..");
        wait_ms(200);
        qp_drawtext(display, 0, 90, my_font, "Booting...");
    }
    #endif
    // boot gif
    #ifndef BOOTGIF
    playing_gif = qp_load_image_mem(gfx_boot);
    #else
    playing_gif = qp_load_image_mem(BOOTGIF);
    #endif

    kb_idle_timer = 0;
    gif_started = 0;

}
bool lcd_is_on(void)
{
    return (boot_displaying || (now_lcd_off == 0));
}

void update_gif_task(void) {
    if (boot_displaying) {
        kb_idle_timer = 0;

        if (boot_displaying == 1 && animation_states[0].frame_number > 1) {
            boot_displaying = 2;
        }

        if (boot_displaying == 2 && animation_states[0].frame_number == 1) {
            boot_displaying = 0;
            wait_ms(800);   

            if (user_eeconfig.lcd_off) {
                palSetLine(17U); //power off to reset the lcd
            }

            now_gif_id = user_eeconfig.gif_id;
            now_lcd_off = user_eeconfig.lcd_off;
            prev_gif_id = 99;
            gif_started = 0;
        }
        return;
    } else if (now_lcd_off) {
        return;
    }

    // capslock
    if (indicator_state & 1) {
        if (now_gif_id != 0) {
            now_gif_id = 0;
            set_active_runtime_image(now_gif_id);
        }
    }

    else if (prev_gif_id != now_gif_id) {
        if (now_gif_id == 0) now_gif_id = (prev_gif_id > 10)?1:prev_gif_id;
        else if (now_gif_id > 5) now_gif_id = 1;

        if (set_active_runtime_image(now_gif_id)) {
            if (playing_gif->width == LCD_WIDTH && playing_gif->height == LCD_HEIGHT) {
                wait_ms(100);
            } else {
                qp_rect(display, 0, 0, LCD_HEIGHT, LCD_WIDTH, 0, 0, 0, 1);
                qp_drawtext(display, 0, 30, my_font, "To be");
                qp_drawtext(display, 0, 60, my_font, "uploaded.");
            }
        } else {
            qp_rect(display, 0, 0, LCD_HEIGHT, LCD_WIDTH, 0, 0, 0, 1);
            qp_drawtext(display, 0, 30, my_font, "To be");
            qp_drawtext(display, 0, 60, my_font, "uploaded.");
        }

        prev_gif_id = now_gif_id;
    }
}

void display_task_user(void)
{
    if (!boot_displaying && user_eeconfig.lcd_off) return;

    update_gif_task();

    if (!boot_displaying && kb_idle_timer == 0 && now_gif_id == 1 && animation_states[0].image != NULL) { 
        static uint8_t prev_frame = 0;
        if (animation_states[0].frame_number != prev_frame) {
            animation_states[0].frame_number += 2;
            if (animation_states[0].frame_number >= animation_states[0].image->frame_count) {
                animation_states[0].frame_number = 0;
            }
            prev_frame = animation_states[0].frame_number;
        }
    } else if (gif_started == 0 && playing_gif != NULL) {
        qp_stop_animation(my_anim);
        my_anim = qp_animate(display, 0, 0, playing_gif);
        gif_started = 1;
    }
#if 0
    if (kb_idle_timer >= 1 && now_gif_id == 1) {
        qp_stop_animation_frame(my_anim);
    } else if (gif_started == 0) {
        qp_stop_animation(my_anim);
        my_anim = qp_animate(display, 0, 0, playing_gif);
        gif_started = 1;
    }
#endif
}

void suspend_power_down_user_display(void)
{
    // keep power off
    // LCD Power OFF， Backlight OFF
    if (!now_lcd_off) {
        now_lcd_off = 1;
        qp_stop_animation(my_anim); 
        prev_gif_id = 99;
        palSetLine(17U);
    }
}

void suspend_wakeup_init_user_display(void)
{
    if (now_lcd_off && !user_eeconfig.lcd_off) {
        // Enable Power
        palClearLine(17U);
        wait_ms(200);
        now_lcd_off = 0;
        prev_gif_id = 99;
        gif_started = 0;
    }
}
