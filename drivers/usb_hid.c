/*
 * Pixel Neko Next - USB HID Subsystem (Keyboard and Mouse)
 *
 * Derived from FreeBSD sys/dev/usb/input/ukbd.c, ums.c and usb_hid.c
 * Copyright (c) 2008 Hans Petter Selasky <hselasky@FreeBSD.org>
 * Copyright (c) 1998, 2004 Lennart Augustsson <lennart@augustsson.net>
 * Copyright (c) 2026 Pixel Neko Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#include "usb_hid.h"
#include "usb_core.h"
#include "keyboard.h"
#include "mouse.h"
#include "log.h"
#include "wm.h"

#define HID_MAX_DEVICES 8

#define HID_TYPE_UNKNOWN  0
#define HID_TYPE_KEYBOARD 1
#define HID_TYPE_MOUSE    2

#define HID_VMWARE_VENDOR_ID   0x0E0FU
#define HID_VMWARE_MOUSE_PID   0x0003U

#define HID_REQ_GET_REPORT     0x01U
#define HID_REQ_SET_IDLE       0x0AU
#define HID_REPORT_INPUT       0x01U

#define HID_LOCAL_USAGE_MAX 32

typedef struct {
    int valid;
    int report_id;
    int x_bit;
    int x_size;
    int x_min;
    int x_max;
    int x_relative;
    int y_bit;
    int y_size;
    int y_min;
    int y_max;
    int y_relative;
    int button_bit;
    int button_size;
    int button_count;
} hid_mouse_layout_t;

typedef struct {
    usb_device_t*   dev;
    usb_endpoint_t* ep;
    int             type;
    int             report_len;
    int             report_seen;
    int             button_reports_logged;
    unsigned int    report_count;
    int             last_debug_tick;
    int             control_poll_enabled;
    int             last_control_tick;
    int             control_reports_logged;
    hid_mouse_layout_t mouse_layout;
    unsigned char   prev_report[16];
} hid_device_t;

static unsigned int hid_item_value(const unsigned char* data, int size)
{
    unsigned int value = 0;
    for (int i = 0; i < size; i++) {
        value |= (unsigned int)data[i] << (i * 8);
    }
    return value;
}

static int hid_item_signed_value(const unsigned char* data, int size)
{
    unsigned int value = hid_item_value(data, size);
    if (size == 1 && (value & 0x80U)) return (int)(value | 0xFFFFFF00U);
    if (size == 2 && (value & 0x8000U)) return (int)(value | 0xFFFF0000U);
    return (int)value;
}

static void hid_add_usage(unsigned short* usages, unsigned short* pages,
                          int* count, unsigned int page, unsigned int usage)
{
    if (*count >= HID_LOCAL_USAGE_MAX) return;
    pages[*count] = (unsigned short)page;
    usages[*count] = (unsigned short)usage;
    (*count)++;
}

static void hid_parse_mouse_layout(const unsigned char* desc, int len,
                                   hid_mouse_layout_t* layout)
{
    unsigned int usage_page = 0;
    unsigned int report_size = 0;
    unsigned int report_count = 0;
    unsigned int report_id = 0;
    int logical_min = 0;
    int logical_max = 0;
    int report_bit = 0;

    unsigned short usages[HID_LOCAL_USAGE_MAX];
    unsigned short pages[HID_LOCAL_USAGE_MAX];
    int usage_count = 0;
    unsigned int usage_min = 0;
    unsigned int usage_max = 0;
    unsigned int usage_min_page = 0;
    unsigned int usage_max_page = 0;
    int has_usage_range = 0;

    usb_memset(layout, 0, sizeof(*layout));
    int offset = 0;
    while (offset < len) {
        unsigned char prefix = desc[offset++];
        if (prefix == 0xFEU) {
            if (offset + 2 > len) break;
            int long_size = desc[offset++];
            offset++;
            if (long_size < 0 || offset + long_size > len) break;
            offset += long_size;
            continue;
        }

        int size = prefix & 3;
        if (size == 3) size = 4;
        if (offset + size > len) break;
        const unsigned char* data = &desc[offset];
        unsigned int value = hid_item_value(data, size);
        int type = (prefix >> 2) & 3;
        int tag = (prefix >> 4) & 0x0F;
        offset += size;

        if (type == 1) {
            if (tag == 0) usage_page = value;
            else if (tag == 1) logical_min = hid_item_signed_value(data, size);
            else if (tag == 2) logical_max = hid_item_signed_value(data, size);
            else if (tag == 7) report_size = value;
            else if (tag == 8) {
                report_id = value;
                report_bit = 0;
            } else if (tag == 9) {
                report_count = value;
            }
        } else if (type == 2) {
            unsigned int page = usage_page;
            unsigned int usage = value;
            if (size == 4) {
                page = value >> 16;
                usage = value & 0xFFFFU;
            }
            if (tag == 0) {
                hid_add_usage(usages, pages, &usage_count, page, usage);
            } else if (tag == 1) {
                usage_min = usage;
                usage_min_page = page;
                has_usage_range = 1;
            } else if (tag == 2) {
                usage_max = usage;
                usage_max_page = page;
            }
        } else if (type == 0) {
            if (tag == 8 && report_size > 0 && report_count > 0) {
                unsigned int flags = value;
                int constant = (flags & 1U) != 0;
                int variable = (flags & 2U) != 0;
                for (unsigned int field = 0; field < report_count; field++) {
                    unsigned int page = usage_page;
                    unsigned int usage = 0;
                    if (usage_count > 0) {
                        int index = (field < (unsigned int)usage_count) ?
                                    (int)field : usage_count - 1;
                        page = pages[index];
                        usage = usages[index];
                    } else if (has_usage_range) {
                        page = usage_min_page;
                        usage = usage_min + field;
                        if (usage_max_page == usage_min_page &&
                            usage > usage_max) {
                            usage = usage_max;
                        }
                    }

                    if (!constant && variable) {
                        if (page == 0x01U && usage == 0x30U &&
                            !layout->x_size) {
                            layout->report_id = (int)report_id;
                            layout->x_bit = report_bit;
                            layout->x_size = (int)report_size;
                            layout->x_min = logical_min;
                            layout->x_max = logical_max;
                            layout->x_relative = (flags & 4U) != 0;
                        } else if (page == 0x01U && usage == 0x31U &&
                                   !layout->y_size) {
                            if (!layout->x_size) {
                                layout->report_id = (int)report_id;
                            }
                            layout->y_bit = report_bit;
                            layout->y_size = (int)report_size;
                            layout->y_min = logical_min;
                            layout->y_max = logical_max;
                            layout->y_relative = (flags & 4U) != 0;
                        } else if (page == 0x09U && usage >= 1U &&
                                   usage <= 8U) {
                            if (!layout->button_size) {
                                layout->button_bit = report_bit;
                                layout->button_size = (int)report_size;
                            }
                            layout->button_count++;
                        }
                    }
                    report_bit += (int)report_size;
                }
            }
            usage_count = 0;
            usage_min = 0;
            usage_max = 0;
            has_usage_range = 0;
        }
    }

    layout->valid = layout->x_size > 0 && layout->y_size > 0 &&
                    layout->x_size <= 32 && layout->y_size <= 32;
}

static unsigned int hid_extract_bits(const unsigned char* report, int len,
                                     int bit_offset, int size)
{
    unsigned int value = 0;
    if (bit_offset < 0 || size <= 0 || size > 32) return 0;
    for (int bit = 0; bit < size; bit++) {
        int source_bit = bit_offset + bit;
        int byte = source_bit >> 3;
        if (byte >= len) break;
        value |= ((report[byte] >> (source_bit & 7)) & 1U) << bit;
    }
    return value;
}

static int hid_signed_bits(unsigned int value, int size)
{
    if (size <= 0) return 0;
    if (size >= 32) return (int)value;
    unsigned int sign = 1U << (size - 1);
    if (value & sign) {
        value |= ~((1U << size) - 1U);
    }
    return (int)value;
}

static int hid_mouse_buttons(const hid_device_t* hid,
                             const unsigned char* report, int len)
{
    if (!report || len <= 0) return 0;

    if (hid && hid->dev &&
        hid->dev->vendor == HID_VMWARE_VENDOR_ID &&
        hid->dev->product == HID_VMWARE_MOUSE_PID) {
        return (len > 0) ? (report[0] & 0x07) : 0;
    }

    const hid_mouse_layout_t* layout = hid ? &hid->mouse_layout : (const hid_mouse_layout_t*)0;
    int base_bit = 0;

    if (layout && layout->valid && layout->report_id) {
        if (len < 1 || report[0] != layout->report_id) return 0;
        base_bit = 8;
    }

    if (layout && layout->valid && layout->button_count > 0 &&
        layout->button_size > 0 && layout->button_size <= 8) {
        int buttons = 0;
        for (int i = 0; i < layout->button_count && i < 3; i++) {
            int bit_offset = base_bit + layout->button_bit +
                             i * layout->button_size;
            if ((bit_offset + layout->button_size + 7) / 8 > len) break;
            unsigned int bit = hid_extract_bits(
                report, len, bit_offset, layout->button_size);
            if (bit & 1U) buttons |= 1 << i;
        }
        if (buttons) return buttons;
    }

    int byte = (layout && layout->report_id) ? 1 : 0;
    return (byte < len) ? (report[byte] & 0x07) : 0;
}

static hid_device_t g_hid_devices[HID_MAX_DEVICES];
static int          g_hid_count = 0;
static int          g_kbd_count = 0;
static int          g_mouse_count = 0;
static unsigned int g_active_ctrls = 0;

static int hid_controller_flag(usb_hci_t* hci)
{
    if (!hci) return 0;
    if (hci->kind == USB_HCI_KIND_EHCI) return USB_HID_CTRL_EHCI;
    if (hci->kind == USB_HCI_KIND_UHCI) return USB_HID_CTRL_UHCI;
    return 0;
}

static void hid_recompute_controllers(void)
{
    g_active_ctrls = 0;
    for (int i = 0; i < g_hid_count; i++) {
        g_active_ctrls |= hid_controller_flag(g_hid_devices[i].dev->hci);
    }
}

/* USB HID usage to PS/2 Set 1 make code mapping (0 = none, high bit 0x80 = extended key) */
static const unsigned char hid_to_ps2[256] = {
    /* 0x00 - 0x03 */ 0, 0, 0, 0,
    /* 0x04 - 0x07: a, b, c, d */ 0x1E, 0x30, 0x2E, 0x20,
    /* 0x08 - 0x0B: e, f, g, h */ 0x12, 0x21, 0x22, 0x23,
    /* 0x0C - 0x0F: i, j, k, l */ 0x17, 0x24, 0x25, 0x26,
    /* 0x10 - 0x13: m, n, o, p */ 0x32, 0x31, 0x18, 0x19,
    /* 0x14 - 0x17: q, r, s, t */ 0x10, 0x13, 0x1F, 0x14,
    /* 0x18 - 0x1B: u, v, w, x */ 0x16, 0x2F, 0x11, 0x2D,
    /* 0x1C - 0x1F: y, z, 1, 2 */ 0x15, 0x2C, 0x02, 0x03,
    /* 0x20 - 0x23: 3, 4, 5, 6 */ 0x04, 0x05, 0x06, 0x07,
    /* 0x24 - 0x27: 7, 8, 9, 0 */ 0x08, 0x09, 0x0A, 0x0B,
    /* 0x28 - 0x2B: Return, Esc, Backspace, Tab */ 0x1C, 0x01, 0x0E, 0x0F,
    /* 0x2C - 0x2F: Space, -, =, [ */ 0x39, 0x0C, 0x0D, 0x1A,
    /* 0x30 - 0x33: ], \, non-us#, ; */ 0x1B, 0x2B, 0x2B, 0x27,
    /* 0x34 - 0x37: ', `, ,, . */ 0x28, 0x29, 0x33, 0x34,
    /* 0x38 - 0x39: /, CapsLock */ 0x35, 0x3A,
    /* 0x3A - 0x45: F1 - F12 */ 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    /* 0x46 - 0x48: PrintScreen, ScrollLock, Pause */ 0, 0x46, 0,
    /* 0x49 - 0x4B: Insert, Home, PageUp (extended) */ 0xD2, 0xC7, 0xC9,
    /* 0x4C - 0x4E: Delete, End, PageDown (extended) */ 0xD3, 0xCF, 0xD1,
    /* 0x4F - 0x53: Right, Left, Down, Up, NumLock */ 0xCD, 0xCB, 0xD0, 0xC8, 0x45,
    /* 0x54 - 0x57: Keypad /, *, -, + */ 0xB5, 0x37, 0x4A, 0x4E,
    /* 0x58 - 0x5B: Keypad Enter, 1, 2, 3 */ 0x9C, 0x4F, 0x50, 0x51,
    /* 0x5C - 0x5F: Keypad 4, 5, 6, 7 */ 0x4B, 0x4C, 0x4D, 0x47,
    /* 0x60 - 0x63: Keypad 8, 9, 0, . */ 0x48, 0x49, 0x52, 0x53,
};

static void dispatch_scancode(unsigned char code, int released)
{
    if (code & 0x80) {
        /* Extended key */
        unsigned char base = code & 0x7F;
        keyboard_handle_byte(0xE0);
        keyboard_handle_byte(released ? (base | 0x80) : base);
    } else {
        keyboard_handle_byte(released ? (code | 0x80) : code);
    }
}

static void process_keyboard_report(hid_device_t* hid, const unsigned char* report)
{
    unsigned char old_mod = hid->prev_report[0];
    unsigned char new_mod = report[0];

    /* Check modifier changes */
    const unsigned char mod_codes[8] = {
        0x1D, /* LCtrl */
        0x2A, /* LShift */
        0x38, /* LAlt */
        0xDB, /* LGUI (extended 0x5B) */
        0x9D, /* RCtrl (extended 0x1D) */
        0x36, /* RShift */
        0xB8, /* RAlt (extended 0x38) */
        0xDC  /* RGUI (extended 0x5C) */
    };

    for (int i = 0; i < 8; i++) {
        int old_down = (old_mod >> i) & 1;
        int new_down = (new_mod >> i) & 1;
        if (old_down != new_down) {
            dispatch_scancode(mod_codes[i], !new_down);
        }
    }

    /* Check released keys (present in prev_report but missing in report) */
    for (int i = 2; i < 8; i++) {
        unsigned char usage = hid->prev_report[i];
        if (usage == 0) continue;

        int still_pressed = 0;
        for (int j = 2; j < 8; j++) {
            if (report[j] == usage) {
                still_pressed = 1;
                break;
            }
        }
        if (!still_pressed) {
            unsigned char code = hid_to_ps2[usage];
            if (code) dispatch_scancode(code, 1);
        }
    }

    /* Check pressed keys (present in report but missing in prev_report) */
    for (int i = 2; i < 8; i++) {
        unsigned char usage = report[i];
        if (usage == 0) continue;

        int was_pressed = 0;
        for (int j = 2; j < 8; j++) {
            if (hid->prev_report[j] == usage) {
                was_pressed = 1;
                break;
            }
        }
        if (!was_pressed) {
            unsigned char code = hid_to_ps2[usage];
            if (code) dispatch_scancode(code, 0);
        }
    }

    usb_memcpy(hid->prev_report, report, 8);
}

static void process_mouse_report(hid_device_t* hid, const unsigned char* report, int len)
{
    const hid_mouse_layout_t* layout = &hid->mouse_layout;
    if (!layout->valid) {
        int buttons = hid_mouse_buttons(hid, report, len);
        signed char dx = (len >= 2) ? (signed char)report[1] : 0;
        signed char dy = (len >= 3) ? (signed char)report[2] : 0;
        signed char dz = (len >= 4) ? (signed char)report[3] : 0;
        mouse_handle_usb_report(buttons, dx, dy, dz);
        return;
    }

    int base_bit = 0;
    if (layout->report_id) {
        if (len < 1 || report[0] != layout->report_id) return;
        base_bit = 8;
    }

    int buttons = hid_mouse_buttons(hid, report, len);

    int need_x_bytes = (base_bit + layout->x_bit + layout->x_size + 7) / 8;
    int need_y_bytes = (base_bit + layout->y_bit + layout->y_size + 7) / 8;

    int current_x = 0;
    int current_y = 0;
    mouse_get(&current_x, &current_y, (int*)0);

    /* If relative mouse: delta coordinates */
    if (layout->x_relative && layout->y_relative) {
        int x_value = 0, y_value = 0;
        if (len >= need_x_bytes) {
            unsigned int x_raw = hid_extract_bits(
                report, len, base_bit + layout->x_bit, layout->x_size);
            x_value = (layout->x_min < 0) ?
                      hid_signed_bits(x_raw, layout->x_size) : (int)x_raw;
        }
        if (len >= need_y_bytes) {
            unsigned int y_raw = hid_extract_bits(
                report, len, base_bit + layout->y_bit, layout->y_size);
            y_value = (layout->y_min < 0) ?
                      hid_signed_bits(y_raw, layout->y_size) : (int)y_raw;
        }
        mouse_handle_usb_report(buttons, x_value, y_value, 0);
        return;
    }

    /* Absolute pointing device */
    int screen_x = current_x;
    int screen_y = current_y;

    if (len >= need_x_bytes) {
        unsigned int x_raw = hid_extract_bits(
            report, len, base_bit + layout->x_bit, layout->x_size);
        int x_value = (layout->x_min < 0) ?
                      hid_signed_bits(x_raw, layout->x_size) : (int)x_raw;
        if (layout->x_relative) {
            screen_x += x_value;
        } else if (layout->x_max > layout->x_min) {
            screen_x = (x_value - layout->x_min) * (screen_w - 1) /
                       (layout->x_max - layout->x_min);
        }
    }

    if (len >= need_y_bytes) {
        unsigned int y_raw = hid_extract_bits(
            report, len, base_bit + layout->y_bit, layout->y_size);
        int y_value = (layout->y_min < 0) ?
                      hid_signed_bits(y_raw, layout->y_size) : (int)y_raw;
        if (layout->y_relative) {
            screen_y += y_value;
        } else if (layout->y_max > layout->y_min) {
            screen_y = (y_value - layout->y_min) * (screen_h - 1) /
                       (layout->y_max - layout->y_min);
        }
    }

    mouse_handle_usb_absolute(buttons, screen_x, screen_y);
}

static void apply_vmware_control_report(hid_device_t* hid,
                                        const unsigned char* report, int len,
                                        const char* source)
{
    if (!hid || !report || len <= 0) return;

    int buttons = hid_mouse_buttons(hid, report, len);
    int previous = hid_mouse_buttons(
        hid, hid->prev_report, hid->report_len);

    if (hid->control_reports_logged < 16 ||
        (buttons != previous && hid->button_reports_logged < 64)) {
        if (buttons != previous) hid->button_reports_logged++;
        hid->control_reports_logged++;
        log_printf("[HIDCTL] %s len=%d btn=%d prev=%d raw="
                   "%X %X %X %X %X %X %X %X\n",
                   source, len, buttons, previous,
                   report[0], len > 1 ? report[1] : 0,
                   len > 2 ? report[2] : 0, len > 3 ? report[3] : 0,
                   len > 4 ? report[4] : 0, len > 5 ? report[5] : 0,
                   len > 6 ? report[6] : 0, len > 7 ? report[7] : 0);
    }

    int current_x = 0;
    int current_y = 0;
    mouse_get(&current_x, &current_y, (int*)0);
    mouse_handle_usb_absolute(buttons, current_x, current_y);
    hid->prev_report[0] = report[0];
}

static void recover_interrupt_endpoint(hid_device_t* hid)
{
    if (!hid || !hid->dev || !hid->ep) return;

    unsigned short endpoint = (unsigned short)(hid->ep->address | 0x80U);
    int ret = usb_control_msg(hid->dev,
                              USB_DIR_OUT | USB_RT_STANDARD |
                              USB_RT_ENDPOINT,
                              USB_REQ_CLEAR_FEATURE, 0, endpoint,
                              (void*)0, 0);
    if (ret >= 0) hid->ep->toggle = 0;
}

int usb_hid_attach(usb_device_t* dev)
{
    if (!dev || g_hid_count >= HID_MAX_DEVICES) return -1;

    /* Find Interrupt IN endpoint */
    usb_endpoint_t* ep = (usb_endpoint_t*)0;
    for (int i = 0; i < dev->endpoint_count; i++) {
        if (dev->endpoints[i].type == USB_EP_TYPE_INTERRUPT &&
            dev->endpoints[i].direction == USB_DIR_IN) {
            ep = &dev->endpoints[i];
            break;
        }
    }
    if (!ep) {
        log_printf("[HID  ] No interrupt IN endpoint found on dev %u\n", dev->address);
        return -1;
    }

    hid_device_t* hid = &g_hid_devices[g_hid_count];
    usb_memset(hid, 0, sizeof(hid_device_t));
    hid->dev = dev;
    hid->ep = ep;

    int vmware_mouse = dev->vendor == HID_VMWARE_VENDOR_ID &&
                       dev->product == HID_VMWARE_MOUSE_PID;
    unsigned char report_desc[256];
    usb_memset(report_desc, 0, sizeof(report_desc));
    int report_desc_len = 0;
    if (dev->interface_protocol == 0 ||
        dev->interface_protocol == USB_HID_PROTOCOL_MOUSE ||
        vmware_mouse) {
        report_desc_len = usb_control_msg(
            dev, USB_DIR_IN | USB_RT_STANDARD | USB_RT_INTERFACE,
            USB_REQ_GET_DESCRIPTOR, (USB_DT_REPORT << 8) | 0,
            dev->interface_number, report_desc, sizeof(report_desc));
    }
    if (report_desc_len > 0) {
        hid_parse_mouse_layout(report_desc, report_desc_len,
                               &hid->mouse_layout);
    }

    log_printf("[HID  ] descriptor dev %u: class=%X protocol=%X "
               "endpoint=%X packet=%u interval=%u\n",
               dev->address, dev->class_code, dev->interface_protocol,
               ep->address, ep->max_packet, ep->interval);
    log_printf("[HID  ] dev=%u interface=%u endpoints=%d\n",
               dev->address, dev->interface_number, dev->endpoint_count);
    for (int i = 0; i < dev->endpoint_count; i++) {
        log_printf("[HID  ] endpoint[%d] addr=%X dir=%X type=%X "
                   "packet=%u interval=%u\n",
                   i, dev->endpoints[i].address, dev->endpoints[i].direction,
                   dev->endpoints[i].type, dev->endpoints[i].max_packet,
                   dev->endpoints[i].interval);
    }

    /* Detect Keyboard vs Mouse */
    if (vmware_mouse) {
        hid->type = HID_TYPE_MOUSE;
        /*
         * VMware Virtual USB Mouse uses an absolute report:
         *   byte 0: buttons (bit 0: left, bit 1: right, bit 2: middle)
         *   bytes 2..3: absolute X (little-endian, bit 16)
         *   bytes 4..5: absolute Y (little-endian, bit 32)
         */
        hid->mouse_layout.valid = 1;
        hid->mouse_layout.report_id = 0;
        hid->mouse_layout.button_bit = 0;
        hid->mouse_layout.button_size = 1;
        hid->mouse_layout.button_count = 3;
        hid->mouse_layout.x_bit = 16;
        hid->mouse_layout.x_size = 16;
        hid->mouse_layout.x_min = 0;
        hid->mouse_layout.x_max = 32767;
        hid->mouse_layout.x_relative = 0;
        hid->mouse_layout.y_bit = 32;
        hid->mouse_layout.y_size = 16;
        hid->mouse_layout.y_min = 0;
        hid->mouse_layout.y_max = 32767;
        hid->mouse_layout.y_relative = 0;

        hid->report_len = ep->max_packet;
        if (hid->report_len < 6) hid->report_len = 6;
        if (hid->report_len > (int)sizeof(hid->prev_report)) {
            hid->report_len = (int)sizeof(hid->prev_report);
        }
        log_printf("[HID  ] report descriptor len=%d layout=%s "
                   "X=%d:%d Y=%d:%d buttons=%d:%d reportID=%d\n",
                   report_desc_len, hid->mouse_layout.valid ? "mouse" : "generic",
                   hid->mouse_layout.x_bit, hid->mouse_layout.x_size,
                   hid->mouse_layout.y_bit, hid->mouse_layout.y_size,
                   hid->mouse_layout.button_bit, hid->mouse_layout.button_count,
                   hid->mouse_layout.report_id);
        g_mouse_count++;
        log_printf("[HID  ] Attached VMware Virtual USB Mouse on dev %u\n",
                   dev->address);
    } else if (dev->interface_protocol == USB_HID_PROTOCOL_KBD ||
        dev->protocol == USB_HID_PROTOCOL_KBD) {
        hid->type = HID_TYPE_KEYBOARD;
        hid->report_len = 8;
        g_kbd_count++;
        log_printf("[HID  ] Attached USB Keyboard on dev %u (endpoint 0x%X)\n",
                   dev->address, ep->address);
    } else if (dev->interface_protocol == USB_HID_PROTOCOL_MOUSE ||
               dev->protocol == USB_HID_PROTOCOL_MOUSE) {
        hid->type = HID_TYPE_MOUSE;
        hid->report_len = 4;
        g_mouse_count++;
        log_printf("[HID  ] Attached USB Mouse on dev %u (endpoint 0x%X)\n",
                   dev->address, ep->address);
    } else {
        log_printf("[HID  ] Interface protocol %X is not a supported "
                   "boot HID on dev %u\n",
                   dev->interface_protocol, dev->address);
        return -1;
    }

    /*
     * Boot protocol exists only for boot subclass interfaces.
     * Keyboards need Boot Protocol (0) so they report standard 8-byte
     * scancodes. Pointing devices (especially VMware Virtual USB Mouse)
     * must remain in Report Protocol (1) so absolute coordinates and
     * multi-button descriptors are preserved and operational.
     */
    if (dev->interface_protocol == USB_HID_PROTOCOL_KBD) {
        int ret = usb_control_msg(
            dev, USB_DIR_OUT | USB_RT_CLASS | USB_RT_INTERFACE,
            0x0B /* SET_PROTOCOL */, 0, dev->interface_number,
            (void*)0, 0);
        if (ret < 0) {
            log_printf("[HID  ] SET_PROTOCOL rejected on dev %u\n",
                       dev->address);
        }
    } else if (dev->interface_protocol == USB_HID_PROTOCOL_MOUSE && !vmware_mouse) {
        /*
         * Boot mouse interfaces can be switched to Report Protocol (1).
         * VMware Virtual USB Mouse does not implement the boot subclass;
         * querying or sending SET_PROTOCOL to it causes STALL, so leave it untouched.
         */
        usb_control_msg(
            dev, USB_DIR_OUT | USB_RT_CLASS | USB_RT_INTERFACE,
            0x0B /* SET_PROTOCOL */, 1 /* Report */, dev->interface_number,
            (void*)0, 0);
    }

    if (vmware_mouse) {
        int idle_ret = usb_control_msg(
            dev, USB_DIR_OUT | USB_RT_CLASS | USB_RT_INTERFACE,
            HID_REQ_SET_IDLE, 0, dev->interface_number, (void*)0, 0);
        log_printf("[HID  ] VMware SET_IDLE result=%d\n", idle_ret);

        unsigned char control_report[16];
        usb_memset(control_report, 0, sizeof(control_report));
        int control_len = usb_control_msg(
            dev, USB_DIR_IN | USB_RT_CLASS | USB_RT_INTERFACE,
            HID_REQ_GET_REPORT,
            (HID_REPORT_INPUT << 8) | 0, dev->interface_number,
            control_report, hid->report_len);
        if (control_len > 0) {
            hid->control_poll_enabled = 1;
            apply_vmware_control_report(
                hid, control_report, control_len, "initial");
        } else {
            log_printf("[HID  ] VMware GET_REPORT unsupported (result=%d)\n",
                       control_len);
        }
    } else {
        /* Set Idle Rate to 0 (only report changes). */
        usb_control_msg(dev, USB_DIR_OUT | USB_RT_CLASS | USB_RT_INTERFACE,
                        HID_REQ_SET_IDLE, 0, dev->interface_number,
                        (void*)0, 0);
    }

    g_hid_count++;
    hid_recompute_controllers();
    return 0;
}

void usb_hid_detach(usb_device_t* dev)
{
    for (int i = 0; i < g_hid_count; i++) {
        if (g_hid_devices[i].dev != dev) continue;

        if (g_hid_devices[i].type == HID_TYPE_KEYBOARD && g_kbd_count > 0) {
            g_kbd_count--;
        } else if (g_hid_devices[i].type == HID_TYPE_MOUSE &&
                   g_mouse_count > 0) {
            g_mouse_count--;
            if (g_mouse_count == 0) mouse_notify_usb_detached();
        }

        g_hid_count--;
        if (i != g_hid_count) {
            g_hid_devices[i] = g_hid_devices[g_hid_count];
        }
        usb_memset(&g_hid_devices[g_hid_count], 0, sizeof(hid_device_t));
        hid_recompute_controllers();
        return;
    }
}

void usb_hid_poll(void)
{
    if (g_hid_count == 0) return;

    unsigned char report_buf[16];

    for (int i = 0; i < g_hid_count; i++) {
        hid_device_t* hid = &g_hid_devices[i];
        if (!hid->dev || !hid->ep || !hid->dev->hci || !hid->dev->hci->intr_in) {
            continue;
        }

        extern volatile int global_tick;
        if (hid->dev->vendor == HID_VMWARE_VENDOR_ID &&
            hid->dev->product == HID_VMWARE_MOUSE_PID &&
            hid->control_poll_enabled &&
            global_tick - hid->last_control_tick >= 2) {
            hid->last_control_tick = global_tick;
            unsigned char control_report[16];
            usb_memset(control_report, 0, sizeof(control_report));
            int control_len = usb_control_msg(
                hid->dev, USB_DIR_IN | USB_RT_CLASS | USB_RT_INTERFACE,
                HID_REQ_GET_REPORT,
                (HID_REPORT_INPUT << 8) | 0, hid->dev->interface_number,
                control_report, hid->report_len);
            if (control_len > 0) {
                apply_vmware_control_report(
                    hid, control_report, control_len, "poll");
            } else {
                hid->control_poll_enabled = 0;
                log_printf("[HID  ] VMware GET_REPORT polling disabled "
                           "(result=%d)\n", control_len);
            }
        }

        if (hid->report_seen &&
            global_tick - hid->last_debug_tick >= 100) {
            hid->last_debug_tick = global_tick;
            log_printf("[HIDHB] dev=%u reports=%u last="
                       "%X %X %X %X %X %X %X %X\n",
                       hid->dev->address, hid->report_count,
                       hid->prev_report[0], hid->prev_report[1],
                       hid->prev_report[2], hid->prev_report[3],
                       hid->prev_report[4], hid->prev_report[5],
                       hid->prev_report[6], hid->prev_report[7]);
        }

        /*
         * Drain up to 8 reports to keep pointer tracking smooth, but break
         * immediately when button state transitions occur or on keyboard reports.
         * This prevents collapsing a press/release edge within a single poll cycle.
         */
        for (int pass = 0; pass < 8; pass++) {
            usb_memset(report_buf, 0, sizeof(report_buf));
            int len = hid->report_len;
            int ret = hid->dev->hci->intr_in(
                hid->dev->hci, hid->dev, hid->ep, report_buf, len);
            if (ret <= 0) {
                if (ret < 0) recover_interrupt_endpoint(hid);
                break;
            }
            hid->report_count++;

            if (!hid->report_seen) {
                hid->report_seen = 1;
                log_printf("[HID  ] first report from dev %u (%d bytes): "
                           "%X %X %X %X %X %X %X %X\n",
                           hid->dev->address, ret,
                           report_buf[0], report_buf[1], report_buf[2],
                           report_buf[3], report_buf[4], report_buf[5],
                           report_buf[6], report_buf[7]);
            }
            if (hid->report_count <= 12) {
                log_printf("[HIDRX] n=%u len=%d raw="
                           "%X %X %X %X %X %X %X %X\n",
                           hid->report_count, ret,
                           report_buf[0], report_buf[1], report_buf[2],
                           report_buf[3], report_buf[4], report_buf[5],
                           report_buf[6], report_buf[7]);
            }

            int curr_btn = hid_mouse_buttons(hid, report_buf, ret);
            int prev_btn = hid_mouse_buttons(
                hid, hid->prev_report, hid->report_len
            );
            int button_edge = (curr_btn != prev_btn);
            int raw_button_changed =
                report_buf[0] != hid->prev_report[0] ||
                report_buf[1] != hid->prev_report[1] ||
                (ret > 6 &&
                 (report_buf[6] != hid->prev_report[6] ||
                  report_buf[7] != hid->prev_report[7]));

            if (hid->dev->vendor == HID_VMWARE_VENDOR_ID &&
                hid->dev->product == HID_VMWARE_MOUSE_PID &&
                (button_edge || raw_button_changed ||
                 ret != hid->report_len) &&
                hid->button_reports_logged < 64) {
                hid->button_reports_logged++;
                log_printf("[HIDBTN] tick=%d len=%d btn=%d prev=%d raw="
                           "%X %X %X %X %X %X %X %X\n",
                           global_tick, ret, curr_btn, prev_btn,
                           report_buf[0], report_buf[1], report_buf[2],
                           report_buf[3], report_buf[4], report_buf[5],
                           report_buf[6], report_buf[7]);
            }

            if (hid->type == HID_TYPE_KEYBOARD && ret >= 8) {
                process_keyboard_report(hid, report_buf);
                break;
            } else if (hid->type == HID_TYPE_MOUSE && ret > 0) {
                process_mouse_report(hid, report_buf, ret);
                int copy_len = ret < (int)sizeof(hid->prev_report) ?
                               ret : (int)sizeof(hid->prev_report);
                usb_memset(hid->prev_report, 0, sizeof(hid->prev_report));
                usb_memcpy(hid->prev_report, report_buf, (unsigned int)copy_len);

                /*
                 * Stop draining immediately when:
                 * 1) Button state changed (press or release edge)
                 * 2) Any button is currently pressed (hold down / drag)
                 * This ensures the UI / window manager processes every button transition
                 * and gets accurate 1-to-1 frames without high-speed mouse reports
                 * swallowing edges or coalescing clicks!
                 */
                if (button_edge || curr_btn != 0) {
                    break;
                }
            }
        }
    }
}

int usb_hid_keyboard_count(void)
{
    return g_kbd_count;
}

int usb_hid_mouse_count(void)
{
    return g_mouse_count;
}

int usb_hid_controller_active(void)
{
    return g_active_ctrls;
}

const char* usb_hid_controller_summary(void)
{
    if ((g_active_ctrls & USB_HID_CTRL_EHCI) && (g_active_ctrls & USB_HID_CTRL_UHCI)) {
        return "EHCI 2.0 + UHCI HID";
    }
    if (g_active_ctrls & USB_HID_CTRL_EHCI) {
        return "EHCI 2.0 HID";
    }
    if (g_active_ctrls & USB_HID_CTRL_UHCI) {
        return "UHCI 1.1 HID";
    }
    return "None Active";
}
