/*
 * Pixel Neko Next - VMware VMMouse compatibility driver
 * Copyright (c) 2026 Pixel Neko Project
 * Licensed under the MIT License.
 *
 * Implements the public VMware backdoor hypervisor protocol for the virtual
 * absolute pointer. The register ABI and command identifiers are documented
 * by VMware's backdoor interface; this is an original implementation.
 */

#include "vmware_mouse.h"
#include "mouse.h"
#include "log.h"
#include "timer.h"
#include "wm.h"

#define VM_BACKDOOR_MAGIC       0x564D5868U
#define VM_BACKDOOR_PORT        0x5658U

#define VM_CMD_ABSPOINTER_DATA      39U
#define VM_CMD_ABSPOINTER_STATUS    40U
#define VM_CMD_ABSPOINTER_COMMAND   41U
#define VM_CMD_ABSPOINTER_RESTRICT  86U
#define VM_CMD_GETVERSION           10U

#define VM_ABS_ENABLE           0x45414552U
#define VM_ABS_DISABLE          0x000000F5U
#define VM_ABS_REQUEST_ABSOLUTE 0x53424152U
#define VM_ABS_REQUEST_RELATIVE 0x4C455252U
#define VM_ABS_VERSION_ID       0x3442554AU

#define VM_ABS_ERROR            0xFFFF0000U
#define VM_ABS_RELATIVE_PACKET  0x00010000U
#define VM_ABS_LEFT_BUTTON      0x20U
#define VM_ABS_RIGHT_BUTTON     0x10U
#define VM_ABS_MIDDLE_BUTTON    0x08U
#define VM_ABS_COORD_MAX        65535U

#define VM_ABS_RESTRICT_CPL0    1U

static int g_vmmouse_active;
static int g_vmmouse_logs;
static int g_vmmouse_last_buttons;

static unsigned int vmware_call1(unsigned int command, unsigned int input)
{
    unsigned int result;
    asm volatile(
        "movw %[port], %%dx\n\t"
        "inl (%%dx), %%eax"
        : "=a"(result)
        : "a"(VM_BACKDOOR_MAGIC), "b"(input), "c"(command), "d"(0U),
          [port] "i"(VM_BACKDOOR_PORT)
        : "cc", "memory");
    return result;
}

static unsigned int vmware_call3(unsigned int command, unsigned int input,
                                 unsigned int* out1, unsigned int* out2)
{
    unsigned int result;
    unsigned int value1;
    unsigned int value2;
    asm volatile(
        "movw %[port], %%dx\n\t"
        "inl (%%dx), %%eax"
        : "=a"(result), "=b"(value1), "=c"(value2)
        : "a"(VM_BACKDOOR_MAGIC), "b"(input), "c"(command), "d"(0U),
          [port] "i"(VM_BACKDOOR_PORT)
        : "cc", "memory");
    if (out1) *out1 = value1;
    if (out2) *out2 = value2;
    return result;
}

static unsigned int vmware_call4(unsigned int command, unsigned int input,
                                 unsigned int* out1, unsigned int* out2,
                                 unsigned int* out3)
{
    unsigned int result;
    unsigned int value1;
    unsigned int value2;
    unsigned int value3;
    asm volatile(
        "movw %[port], %%dx\n\t"
        "inl (%%dx), %%eax"
        : "=a"(result), "=b"(value1), "=c"(value2), "=d"(value3)
        : "a"(VM_BACKDOOR_MAGIC), "b"(input), "c"(command), "d"(0U),
          [port] "i"(VM_BACKDOOR_PORT)
        : "cc", "memory");
    if (out1) *out1 = value1;
    if (out2) *out2 = value2;
    if (out3) *out3 = value3;
    return result;
}

static void vmware_abs_disable(void)
{
    (void)vmware_call1(VM_CMD_ABSPOINTER_COMMAND, VM_ABS_DISABLE);
}

int vmware_mouse_init(void)
{
    if (g_vmmouse_active) return 0;

    unsigned int magic = 0;
    unsigned int version = vmware_call3(VM_CMD_GETVERSION, 0, &magic, 0);
    if (version == 0xFFFFFFFFU || magic != VM_BACKDOOR_MAGIC) {
        log_printf("[VMM  ] VMware backdoor unavailable (ver=%X magic=%X)\n",
                   version, magic);
        return -1;
    }

    (void)vmware_call1(VM_CMD_ABSPOINTER_COMMAND, VM_ABS_ENABLE);
    unsigned int status = vmware_call1(VM_CMD_ABSPOINTER_STATUS, 0);
    if ((status & VM_ABS_ERROR) == VM_ABS_ERROR ||
        (status & 0xFFFFU) == 0) {
        log_printf("[VMM  ] VMMouse enable failed (status=%X)\n", status);
        vmware_abs_disable();
        return -1;
    }

    unsigned int id = vmware_call1(VM_CMD_ABSPOINTER_DATA, 1);
    if (id != VM_ABS_VERSION_ID) {
        log_printf("[VMM  ] VMMouse version mismatch (id=%X)\n", id);
        vmware_abs_disable();
        return -1;
    }

    (void)vmware_call1(VM_CMD_ABSPOINTER_RESTRICT, VM_ABS_RESTRICT_CPL0);
    (void)vmware_call1(VM_CMD_ABSPOINTER_COMMAND, VM_ABS_REQUEST_ABSOLUTE);
    g_vmmouse_active = 1;
    g_vmmouse_last_buttons = 0;
    log_printf("[VMM  ] VMware VMMouse enabled (id=%X)\n", id);
    return 0;
}

void vmware_mouse_poll(void)
{
    if (!g_vmmouse_active) return;

    for (int packet = 0; packet < 256; packet++) {
        unsigned int status = vmware_call1(VM_CMD_ABSPOINTER_STATUS, 0);
        if ((status & VM_ABS_ERROR) == VM_ABS_ERROR) {
            log_printf("[VMM  ] status failed (status=%X)\n", status);
            g_vmmouse_active = 0;
            return;
        }

        unsigned int queue = status & 0xFFFFU;
        if (queue == 0) return;
        if ((queue & 3U) != 0) {
            log_printf("[VMM  ] invalid queue length %u\n", queue);
            return;
        }

        unsigned int x;
        unsigned int y;
        unsigned int z;
        status = vmware_call4(VM_CMD_ABSPOINTER_DATA, 4, &x, &y, &z);
        if ((status & VM_ABS_ERROR) == VM_ABS_ERROR) {
            log_printf("[VMM  ] data failed (status=%X)\n", status);
            g_vmmouse_active = 0;
            return;
        }

        int buttons = 0;
        if (status & VM_ABS_LEFT_BUTTON) buttons |= 1;
        if (status & VM_ABS_RIGHT_BUTTON) buttons |= 2;
        if (status & VM_ABS_MIDDLE_BUTTON) buttons |= 4;

        if (status & VM_ABS_RELATIVE_PACKET) {
            mouse_handle_usb_report(buttons, (int)x, -(int)y, (int)z);
        } else {
            unsigned int screen_x =
                (x * (unsigned int)(screen_w - 1)) / VM_ABS_COORD_MAX;
            unsigned int screen_y =
                (y * (unsigned int)(screen_h - 1)) / VM_ABS_COORD_MAX;
            mouse_handle_usb_absolute(
                buttons, (int)screen_x, (int)screen_y);
        }

        if (g_vmmouse_logs < 24 || buttons != g_vmmouse_last_buttons) {
            g_vmmouse_logs++;
            log_printf("[VMM  ] tick=%d btn=%d x=%u y=%u z=%u status=%X\n",
                       global_tick, buttons, x, y, z, status);
        }
        g_vmmouse_last_buttons = buttons;
    }
}

int vmware_mouse_active(void)
{
    return g_vmmouse_active;
}
