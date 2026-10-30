/*
 * Pixel Neko Next - Universal Host Controller Interface (UHCI) Driver
 *
 * Derived from FreeBSD sys/dev/usb/controller/uhci.c
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


#include "usb_uhci.h"
#include "usb_core.h"
#include "mm.h"
#include "port.h"
#include "timer.h"
#include "log.h"
#include "pci.h"

#define UHCI_MAX_CONTROLLERS 4

#define UHCI_USBCMD_RS       0x0001U
#define UHCI_USBCMD_HCRESET  0x0002U
#define UHCI_USBCMD_MAXP     0x0080U
#define UHCI_USBSTS_HCHALTED 0x0020U

#define UHCI_PORT_CONNECT    0x0001U
#define UHCI_PORT_CONN_CHG   0x0002U
#define UHCI_PORT_ENABLED    0x0004U
#define UHCI_PORT_ENABLE_CHG 0x0008U
#define UHCI_PORT_LOW_SPEED  0x0100U
#define UHCI_PORT_RESET      0x0200U
#define UHCI_PORT_SUSPEND    0x1000U
#define UHCI_PORT_CHANGE_BITS (UHCI_PORT_CONN_CHG | UHCI_PORT_ENABLE_CHG)

#define UHCI_LEGACY_SUPPORT  0x00C0U
#define UHCI_LEGACY_RWC      0x8F00U
#define UHCI_LEGACY_RO       0x5040U

#define UHCI_VMWARE_VENDOR_ID       0x15ADU
#define UHCI_VMWARE_DEVICE_ID       0x0774U

#define UHCI_TD_ACTIVE       (1U << 23)
#define UHCI_TD_SHORT_PACKET (1U << 29)
#define UHCI_TD_CERR_3       (3U << 27)
#define UHCI_TD_LOW_SPEED    (1U << 26)
#define UHCI_TD_NAK          (1U << 19)
#define UHCI_TD_FATAL_MASK   0x00760000U
#define UHCI_TD_ERROR_MASK   (UHCI_TD_FATAL_MASK | UHCI_TD_NAK)

#define UHCI_MAX_INTR_SLOTS  4

typedef struct uhci_td {
    volatile unsigned int link;
    volatile unsigned int ctrl;
    volatile unsigned int token;
    volatile unsigned int buffer;
    unsigned int          reserved[4];
} __attribute__((aligned(16))) uhci_td_t;

typedef struct uhci_qh {
    volatile unsigned int head_link;
    volatile unsigned int element_link;
    unsigned int          reserved[6];
} __attribute__((aligned(16))) uhci_qh_t;

typedef struct {
    int             used;
    usb_device_t*   dev;
    usb_endpoint_t* ep;
    uhci_qh_t       qh;
    uhci_td_t       td;
    unsigned char   buffer[64];
    unsigned int    polls;
    unsigned int    active_polls;
    unsigned int    completions;
    unsigned int    errors;
    unsigned int    last_status;
    int             last_length;
} uhci_intr_slot_t;

typedef struct {
    usb_hci_t           hci;
    const pci_device_t* pci_dev;
    unsigned short      io_base;
    unsigned int*       frame_list;
    uhci_qh_t           ctrl_qh;
    uhci_td_t           setup_td;
    uhci_td_t           data_td;
    uhci_td_t           status_td;
    uhci_intr_slot_t    intr_slots[UHCI_MAX_INTR_SLOTS];
    unsigned int        ports_seen;
} uhci_controller_t;

static uhci_controller_t g_uhci_ctrls[UHCI_MAX_CONTROLLERS];
static int               g_uhci_count = 0;

static unsigned int uhci_link_qh(uhci_qh_t* qh)
{
    return ((unsigned int)qh & ~0x0FU) | 2U;
}

static unsigned int uhci_len_field(int len)
{
    if (len <= 0) return 0x7FFU;
    if (len > 0x7FE) len = 0x7FE;
    return (unsigned int)(len - 1);
}

static int uhci_td_actual_length(unsigned int status)
{
    return (int)((status + 1U) & 0x7FFU);
}

static void uhci_rebuild_schedule(uhci_controller_t* c)
{
    unsigned short cmd = inw(c->io_base + 0x00);
    if (cmd & UHCI_USBCMD_RS) {
        outw(c->io_base + 0x00, cmd & ~UHCI_USBCMD_RS);
        for (int timeout = 0; timeout < 20; timeout++) {
            if (inw(c->io_base + 0x02) & UHCI_USBSTS_HCHALTED) break;
            sleep_ms(1);
        }
    }

    uhci_qh_t* first = (uhci_qh_t*)0;
    uhci_qh_t* last = (uhci_qh_t*)0;
    for (int i = 0; i < UHCI_MAX_INTR_SLOTS; i++) {
        uhci_intr_slot_t* slot = &c->intr_slots[i];
        if (!slot->used) continue;
        slot->qh.head_link = 1;
        if (last) {
            last->head_link = uhci_link_qh(&slot->qh);
        } else {
            first = &slot->qh;
        }
        last = &slot->qh;
    }

    c->ctrl_qh.head_link = first ? uhci_link_qh(first) : 1;
    for (int i = 0; i < 1024; i++) {
        c->frame_list[i] = uhci_link_qh(&c->ctrl_qh);
    }

    cmd = inw(c->io_base + 0x00);
    outw(c->io_base + 0x00, cmd | UHCI_USBCMD_RS);
}

static void uhci_rearm_intr_slot(uhci_intr_slot_t* slot, int len)
{
    if (!slot || len <= 0) return;

    unsigned int address = slot->dev->address & 0x7F;
    unsigned int endpoint = slot->ep->address & 0x0F;
    uhci_td_t* td = &slot->td;
    usb_memset(td, 0, sizeof(*td));
    td->ctrl = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | UHCI_TD_SHORT_PACKET;
    if (slot->dev->speed == USB_SPEED_LOW) {
        td->ctrl |= UHCI_TD_LOW_SPEED;
    }
    td->token = 0x69U | (address << 8) | (endpoint << 15) |
                ((slot->ep->toggle & 1U) << 19) |
                (uhci_len_field(len) << 21);
    td->buffer = (unsigned int)slot->buffer;
    td->link = 1; /* Terminate link bit: hardware executes this single TD */

    slot->qh.element_link = (unsigned int)td;
}

static uhci_intr_slot_t* uhci_find_intr_slot(uhci_controller_t* c,
                                             usb_device_t* dev,
                                             usb_endpoint_t* ep)
{
    for (int i = 0; i < UHCI_MAX_INTR_SLOTS; i++) {
        uhci_intr_slot_t* slot = &c->intr_slots[i];
        if (slot->used && slot->dev == dev && slot->ep == ep) return slot;
    }
    return (uhci_intr_slot_t*)0;
}

static void uhci_release_intr_slot(uhci_controller_t* c, usb_device_t* dev)
{
    int changed = 0;
    for (int i = 0; i < UHCI_MAX_INTR_SLOTS; i++) {
        uhci_intr_slot_t* slot = &c->intr_slots[i];
        if (slot->used && (!dev || slot->dev == dev)) {
            usb_memset(slot, 0, sizeof(*slot));
            changed = 1;
        }
    }
    if (changed) uhci_rebuild_schedule(c);
}

static int uhci_port_reset(usb_hci_t* hci, int port)
{
    uhci_controller_t* c = (uhci_controller_t*)hci->priv;
    if (port < 1 || port > 2) return -1;
    unsigned short port_reg = c->io_base + ((port == 1) ? 0x10 : 0x12);
    unsigned short status = inw(port_reg);

    if (!(status & UHCI_PORT_CONNECT)) return 0;

    /* Acknowledge stale change bits without disturbing firmware port state. */
    outw(port_reg, (unsigned short)(status | UHCI_PORT_CHANGE_BITS));
    sleep_ms(10);

    status = inw(port_reg);
    status &= ~(UHCI_PORT_RESET | UHCI_PORT_SUSPEND);
    outw(port_reg, (unsigned short)(status | UHCI_PORT_RESET));
    sleep_ms(50);

    status = inw(port_reg);
    outw(port_reg, (unsigned short)((status & ~UHCI_PORT_RESET) |
                                    UHCI_PORT_CHANGE_BITS));

    for (int timeout = 0; timeout < 100; timeout++) {
        sleep_ms(1);
        status = inw(port_reg);
        if (!(status & UHCI_PORT_RESET)) break;
    }
    if (status & UHCI_PORT_RESET) {
        log_printf("[UHCI ] port %d reset timed out\n", port);
        return -1;
    }

    sleep_ms(20);

    status = inw(port_reg);
    if (!(status & UHCI_PORT_ENABLED) && (status & UHCI_PORT_CONNECT)) {
        outw(port_reg, (unsigned short)(status | UHCI_PORT_ENABLED |
                                        UHCI_PORT_CHANGE_BITS));
        sleep_ms(10);
        status = inw(port_reg);
    }

    if (!(status & UHCI_PORT_CONNECT)) return 0;
    return (status & UHCI_PORT_LOW_SPEED) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static int uhci_control(usb_hci_t* hci, usb_device_t* dev,
                        const usb_setup_packet_t* setup, void* data, int len, int dir)
{
    uhci_controller_t* c = (uhci_controller_t*)hci->priv;
    if (!dev || !setup || len < 0) return -1;

    usb_memset(&c->setup_td, 0, sizeof(uhci_td_t));
    usb_memset(&c->data_td, 0, sizeof(uhci_td_t));
    usb_memset(&c->status_td, 0, sizeof(uhci_td_t));

    unsigned int ls_bit =
        (dev->speed == USB_SPEED_LOW) ? UHCI_TD_LOW_SPEED : 0;
    unsigned int addr = dev->address & 0x7F;

    c->setup_td.ctrl = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | ls_bit;
    c->setup_td.token = 0x2D | (addr << 8) |
                        (uhci_len_field(8) << 21);
    c->setup_td.buffer = (unsigned int)setup;

    if (len > 0) {
        c->setup_td.link = (unsigned int)&c->data_td;

        unsigned int pid = (dir == USB_DIR_IN) ? 0x69U : 0xE1U;
        c->data_td.ctrl = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | ls_bit;
        if (dir == USB_DIR_IN) c->data_td.ctrl |= UHCI_TD_SHORT_PACKET;
        c->data_td.token = pid | (addr << 8) | (1U << 19) |
                           (uhci_len_field(len) << 21);
        c->data_td.buffer = (unsigned int)data;
        c->data_td.link = (unsigned int)&c->status_td;
    } else {
        c->setup_td.link = (unsigned int)&c->status_td;
    }

    unsigned int status_pid = (dir == USB_DIR_IN) ? 0xE1U : 0x69U;
    c->status_td.ctrl = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | ls_bit;
    c->status_td.token = status_pid | (addr << 8) | (1U << 19) |
                         (uhci_len_field(0) << 21);
    c->status_td.buffer = 0;
    c->status_td.link = 1;

    c->ctrl_qh.element_link = (unsigned int)&c->setup_td;
    outw(c->io_base + 0x00,
         inw(c->io_base + 0x00) | UHCI_USBCMD_RS);

    int timeout = 250;
    int success = 0;
    while (timeout-- > 0) {
        if (c->setup_td.ctrl & UHCI_TD_FATAL_MASK) break;
        if (len > 0 && (c->data_td.ctrl & UHCI_TD_FATAL_MASK)) break;
        if (c->status_td.ctrl & UHCI_TD_FATAL_MASK) break;

        if ((c->setup_td.ctrl | (len > 0 ? c->data_td.ctrl : 0) |
             c->status_td.ctrl) & UHCI_TD_NAK) {
            c->setup_td.ctrl =
                (c->setup_td.ctrl & ~UHCI_TD_ERROR_MASK) | UHCI_TD_ACTIVE;
            if (len > 0) {
                c->data_td.ctrl =
                    (c->data_td.ctrl & ~UHCI_TD_ERROR_MASK) |
                    UHCI_TD_ACTIVE;
            }
            c->status_td.ctrl =
                (c->status_td.ctrl & ~UHCI_TD_ERROR_MASK) |
                UHCI_TD_ACTIVE;
            c->ctrl_qh.element_link = (unsigned int)&c->setup_td;
            continue;
        }

        /*
         * A short IN packet completes the data TD but may stop the QH before
         * the status TD. Explicitly advance the element pointer so the
         * control STATUS stage is still executed.
         */
        if (!(c->setup_td.ctrl & UHCI_TD_ACTIVE) &&
            (c->status_td.ctrl & UHCI_TD_ACTIVE)) {
            if (len == 0 || !(c->data_td.ctrl & UHCI_TD_ACTIVE)) {
                c->ctrl_qh.element_link = (unsigned int)&c->status_td;
            }
        }

        if (!(c->status_td.ctrl & UHCI_TD_ACTIVE)) {
            success = 1;
            break;
        }
        sleep_ms(1);
    }

    c->ctrl_qh.element_link = 1;

    if (!success) {
        log_printf("[UHCI ] control transfer failed "
                   "(setup=0x%X data=0x%X status=0x%X)\n",
                   c->setup_td.ctrl, c->data_td.ctrl,
                   c->status_td.ctrl);
        return -1;
    }
    if (dir != USB_DIR_IN || len == 0) return 0;
    int actual = uhci_td_actual_length(c->data_td.ctrl);
    return (actual <= len) ? actual : len;
}

static int uhci_intr_in(usb_hci_t* hci, usb_device_t* dev, usb_endpoint_t* ep,
                        void* data, int len)
{
    uhci_controller_t* c = (uhci_controller_t*)hci->priv;
    if (!dev || !ep || !data || len <= 0) return -1;

    uhci_intr_slot_t* slot = uhci_find_intr_slot(c, dev, ep);
    if (!slot) {
        for (int i = 0; i < UHCI_MAX_INTR_SLOTS; i++) {
            if (!c->intr_slots[i].used) {
                slot = &c->intr_slots[i];
                break;
            }
        }
        if (!slot) {
            log_printf("[UHCI ] no periodic slot for device %u endpoint 0x%X\n",
                       dev->address, ep->address);
            return -1;
        }

        usb_memset(slot, 0, sizeof(*slot));
        slot->used = 1;
        slot->dev = dev;
        slot->ep = ep;
        slot->qh.head_link = 1;
        ep->hcd_priv = slot;

        int report_len = (len > (int)sizeof(slot->buffer)) ?
                         (int)sizeof(slot->buffer) : len;
        uhci_rearm_intr_slot(slot, report_len);
        uhci_rebuild_schedule(c);
        log_printf("[UHCI ] periodic HID slot dev %u endpoint 0x%X "
                   "packet=%u interval=%u\n",
                   dev->address, ep->address, ep->max_packet, ep->interval);
    }

    unsigned int status = slot->td.ctrl;
    slot->polls++;
    slot->last_status = status;

    /* If the TD is still active in hardware, transfer is not complete (waiting or NAK) */
    if (status & UHCI_TD_ACTIVE) {
        slot->active_polls++;
        return 0;
    }

    /* Check for fatal bus/protocol errors */
    if (status & UHCI_TD_FATAL_MASK) {
        slot->errors++;
        log_printf("[UHCI ] periodic endpoint 0x%X dev %u failed (status=0x%X)\n",
                   ep->address, dev->address, status);
        ep->hcd_priv = (void*)0;
        uhci_release_intr_slot(c, dev);
        return -1;
    }

    int report_len = (len > (int)sizeof(slot->buffer)) ?
                     (int)sizeof(slot->buffer) : len;

    /* Packet transferred successfully */
    int received = uhci_td_actual_length(status);
    if (received > report_len) received = report_len;
    if (received > len) received = len;
    if (received > 0) {
        usb_memcpy(data, slot->buffer, (unsigned int)received);
        slot->completions++;
        slot->last_length = received;
        slot->ep->toggle ^= 1;
        uhci_rearm_intr_slot(slot, report_len);
        return received;
    }

    /* No new data from device (empty or NAK); rearm and do not toggle */
    uhci_rearm_intr_slot(slot, report_len);
    return 0;
}

static void uhci_device_release(usb_hci_t* hci, usb_device_t* dev)
{
    if (!hci || !hci->priv) return;
    uhci_release_intr_slot((uhci_controller_t*)hci->priv, dev);
}

static void uhci_scan_ports(uhci_controller_t* c)
{
    for (int port = 1; port <= 2; port++) {
        unsigned short port_reg = c->io_base + ((port == 1) ? 0x10 : 0x12);
        unsigned int seen_bit = 1U << (port - 1);
        unsigned short status = inw(port_reg);
        int connected = (status & UHCI_PORT_CONNECT) != 0;
        int new_connection = (status & UHCI_PORT_CONN_CHG) != 0;

        if (status & UHCI_PORT_CHANGE_BITS) {
            outw(port_reg, (unsigned short)(status | UHCI_PORT_CHANGE_BITS));
            sleep_ms(1);
        }

        if (!connected) {
            if (c->ports_seen & seen_bit) {
                log_printf("[UHCI ] port %d disconnected\n", port);
                usb_disconnect_port(&c->hci, port);
                c->ports_seen &= ~seen_bit;
            }
            continue;
        }

        if ((c->ports_seen & seen_bit) && !new_connection) continue;
        if (c->ports_seen & seen_bit) {
            usb_disconnect_port(&c->hci, port);
            c->ports_seen &= ~seen_bit;
        }

        int speed = uhci_port_reset(&c->hci, port);
        if (speed > 0) {
            const char* speed_name =
                (speed == USB_SPEED_LOW) ? "Low-Speed" : "Full-Speed";
            log_printf("[UHCI ] %s USB device on port %d\n", speed_name, port);
            if (usb_enumerate(&c->hci, port, speed) != 0) {
                log_printf("[UHCI ] enumeration failed on port %d\n", port);
            }
        } else {
            log_printf("[UHCI ] port %d did not reset cleanly\n", port);
        }
        c->ports_seen |= seen_bit;
    }
}

static void uhci_poll_ports(usb_hci_t* hci)
{
    if (!hci || !hci->priv) return;
    uhci_scan_ports((uhci_controller_t*)hci->priv);
}

int uhci_probe(const pci_device_t* dev)
{
    if (g_uhci_count >= UHCI_MAX_CONTROLLERS) {
        return -1;
    }
    if (!dev) return -1;
    if (usb_firmware_legacy_active()) {
        log_printf("[UHCI ] firmware still owns legacy USB; probe skipped\n");
        return -1;
    }

    uhci_controller_t* c = &g_uhci_ctrls[g_uhci_count];
    usb_memset(c, 0, sizeof(uhci_controller_t));

    c->pci_dev = dev;

    int io_bar = -1;
    for (int bar = 0; bar < PCI_BAR_COUNT; bar++) {
        if (dev->bar_valid[bar] && dev->bar_is_io[bar]) {
            io_bar = bar;
            break;
        }
    }
    if (io_bar < 0) {
        log_printf("[UHCI ] Invalid I/O BAR\n");
        return -1;
    }
    c->io_base = (unsigned short)(dev->bar[io_bar] & ~0x1FU);
    if (!c->io_base) {
        log_printf("[UHCI ] Invalid I/O base\n");
        return -1;
    }

    unsigned int legacy =
        pci_config_read(dev->bus, dev->slot, dev->function,
                        UHCI_LEGACY_SUPPORT);
    unsigned int legacy_low = legacy & 0xFFFFU;
    int vmware_uhci = dev->vendor == UHCI_VMWARE_VENDOR_ID &&
                      dev->device == UHCI_VMWARE_DEVICE_ID;
    if (!vmware_uhci &&
        (legacy_low & ~(UHCI_LEGACY_RWC | UHCI_LEGACY_RO))) {
        log_printf("[UHCI ] firmware legacy path active (legsup=0x%X); "
                   "controller left untouched\n", legacy_low);
        return -1;
    }
    if (vmware_uhci && legacy_low) {
        log_printf("[UHCI ] VMware virtual legacy bits (0x%X) cleared for "
                   "native takeover\n", legacy_low);
    }
    pci_config_write(dev->bus, dev->slot, dev->function, UHCI_LEGACY_SUPPORT,
                     (legacy & 0xFFFF0000U) |
                     (legacy_low | UHCI_LEGACY_RWC));

    pci_enable_io(dev);
    pci_enable_bus_master(dev);

    log_printf("[UHCI ] Controller %d at IO 0x%X\n", g_uhci_count, c->io_base);

    /* 1. Reset only the host controller, not its root hub. */
    outw(c->io_base + 0x00, UHCI_USBCMD_HCRESET);
    int reset_timeout = 100;
    while ((inw(c->io_base + 0x00) & UHCI_USBCMD_HCRESET) &&
           reset_timeout-- > 0) {
        sleep_ms(1);
    }
    if (inw(c->io_base + 0x00) & UHCI_USBCMD_HCRESET) {
        log_printf("[UHCI ] host controller reset timed out\n");
        return -1;
    }
    outw(c->io_base + 0x00, 0x0000);
    sleep_ms(10);

    /* 2. Clear Status and Disable IRQs */
    outw(c->io_base + 0x02, 0x003F);
    outw(c->io_base + 0x04, 0x0000);
    outb(c->io_base + 0x0C, 0x40); /* SOF timing */

    /* 3. Allocate 4KB Frame List */
    c->frame_list = (unsigned int*)pmm_alloc_page();
    if (!c->frame_list) {
        log_printf("[UHCI ] Failed to allocate frame list\n");
        return -1;
    }
    usb_memset(&c->ctrl_qh, 0, sizeof(c->ctrl_qh));
    c->ctrl_qh.head_link = 1;
    c->ctrl_qh.element_link = 1;
    for (int i = 0; i < 1024; i++) {
        c->frame_list[i] = uhci_link_qh(&c->ctrl_qh);
    }

    outl(c->io_base + 0x08, (unsigned int)c->frame_list);
    outw(c->io_base + 0x06, 0); /* Frame Number = 0 */

    /* 4. Start Controller (Run=1, Max Packet 64=1) */
    outw(c->io_base + 0x00, UHCI_USBCMD_RS | UHCI_USBCMD_MAXP);
    sleep_ms(10);

    /* 5. Setup HCI */
    c->hci.name = "UHCI 1.1";
    c->hci.kind = USB_HCI_KIND_UHCI;
    c->hci.priv = c;
    c->hci.supports_hubs = 1;
    c->hci.control = uhci_control;
    c->hci.intr_in = uhci_intr_in;
    c->hci.bulk = (void*)0;
    c->hci.port_reset = uhci_port_reset;
    c->hci.device_release = uhci_device_release;
    c->hci.poll = uhci_poll_ports;

    usb_register_hci(&c->hci);
    g_uhci_count++;

    /* 6. Enumerate root ports and keep ownership changes pollable. */
    uhci_scan_ports(c);

    return 0;
}

void uhci_poll(void)
{
}
