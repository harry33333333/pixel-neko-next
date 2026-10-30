/*
 * Pixel Neko Next - Enhanced Host Controller Interface (EHCI 2.0) Driver
 *
 * Derived from FreeBSD sys/dev/usb/controller/ehci.c
 * Copyright (c) 2008 Hans Petter Selasky <hselasky@FreeBSD.org>
 * Copyright (c) 2001-2004 Lennart Augustsson <lennart@augustsson.net>
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


#include "usb_ehci.h"
#include "usb_core.h"
#include "mm.h"
#include "timer.h"
#include "log.h"
#include "pci.h"

#define EHCI_MAX_CONTROLLERS 2

#define EHCI_USBCMD_RUN              (1U << 0)
#define EHCI_USBCMD_HCRESET          (1U << 1)
#define EHCI_USBCMD_PERIODIC_ENABLE  (1U << 4)
#define EHCI_USBCMD_ASYNC_ENABLE     (1U << 5)
#define EHCI_USBSTS_HALTED           (1U << 12)
#define EHCI_USBSTS_HOST_ERROR       (1U << 4)

#define EHCI_LEGACY_BIOS_OWNED       (1U << 16)
#define EHCI_LEGACY_OS_OWNED         (1U << 24)

#define EHCI_PORT_CONNECT            (1U << 0)
#define EHCI_PORT_CONNECT_CHANGE     (1U << 1)
#define EHCI_PORT_ENABLED            (1U << 2)
#define EHCI_PORT_ENABLE_CHANGE      (1U << 3)
#define EHCI_PORT_OVER_CURRENT       (1U << 5)
#define EHCI_PORT_RESET              (1U << 8)
#define EHCI_PORT_POWER              (1U << 12)
#define EHCI_PORT_OWNER              (1U << 13)
#define EHCI_PORT_CHANGE_BITS        (EHCI_PORT_CONNECT_CHANGE | \
                                      EHCI_PORT_ENABLE_CHANGE | \
                                      EHCI_PORT_OVER_CURRENT)

#define EHCI_QTD_ACTIVE              (1U << 7)
#define EHCI_QTD_HALTED              (1U << 6)
#define EHCI_QTD_PID_IN              1U
#define EHCI_QTD_PID_OUT             0U
#define EHCI_QTD_PID_SETUP           2U

#define EHCI_QH_CONTROL              (1U << 27)

#define EHCI_MAX_INTR_SLOTS          8

/* EHCI qTD Structure (32-byte aligned) */
typedef struct ehci_qtd {
    volatile unsigned int next;
    volatile unsigned int alt_next;
    volatile unsigned int token;
    volatile unsigned int buf[5];
} __attribute__((aligned(32))) ehci_qtd_t;

/* EHCI Queue Head Structure (32-byte aligned) */
typedef struct ehci_qh {
    volatile unsigned int hlp;
    volatile unsigned int epchar;
    volatile unsigned int epcap;
    volatile unsigned int current_qtd;
    /* Transfer Overlay */
    volatile unsigned int next_qtd;
    volatile unsigned int alt_qtd;
    volatile unsigned int token;
    volatile unsigned int buf[5];
    volatile unsigned int ext_buf[5];
    unsigned int          reserved[7];
} __attribute__((aligned(32))) ehci_qh_t;

typedef struct {
    int             used;
    usb_device_t*   dev;
    usb_endpoint_t* ep;
    ehci_qh_t       qh;
    ehci_qtd_t      qtd;
    unsigned char   buffer[64];
} ehci_intr_slot_t;

typedef struct {
    usb_hci_t             hci;
    const pci_device_t*   pci_dev;
    unsigned int          cap_base;
    unsigned int          op_base;
    unsigned char         caplength;
    unsigned char         n_ports;
    unsigned char         has_ppc;
    unsigned int*         periodic_list;
    ehci_qh_t             async_qh;
    ehci_qh_t             ctrl_qh;
    ehci_qtd_t            setup_qtd;
    ehci_qtd_t            data_qtd;
    ehci_qtd_t            status_qtd;
    ehci_intr_slot_t      intr_slots[EHCI_MAX_INTR_SLOTS];
    unsigned int          ports_seen;
} ehci_controller_t;

static ehci_controller_t g_ehci_ctrls[EHCI_MAX_CONTROLLERS] __attribute__((aligned(64)));
static int               g_ehci_count = 0;

/* MMIO helpers */
static inline unsigned char ehci_read8(unsigned int addr)
{
    return *(volatile unsigned char*)addr;
}

static inline unsigned int ehci_read32(unsigned int addr)
{
    return *(volatile unsigned int*)addr;
}

static inline void ehci_write32(unsigned int addr, unsigned int val)
{
    *(volatile unsigned int*)addr = val;
}

static unsigned int ehci_link_qh(ehci_qh_t* qh)
{
    return ((unsigned int)qh) | 2U;
}

static void ehci_qtd_init(ehci_qtd_t* qtd, unsigned int pid, void* data,
                          int len, int toggle)
{
    usb_memset(qtd, 0, sizeof(*qtd));
    qtd->next = 1;
    qtd->alt_next = 1;
    if (len < 0) len = 0;
    if (len > 0x7FFF) len = 0x7FFF;

    qtd->token = EHCI_QTD_ACTIVE | (3U << 10) | (pid << 8) |
                 ((unsigned int)len << 16) | ((toggle & 1) << 31);
    if (len > 0 && data) {
        qtd->buf[0] = (unsigned int)data;
        qtd->buf[1] = ((unsigned int)data + (unsigned int)len - 1U) &
                      0xFFFFF000U;
        if (qtd->buf[1] == ((unsigned int)data & 0xFFFFF000U)) {
            qtd->buf[1] = 0;
        }
    }
}

static int ehci_qtd_failed(unsigned int token)
{
    return (token & EHCI_QTD_HALTED) != 0;
}

static unsigned int ehci_qh_epcap(usb_device_t* dev, unsigned int smask,
                                  unsigned int cmask)
{
    unsigned int epcap = (1U << 30) |
                         ((smask & 0xFFU) << 8) |
                         (cmask & 0xFFU);

    if (dev->speed != USB_SPEED_HIGH && dev->parent_hub > 0) {
        epcap |= ((unsigned int)(dev->parent_hub & 0x7FU) << 16);
        epcap |= ((unsigned int)(dev->parent_port & 0x7FU) << 24);
    }
    return epcap;
}

static void ehci_rebuild_periodic(ehci_controller_t* c)
{
    unsigned int usbcmd_addr = c->op_base + 0x00;
    unsigned int cmd = ehci_read32(usbcmd_addr);
    int was_enabled = (cmd & EHCI_USBCMD_PERIODIC_ENABLE) != 0;

    if (was_enabled) {
        ehci_write32(usbcmd_addr, cmd & ~EHCI_USBCMD_PERIODIC_ENABLE);
        sleep_ms(2);
    }

    ehci_qh_t* first = (ehci_qh_t*)0;
    ehci_qh_t* last = (ehci_qh_t*)0;

    for (int i = 0; i < EHCI_MAX_INTR_SLOTS; i++) {
        ehci_intr_slot_t* slot = &c->intr_slots[i];
        if (!slot->used) continue;

        slot->qh.hlp = 1;
        if (last) {
            last->hlp = ehci_link_qh(&slot->qh);
        } else {
            first = &slot->qh;
        }
        last = &slot->qh;
    }

    unsigned int head = first ? ehci_link_qh(first) : 1;
    for (int i = 0; i < 1024; i++) {
        c->periodic_list[i] = head;
    }

    if (was_enabled) {
        cmd = ehci_read32(usbcmd_addr);
        ehci_write32(usbcmd_addr, cmd | EHCI_USBCMD_PERIODIC_ENABLE);
        sleep_ms(2);
    }
}

static ehci_intr_slot_t* ehci_find_intr_slot(ehci_controller_t* c,
                                             usb_device_t* dev,
                                             usb_endpoint_t* ep)
{
    for (int i = 0; i < EHCI_MAX_INTR_SLOTS; i++) {
        ehci_intr_slot_t* slot = &c->intr_slots[i];
        if (slot->used && slot->dev == dev && slot->ep == ep) return slot;
    }
    return (ehci_intr_slot_t*)0;
}

static void ehci_rearm_intr_slot(ehci_intr_slot_t* slot, int len)
{
    if (!slot || len <= 0) return;

    int toggle = slot->ep->toggle & 1;
    ehci_qtd_init(&slot->qtd, EHCI_QTD_PID_IN, slot->buffer, len, toggle);

    slot->qh.next_qtd = (unsigned int)&slot->qtd;
    slot->qh.alt_qtd = 1;
    slot->qh.token = 0;
    for (int i = 0; i < 5; i++) slot->qh.buf[i] = 0;
}

static void ehci_release_intr_slot(ehci_controller_t* c, usb_device_t* dev)
{
    int changed = 0;
    for (int i = 0; i < EHCI_MAX_INTR_SLOTS; i++) {
        ehci_intr_slot_t* slot = &c->intr_slots[i];
        if (slot->used && (!dev || slot->dev == dev)) {
            usb_memset(slot, 0, sizeof(*slot));
            changed = 1;
        }
    }
    if (changed) ehci_rebuild_periodic(c);
}

/*
 * EECP is found through HCCPARAMS in controller MMIO. A BIOS that keeps its
 * ownership semaphore after the timeout still owns the legacy input path, so
 * abort without resetting or disabling its SMIs.
 */
static int ehci_bios_handoff(const pci_device_t* dev, unsigned int cap_base)
{
    unsigned int hccparams = ehci_read32(cap_base + 0x08);
    unsigned int offset = (hccparams >> 8) & 0xFFU;
    int legacy = -1;

    for (int i = 0; i < 32 && offset >= 0x40U && offset < 0x100U; i++) {
        unsigned int capability =
            pci_config_read(dev->bus, dev->slot, dev->function,
                            (unsigned char)offset);
        unsigned char id = (unsigned char)(capability & 0xFFU);
        unsigned char next = (unsigned char)((capability >> 8) & 0xFFU);
        if (id == 0x01U) {
            legacy = (int)offset;
            break;
        }
        if (next == 0 || next == offset) break;
        offset = next;
    }

    if (legacy < 0) return 0;

    unsigned int legsup =
        pci_config_read(dev->bus, dev->slot, dev->function,
                        (unsigned char)legacy);
    if (legsup & EHCI_LEGACY_BIOS_OWNED) {
        log_printf("[EHCI ] BIOS owns legacy input (eecp=0x%X), requesting hand-off\n",
                   legacy);
        pci_config_write(dev->bus, dev->slot, dev->function,
                         (unsigned char)legacy,
                         legsup | EHCI_LEGACY_OS_OWNED);

        for (int timeout = 0; timeout < 200; timeout++) {
            sleep_ms(1);
            legsup = pci_config_read(dev->bus, dev->slot, dev->function,
                                     (unsigned char)legacy);
            if (!(legsup & EHCI_LEGACY_BIOS_OWNED)) break;
        }

        if (legsup & EHCI_LEGACY_BIOS_OWNED) {
            pci_config_write(dev->bus, dev->slot, dev->function,
                             (unsigned char)legacy,
                             legsup & ~EHCI_LEGACY_OS_OWNED);
            log_printf("[EHCI ] BIOS hand-off timed out; controller left untouched\n");
            return -1;
        }
    }

    if (!(legsup & EHCI_LEGACY_OS_OWNED)) {
        pci_config_write(dev->bus, dev->slot, dev->function,
                         (unsigned char)legacy,
                         legsup | EHCI_LEGACY_OS_OWNED);
    }
    pci_config_write(dev->bus, dev->slot, dev->function,
                     (unsigned char)(legacy + 4), 0);
    log_printf("[EHCI ] BIOS hand-off complete\n");
    return 0;
}

static int ehci_port_reset(usb_hci_t* hci, int port)
{
    ehci_controller_t* c = (ehci_controller_t*)hci->priv;
    if (port < 1 || port > c->n_ports) return -1;

    unsigned int port_reg = c->op_base + 0x44 + 4 * (port - 1);
    unsigned int val = ehci_read32(port_reg);
    if (!(val & EHCI_PORT_CONNECT)) return 0;
    if (val & EHCI_PORT_OWNER) return 0;

    val |= EHCI_PORT_POWER;
    val &= ~EHCI_PORT_OWNER;
    val |= EHCI_PORT_RESET;
    ehci_write32(port_reg, val);

    sleep_ms(50);

    val = ehci_read32(port_reg);
    val |= EHCI_PORT_POWER;
    val &= ~EHCI_PORT_RESET;
    ehci_write32(port_reg, val);

    for (int timeout = 0; timeout < 100; timeout++) {
        sleep_ms(1);
        val = ehci_read32(port_reg);
        if (!(val & EHCI_PORT_RESET)) break;
    }
    if (val & EHCI_PORT_RESET) {
        log_printf("[EHCI ] root port %d reset timed out\n", port);
        return -1;
    }

    sleep_ms(20);
    val = ehci_read32(port_reg);
    if (val & EHCI_PORT_CHANGE_BITS) {
        ehci_write32(port_reg, val | EHCI_PORT_CHANGE_BITS);
        sleep_ms(1);
        val = ehci_read32(port_reg);
    }
    return (val & EHCI_PORT_ENABLED) ? USB_SPEED_HIGH : USB_SPEED_UNKNOWN;
}

static int ehci_control(usb_hci_t* hci, usb_device_t* dev,
                        const usb_setup_packet_t* setup, void* data, int len, int dir)
{
    ehci_controller_t* c = (ehci_controller_t*)hci->priv;
    if (!dev || !setup || len < 0) return -1;

    ehci_qtd_init(&c->setup_qtd, EHCI_QTD_PID_SETUP, (void*)setup, 8, 0);

    if (len > 0) {
        unsigned int pid = (dir == USB_DIR_IN) ?
                           EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT;
        ehci_qtd_init(&c->data_qtd, pid, data, len, 1);
        c->setup_qtd.next = (unsigned int)&c->data_qtd;
        c->data_qtd.next = (unsigned int)&c->status_qtd;
    } else {
        c->setup_qtd.next = (unsigned int)&c->status_qtd;
    }

    unsigned int status_pid =
        (dir == USB_DIR_IN) ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN;
    ehci_qtd_init(&c->status_qtd, status_pid, (void*)0, 0, 1);

    usb_memset(&c->ctrl_qh, 0, sizeof(ehci_qh_t));

    unsigned int speed_code = 2; /* Default High */
    if (dev->speed == USB_SPEED_LOW) speed_code = 1;
    else if (dev->speed == USB_SPEED_FULL) speed_code = 0;

    unsigned int epchar = (dev->address & 0x7F) |
                          (speed_code << 12) |
                          ((unsigned int)(dev->max_packet0 & 0x7FF) << 16) |
                          (4U << 28);
    if (dev->speed != USB_SPEED_HIGH) {
        epchar |= EHCI_QH_CONTROL;
    }
    c->ctrl_qh.epchar = epchar;

    unsigned int cmask = (dev->speed == USB_SPEED_HIGH) ? 0U : 0xE0U;
    c->ctrl_qh.epcap = ehci_qh_epcap(dev, 0, cmask);

    c->ctrl_qh.current_qtd = 0;
    c->ctrl_qh.next_qtd = (unsigned int)&c->setup_qtd;
    c->ctrl_qh.alt_qtd = 1;
    c->ctrl_qh.token = 0;

    /* Insert into circular async schedule */
    c->ctrl_qh.hlp = c->async_qh.hlp;
    c->async_qh.hlp = ((unsigned int)&c->ctrl_qh) | 2; /* Type = QH */

    int timeout = 500;
    int success = 0;
    while (timeout-- > 0) {
        if (ehci_qtd_failed(c->setup_qtd.token) ||
            (len > 0 && ehci_qtd_failed(c->data_qtd.token)) ||
            ehci_qtd_failed(c->status_qtd.token)) {
            break;
        }

        if (!(c->status_qtd.token & EHCI_QTD_ACTIVE)) {
            success = 1;
            break;
        }
        sleep_ms(1);
    }

    /* Remove from async schedule */
    c->async_qh.hlp = ((unsigned int)&c->async_qh) | 2;

    if (!success) {
        log_printf("[EHCI ] control transfer failed "
                   "(setup=0x%X data=0x%X status=0x%X)\n",
                   c->setup_qtd.token, c->data_qtd.token,
                   c->status_qtd.token);
        return -1;
    }

    if (len == 0) return 0;
    if (dir == USB_DIR_IN) {
        unsigned int remaining = (c->data_qtd.token >> 16) & 0x7FFFU;
        return (int)((unsigned int)len - remaining);
    }
    return len;
}

static int ehci_intr_in(usb_hci_t* hci, usb_device_t* dev, usb_endpoint_t* ep,
                        void* data, int len)
{
    ehci_controller_t* c = (ehci_controller_t*)hci->priv;
    if (!dev || !ep || !data || len <= 0) return -1;

    ehci_intr_slot_t* slot = ehci_find_intr_slot(c, dev, ep);
    if (!slot) {
        for (int i = 0; i < EHCI_MAX_INTR_SLOTS; i++) {
            if (!c->intr_slots[i].used) {
                slot = &c->intr_slots[i];
                break;
            }
        }
        if (!slot) {
            log_printf("[EHCI ] no periodic slot for device %u endpoint 0x%X\n",
                       dev->address, ep->address);
            return -1;
        }

        usb_memset(slot, 0, sizeof(*slot));
        slot->used = 1;
        slot->dev = dev;
        slot->ep = ep;

        unsigned int speed_code = 2;
        if (dev->speed == USB_SPEED_LOW) speed_code = 1;
        else if (dev->speed == USB_SPEED_FULL) speed_code = 0;

        slot->qh.epchar = (dev->address & 0x7F) |
                          (((unsigned int)ep->address & 0x0F) << 8) |
                          (speed_code << 12) |
                          ((unsigned int)(ep->max_packet & 0x7FF) << 16) |
                          (4U << 28);
        unsigned int cmask = (dev->speed == USB_SPEED_HIGH) ? 0U : 0xE0U;
        slot->qh.epcap = ehci_qh_epcap(dev, 0x08U, cmask);
        slot->qh.current_qtd = 0;
        slot->qh.alt_qtd = 1;
        slot->qh.token = 0;
        ep->hcd_priv = slot;

        int report_len = (len > (int)sizeof(slot->buffer)) ?
                         (int)sizeof(slot->buffer) : len;
        ehci_rearm_intr_slot(slot, report_len);
        ehci_rebuild_periodic(c);
        log_printf("[EHCI ] periodic HID slot dev %u endpoint 0x%X "
                   "packet=%u interval=%u\n",
                   dev->address, ep->address, ep->max_packet, ep->interval);
    }

    unsigned int token = slot->qtd.token;
    if (token & EHCI_QTD_ACTIVE) return 0;
    if (ehci_qtd_failed(token)) {
        log_printf("[EHCI ] periodic endpoint 0x%X dev %u failed (token=0x%X)\n",
                   ep->address, dev->address, token);
        ep->hcd_priv = (void*)0;
        ehci_release_intr_slot(c, dev);
        return -1;
    }

    int report_len = (len > (int)sizeof(slot->buffer)) ?
                     (int)sizeof(slot->buffer) : len;
    unsigned int remaining = (token >> 16) & 0x7FFFU;
    int received = report_len - (int)remaining;
    if (received < 0) received = 0;
    if (received > len) received = len;

    if (received > 0) {
        usb_memcpy(data, slot->buffer, (unsigned int)received);
    }
    ep->toggle ^= 1;
    ehci_rearm_intr_slot(slot, report_len);
    return received;
}

static void ehci_device_release(usb_hci_t* hci, usb_device_t* dev)
{
    if (!hci || !hci->priv) return;
    ehci_release_intr_slot((ehci_controller_t*)hci->priv, dev);
}

static void ehci_scan_ports(ehci_controller_t* c)
{
    for (int p = 0; p < c->n_ports && p < 32; p++) {
        unsigned int port_reg = c->op_base + 0x44 + 4 * p;
        unsigned int seen_bit = 1U << p;
        unsigned int val = ehci_read32(port_reg);
        int connected = (val & EHCI_PORT_CONNECT) != 0;
        int new_connection = (val & EHCI_PORT_CONNECT_CHANGE) != 0;

        if (val & EHCI_PORT_CHANGE_BITS) {
            ehci_write32(port_reg, val | EHCI_PORT_CHANGE_BITS);
            sleep_ms(1);
        }

        if (!connected) {
            if (c->ports_seen & seen_bit) {
                log_printf("[EHCI ] root port %d disconnected\n", p + 1);
                usb_disconnect_port(&c->hci, p + 1);
                c->ports_seen &= ~seen_bit;
            }
            continue;
        }

        if ((c->ports_seen & seen_bit) && !new_connection) continue;
        if (c->ports_seen & seen_bit) {
            usb_disconnect_port(&c->hci, p + 1);
            c->ports_seen &= ~seen_bit;
        }

        val = ehci_read32(port_reg);
        val |= EHCI_PORT_POWER;
        val &= ~EHCI_PORT_OWNER;
        ehci_write32(port_reg, val);
        sleep_ms(10);

        log_printf("[EHCI ] resetting root port %d (PORTSC=0x%X)\n",
                   p + 1, val);
        int speed = ehci_port_reset(&c->hci, p + 1);
        if (speed == USB_SPEED_HIGH) {
            log_printf("[EHCI ] High-Speed USB 2.0 device on root port %d\n",
                       p + 1);
            if (usb_enumerate(&c->hci, p + 1, USB_SPEED_HIGH) != 0) {
                log_printf("[EHCI ] enumeration failed on root port %d\n", p + 1);
            }
            c->ports_seen |= seen_bit;
        } else {
            val = ehci_read32(port_reg);
            if (val & EHCI_PORT_CONNECT) {
                val |= EHCI_PORT_POWER | EHCI_PORT_OWNER;
                ehci_write32(port_reg, val);
                log_printf("[EHCI ] root port %d returned to UHCI companion\n",
                           p + 1);
            }
            c->ports_seen |= seen_bit;
        }
    }
}

static void ehci_poll_ports(usb_hci_t* hci)
{
    if (!hci || !hci->priv) return;
    ehci_scan_ports((ehci_controller_t*)hci->priv);
}

int ehci_probe(const pci_device_t* dev)
{
    if (g_ehci_count >= EHCI_MAX_CONTROLLERS) {
        log_printf("[EHCI ] Maximum controller limit reached\n");
        return -1;
    }
    if (!dev || dev->bar_is_io[0] || !dev->bar_valid[0] ||
        (dev->bar_is_64bit[0] && dev->bar_high[0] != 0)) {
        log_printf("[EHCI ] Invalid or unreachable BAR0\n");
        return -1;
    }
    if (usb_firmware_legacy_active()) {
        log_printf("[EHCI ] firmware still owns legacy USB; probe skipped\n");
        return -1;
    }

    ehci_controller_t* c = &g_ehci_ctrls[g_ehci_count];
    usb_memset(c, 0, sizeof(ehci_controller_t));

    c->pci_dev = dev;
    unsigned int original_command =
        pci_config_read(dev->bus, dev->slot, dev->function, 0x04);
    pci_enable_memory(dev);

    c->cap_base = dev->bar[0] & ~0x0FU;
    if (!c->cap_base) {
        log_printf("[EHCI ] Invalid BAR0\n");
        return -1;
    }

    c->caplength = ehci_read8(c->cap_base + 0x00);
    if (c->caplength < 0x10U) {
        log_printf("[EHCI ] Invalid capability length %u\n", c->caplength);
        pci_config_write(dev->bus, dev->slot, dev->function, 0x04,
                         original_command);
        return -1;
    }
    c->op_base = c->cap_base + c->caplength;

    unsigned int hcsparams = ehci_read32(c->cap_base + 0x04);
    c->n_ports = hcsparams & 0x0F;
    c->has_ppc = (hcsparams >> 4) & 1;
    if (c->n_ports == 0) {
        log_printf("[EHCI ] controller reports no root ports\n");
        pci_config_write(dev->bus, dev->slot, dev->function, 0x04,
                         original_command);
        return -1;
    }

    log_printf("[EHCI ] Controller %d at MMIO 0x%X (op=0x%X, ports=%d, ppc=%d)\n",
               g_ehci_count, c->cap_base, c->op_base, c->n_ports, c->has_ppc);

    /* 1. Proceed only after firmware has explicitly released its input path. */
    if (ehci_bios_handoff(dev, c->cap_base) != 0) {
        pci_config_write(dev->bus, dev->slot, dev->function, 0x04,
                         original_command);
        return -1;
    }
    pci_enable_bus_master(dev);

    /* 2. Halt Host Controller before Reset */
    unsigned int usbcmd_addr = c->op_base + 0x00;
    unsigned int usbsts_addr = c->op_base + 0x04;

    ehci_write32(c->op_base + 0x08, 0);
    unsigned int cmd = ehci_read32(usbcmd_addr);
    if (cmd & 1) {
        ehci_write32(usbcmd_addr, cmd & ~1U);
    }

    int timeout = 50;
    while (!(ehci_read32(usbsts_addr) & EHCI_USBSTS_HALTED) &&
           timeout-- > 0) {
        sleep_ms(1);
    }

    /* 3. Reset Host Controller (HCRESET) */
    ehci_write32(usbcmd_addr,
                 ehci_read32(usbcmd_addr) | EHCI_USBCMD_HCRESET);
    timeout = 100;
    while ((ehci_read32(usbcmd_addr) & EHCI_USBCMD_HCRESET) &&
           timeout-- > 0) {
        sleep_ms(1);
    }
    if (ehci_read32(usbcmd_addr) & EHCI_USBCMD_HCRESET) {
        log_printf("[EHCI ] host controller reset timed out\n");
        return -1;
    }
    sleep_ms(10);
    ehci_write32(usbsts_addr, 0x3FU);
    ehci_write32(c->op_base + 0x08, 0);

    /* 4. Allocate Periodic Frame List (4KB, 1024 dwords) */
    c->periodic_list = (unsigned int*)pmm_alloc_page();
    if (!c->periodic_list) {
        log_printf("[EHCI ] Failed to allocate periodic frame list\n");
        return -1;
    }
    for (int i = 0; i < 1024; i++) {
        c->periodic_list[i] = 1; /* Terminate = 1 */
    }

    ehci_write32(c->op_base + 0x10, 0); /* 32-bit address structures */
    ehci_write32(c->op_base + 0x14, (unsigned int)c->periodic_list);

    /* 5. Initialize Circular Async Queue Head */
    usb_memset(&c->async_qh, 0, sizeof(ehci_qh_t));
    c->async_qh.hlp = ((unsigned int)&c->async_qh) | 2; /* Points to self, type QH */
    c->async_qh.epchar = (1U << 15);                   /* Head of Reclamation List H=1 */
    c->async_qh.token = (1U << 30);                    /* Inactive / Halted */
    c->async_qh.next_qtd = 1;
    c->async_qh.alt_qtd = 1;

    ehci_write32(c->op_base + 0x18, (unsigned int)&c->async_qh);

    /* 6. Start Host Controller, preserving the configured frame list size. */
    cmd = ehci_read32(usbcmd_addr) & (3U << 2);
    cmd |= EHCI_USBCMD_ASYNC_ENABLE | EHCI_USBCMD_PERIODIC_ENABLE |
           EHCI_USBCMD_RUN;
    ehci_write32(usbcmd_addr, cmd);
    sleep_ms(20);
    unsigned int status = ehci_read32(usbsts_addr);
    if (status & (EHCI_USBSTS_HALTED | EHCI_USBSTS_HOST_ERROR)) {
        log_printf("[EHCI ] controller failed to leave halted state\n");
        return -1;
    }

    /* 7. Route all root ports to the EHCI controller. */
    ehci_write32(c->op_base + 0x40, 1);

    /* 8. Setup HCI interface */
    c->hci.name = "Intel EHCI 2.0";
    c->hci.kind = USB_HCI_KIND_EHCI;
    c->hci.priv = c;
    c->hci.supports_hubs = 1;
    c->hci.control = ehci_control;
    c->hci.intr_in = ehci_intr_in;
    c->hci.bulk = (void*)0;
    c->hci.port_reset = ehci_port_reset;
    c->hci.device_release = ehci_device_release;
    c->hci.poll = ehci_poll_ports;

    usb_register_hci(&c->hci);
    g_ehci_count++;

    /* 9. Power root ports without stealing ports owned by a companion. */
    for (int p = 0; p < c->n_ports; p++) {
        unsigned int port_reg = c->op_base + 0x44 + 4 * p;
        unsigned int val = ehci_read32(port_reg);
        if (!(val & EHCI_PORT_POWER)) {
            ehci_write32(port_reg, val | EHCI_PORT_POWER);
        }
    }
    sleep_ms(100);

    /* 10. Enumerate USB 2.0 root devices and route FS/LS ports to UHCI. */
    ehci_scan_ports(c);

    return 0;
}

void ehci_poll(void)
{
    /* Polling handled via usb_poll */
}
