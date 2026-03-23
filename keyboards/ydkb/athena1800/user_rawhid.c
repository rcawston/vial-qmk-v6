/*
Copyright 2025 YANG

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
#include "quantum.h"
#include "raw_hid.h"
#include "via.h"
#include "c1.h"
#include "qgf.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
//#include "pico/multicore.h"

#ifndef LED_TYPE
#define LED_TYPE rgb_led_t
#endif

extern uint8_t indicator_color_config[];
extern LED_TYPE indicator_color[];
void user_eeconfig_init(void);
extern user_eeconfig_t user_eeconfig;

void rprint(char *msg) {
    return;
    //0xfdee
    uint8_t eeee_buf[32] = {0};
    uint8_t msg_len = strlen(msg);
    if (msg_len > 30) msg_len = 30;
    memcpy(&eeee_buf[2], msg, msg_len);
    eeee_buf[0] = 0xFD;
    eeee_buf[1] = 0xEE;
    raw_hid_send(eeee_buf, 32);
}

void raw_hid_send_bouncing_key(uint8_t row, uint8_t col) {
    return;
    //0xfdbc
    uint8_t buf[32] = {0};
    buf[0] = 0xFD;
    buf[1] = 0xBC;
    buf[2] = row;
    buf[3] = col;
    raw_hid_send(buf, 32);
}


static void call_flash_range_program(void *param) {
    uint32_t offset = ((uintptr_t*)param)[0];
    const uint8_t *data = (const uint8_t *)((uintptr_t*)param)[1];
    flash_range_program(offset, data, 256);
}

enum {
    ATHENA_HID_PREFIX = 0xFD,
    ATHENA_GIF_GET_INFO = 0xA0,
    ATHENA_GIF_BEGIN_UPLOAD = 0xA1,
    ATHENA_GIF_WRITE_CHUNK = 0xA2,
    ATHENA_GIF_FINISH_UPLOAD = 0xA3,
    ATHENA_GIF_SET_ACTIVE_SLOT = 0xA4,
};

enum {
    ATHENA_GIF_STATUS_OK = 0,
    ATHENA_GIF_STATUS_BAD_CMD = 1,
    ATHENA_GIF_STATUS_BAD_SLOT = 2,
    ATHENA_GIF_STATUS_BAD_SIZE = 3,
    ATHENA_GIF_STATUS_NOT_ACTIVE = 4,
    ATHENA_GIF_STATUS_BAD_OFFSET = 5,
    ATHENA_GIF_STATUS_BAD_LENGTH = 6,
    ATHENA_GIF_STATUS_FLASH = 7,
    ATHENA_GIF_STATUS_INVALID_QGF = 8,
};

static const uint32_t gif_slot_addr[] = {
    0x10400000u,
    0x10500000u,
    0x10600000u,
    0x10800000u,
    0x10A00000u,
    0x10C00000u,
};

static const uint32_t gif_slot_size[] = {
    0x00100000u,
    0x00100000u,
    0x00200000u,
    0x00200000u,
    0x00200000u,
    0x00200000u,
};

typedef struct {
    bool     active;
    uint8_t  slot;
    uint32_t slot_addr;
    uint32_t slot_offset;
    uint32_t slot_size;
    uint32_t total_size;
    uint32_t next_offset;
    uint32_t page_base;
    bool     page_dirty;
    uint8_t  page_data[FLASH_PAGE_SIZE];
} athena_gif_upload_state_t;

static athena_gif_upload_state_t gif_upload = {
    .page_base = UINT32_MAX,
};

static void athena_gif_reset_state(void) {
    gif_upload.active     = false;
    gif_upload.slot       = 0;
    gif_upload.slot_addr  = 0;
    gif_upload.slot_offset = 0;
    gif_upload.slot_size  = 0;
    gif_upload.total_size = 0;
    gif_upload.next_offset = 0;
    gif_upload.page_base  = UINT32_MAX;
    gif_upload.page_dirty = false;
    memset(gif_upload.page_data, 0xFF, sizeof(gif_upload.page_data));
}

static bool athena_gif_slot_valid(uint8_t slot) {
    return slot < (sizeof(gif_slot_addr) / sizeof(gif_slot_addr[0]));
}

static bool athena_gif_validate_qgf(const void *buffer, uint32_t expected_size) {
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

    if (descriptor->neg_total_file_size != ~descriptor->total_file_size) {
        return false;
    }

    if (descriptor->total_file_size != expected_size || descriptor->total_file_size > gif_upload.slot_size || descriptor->total_file_size < sizeof(qgf_graphics_descriptor_v1_t)) {
        return false;
    }

    return descriptor->image_width > 0 && descriptor->image_height > 0 && descriptor->frame_count > 0;
}

static bool athena_gif_flush_page(void) {
    if (!gif_upload.page_dirty || gif_upload.page_base == UINT32_MAX) {
        return true;
    }

    c1_before_flash_operation();
    flash_range_program(gif_upload.slot_offset + gif_upload.page_base, gif_upload.page_data, FLASH_PAGE_SIZE);
    c1_after_flash_operation();

    gif_upload.page_dirty = false;
    return true;
}

static uint8_t athena_gif_begin_upload(uint8_t slot, uint32_t total_size) {
    athena_gif_reset_state();

    if (!athena_gif_slot_valid(slot)) {
        return ATHENA_GIF_STATUS_BAD_SLOT;
    }

    if (total_size == 0 || total_size > gif_slot_size[slot]) {
        return ATHENA_GIF_STATUS_BAD_SIZE;
    }

    gif_upload.active      = true;
    gif_upload.slot        = slot;
    gif_upload.slot_addr   = gif_slot_addr[slot];
    gif_upload.slot_offset = gif_upload.slot_addr - XIP_BASE;
    gif_upload.slot_size   = gif_slot_size[slot];
    gif_upload.total_size  = total_size;
    gif_upload.page_base   = UINT32_MAX;
    memset(gif_upload.page_data, 0xFF, sizeof(gif_upload.page_data));

    uint32_t erase_size = (total_size + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
    c1_before_flash_operation();
    flash_range_erase(gif_upload.slot_offset, erase_size);
    c1_after_flash_operation();
    return ATHENA_GIF_STATUS_OK;
}

static uint8_t athena_gif_write_chunk(uint32_t offset, const uint8_t *chunk, uint8_t chunk_len) {
    if (!gif_upload.active) {
        return ATHENA_GIF_STATUS_NOT_ACTIVE;
    }

    if (chunk_len == 0 || chunk_len > 25) {
        return ATHENA_GIF_STATUS_BAD_LENGTH;
    }

    if (offset != gif_upload.next_offset) {
        return ATHENA_GIF_STATUS_BAD_OFFSET;
    }

    if (offset + chunk_len > gif_upload.total_size) {
        return ATHENA_GIF_STATUS_BAD_SIZE;
    }

    uint32_t pos = offset;
    uint8_t  idx = 0;
    while (idx < chunk_len) {
        uint32_t target_page = pos & ~(uint32_t)(FLASH_PAGE_SIZE - 1u);
        uint32_t page_index  = pos & (uint32_t)(FLASH_PAGE_SIZE - 1u);
        uint32_t copy_len    = MIN((uint32_t)(chunk_len - idx), (uint32_t)(FLASH_PAGE_SIZE - page_index));

        if (gif_upload.page_base != target_page) {
            if (!athena_gif_flush_page()) {
                return ATHENA_GIF_STATUS_FLASH;
            }
            gif_upload.page_base = target_page;
            memset(gif_upload.page_data, 0xFF, sizeof(gif_upload.page_data));
        }

        memcpy(&gif_upload.page_data[page_index], &chunk[idx], copy_len);
        gif_upload.page_dirty = true;
        pos += copy_len;
        idx += copy_len;

        if (page_index + copy_len == FLASH_PAGE_SIZE) {
            if (!athena_gif_flush_page()) {
                return ATHENA_GIF_STATUS_FLASH;
            }
            gif_upload.page_base = UINT32_MAX;
            memset(gif_upload.page_data, 0xFF, sizeof(gif_upload.page_data));
        }
    }

    gif_upload.next_offset = pos;
    return ATHENA_GIF_STATUS_OK;
}

static uint8_t athena_gif_finish_upload(uint8_t set_active_slot) {
    if (!gif_upload.active) {
        return ATHENA_GIF_STATUS_NOT_ACTIVE;
    }

    if (gif_upload.next_offset != gif_upload.total_size) {
        return ATHENA_GIF_STATUS_BAD_SIZE;
    }

    if (!athena_gif_flush_page()) {
        athena_gif_reset_state();
        return ATHENA_GIF_STATUS_FLASH;
    }

    if (!athena_gif_validate_qgf((const void *)gif_upload.slot_addr, gif_upload.total_size)) {
        athena_gif_reset_state();
        return ATHENA_GIF_STATUS_INVALID_QGF;
    }

    if (set_active_slot && gif_upload.slot > 0) {
        user_eeconfig.gif_id = gif_upload.slot;
        eeconfig_update_user(user_eeconfig.raw);
    }

    athena_gif_reset_state();
    return ATHENA_GIF_STATUS_OK;
}

static uint8_t athena_gif_set_active_slot(uint8_t slot) {
    if (!athena_gif_slot_valid(slot) || slot == 0) {
        return ATHENA_GIF_STATUS_BAD_SLOT;
    }

    user_eeconfig.gif_id = slot;
    eeconfig_update_user(user_eeconfig.raw);
    return ATHENA_GIF_STATUS_OK;
}

void raw_hid_receive_kb(uint8_t *data, uint8_t length) {
    uint8_t *command_id = &(data[0]);
    if (*command_id == 0xFD) {
        data[2] = ATHENA_GIF_STATUS_BAD_CMD;

        if (data[1] == ATHENA_GIF_GET_INFO) {
            data[2] = ATHENA_GIF_STATUS_OK;
            data[3] = (uint8_t)(sizeof(gif_slot_addr) / sizeof(gif_slot_addr[0]));
            data[4] = user_eeconfig.gif_id;
            data[5] = user_eeconfig.lcd_off;
            uint32_t flash_size = PICO_FLASH_SIZE_BYTES;
            memcpy(&data[6], &flash_size, sizeof(flash_size));
        } else if (data[1] == ATHENA_GIF_BEGIN_UPLOAD) {
            uint32_t total_size = 0;
            memcpy(&total_size, &data[3], sizeof(total_size));
            data[2] = athena_gif_begin_upload(data[2], total_size);
        } else if (data[1] == ATHENA_GIF_WRITE_CHUNK) {
            uint32_t offset = 0;
            memcpy(&offset, &data[2], sizeof(offset));
            data[2] = athena_gif_write_chunk(offset, &data[7], data[6]);
        } else if (data[1] == ATHENA_GIF_FINISH_UPLOAD) {
            data[2] = athena_gif_finish_upload(data[2]);
        } else if (data[1] == ATHENA_GIF_SET_ACTIVE_SLOT) {
            data[2] = athena_gif_set_active_slot(data[2]);
        } else if (data[1] == 0xF1) {
            // 0xF1: write
            if (data[4] == 0) {
                //data start
                memset(gif_upload.page_data, 0, sizeof(gif_upload.page_data));
            }
            for (uint8_t i=0; i<27; i++) {
                uint16_t target = data[4] + i;
                if (target < 256) {
                    gif_upload.page_data[target] = data[5+i];
                } else {
                    uint32_t offset = (data[2] << 16) | (data[3] << 8);
                    if (offset < 0x400000) return;
                    break;
                }
            }
        }
    }
}

//after set layout command
void via_set_layout_options_after(void)
{
    user_eeconfig_init();
}

#define DEBOUNCE_DN(x) (uint8_t)(~(0x80 >> x))
#define DEBOUNCE_UP(x) (uint8_t)(0x80 >> x)
extern uint8_t now_debounce_dn_mask;
extern uint8_t now_debounce_up_mask;
static uint8_t debounce_dn_level[3] = {DEBOUNCE_DN(1), DEBOUNCE_DN(3), DEBOUNCE_DN(6)};
static uint8_t debounce_up_level[3] = {DEBOUNCE_UP(4), DEBOUNCE_UP(5), DEBOUNCE_UP(7)};

void update_debounce_level(uint8_t level) {
    level = level & 0b11;
    now_debounce_dn_mask = debounce_dn_level[level];
    now_debounce_up_mask = debounce_up_level[level];
    xprintf("\n debounce dn: %08b, up:%08b", now_debounce_dn_mask, now_debounce_up_mask);
}

void user_eeconfig_init(void)
{
    static const uint8_t indicator_hue_preset[8] = {0, 21, 42, 85, 127, 170, 212, 255};
    #ifdef INDICATOR_VAL
    static uint8_t val = INDICATOR_VAL;
    #else 
    static uint8_t val = 255;
    #endif

    uint16_t layout_value = via_get_layout_options();
    for (uint8_t i=0; i<3; i++) {
        indicator_color_config[i] = (layout_value & 0b111);
        uint8_t hue = indicator_hue_preset[ indicator_color_config[i] ];
        layout_value >>= 3;
        if (hue == 255) indicator_color[i] = (LED_TYPE){0, 0, 0};
        else            indicator_color[i] = hsv_to_rgb((HSV){hue, 255, val});
        if (i < 2) xprintf("\n indicator %d R: %d, G: %d, B:%d", i, indicator_color[i].r, indicator_color[i].g, indicator_color[i].b);
    }
    update_debounce_level(indicator_color_config[2]);
    led_wakeup();
    rprint("Layout set change\n");
}
