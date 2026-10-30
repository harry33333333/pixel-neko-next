/*
 * Pixel Neko Next - USB Core Subsystem
 *
 * Derived from FreeBSD sys/dev/usb/usb_core.c, usb_transfer.c and usb_device.c
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


#include "usb_core.h"
#include "usb_hub.h"
#include "usb_hid.h"
#include "usb_ehci.h"
#include "usb_uhci.h"
#include "timer.h"
#include "log.h"
#include "pci.h"

#define USB_UHCI_LEGACY_SUPPORT_REG 0xC0U
#define USB_UHCI_LEGACY_RWC         0x8F00U
#define USB_UHCI_LEGACY_RO          0x5040U

#define USB_VMWARE_VENDOR_ID        0x15ADU
#define USB_VMWARE_UHCI_DEVICE_ID   0x0774U

static usb_hci_t*   g_hcis[USB_MAX_HCIS];
static int          g_hci_count = 0;
static unsigned int g_host_controller_kinds = 0;

static usb_device_t g_devices[USB_MAX_DEVICES];
static unsigned char g_next_addr = 1;
static int g_firmware_legacy_state = -1;

int usb_firmware_legacy_active(void)
{
    if (g_firmware_legacy_state >= 0) return g_firmware_legacy_state;

    g_firmware_legacy_state = 0;
    int count = pci_get_device_count();

    for (int i = 0; i < count; i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x0C || dev->subclass != 0x03) {
            continue;
        }

        if (dev->prog_if == 0x00) {
            unsigned int legsup =
                pci_config_read(dev->bus, dev->slot, dev->function,
                                USB_UHCI_LEGACY_SUPPORT_REG) & 0xFFFFU;

            /*
             * VMware's virtual UHCI can leave firmware routing bits visible
             * even though there is no SMM legacy emulation behind them. It
             * must be handed to the native driver after firmware boot.
             */
            if (dev->vendor == USB_VMWARE_VENDOR_ID &&
                dev->device == USB_VMWARE_UHCI_DEVICE_ID) {
                continue;
            }

            /*
             * The status bits are write-one-to-clear and several bits are
             * read-only. Only the remaining programmed legacy controls show
             * that firmware is still translating USB input through SMM.
             */
            if (legsup & ~(USB_UHCI_LEGACY_RWC | USB_UHCI_LEGACY_RO)) {
                log_printf("[USB  ] firmware legacy USB owns UHCI %X:%X.%X "
                           "(legsup=0x%X)\n",
                           dev->bus, dev->slot, dev->function, legsup);
                g_firmware_legacy_state = 1;
                break;
            }
        }
    }

    if (g_firmware_legacy_state == 0) {
        log_printf("[USB  ] no firmware-owned legacy USB controller detected\n");
    }
    return g_firmware_legacy_state;
}

void usb_memset(void* dst, unsigned char c, unsigned int n)
{
    unsigned char* d = (unsigned char*)dst;
    for (unsigned int i = 0; i < n; i++) {
        d[i] = c;
    }
}

void usb_memcpy(void* dst, const void* src, unsigned int n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned int i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

void usb_register_hci(usb_hci_t* hci)
{
    if (g_hci_count < USB_MAX_HCIS && hci) {
        g_hcis[g_hci_count++] = hci;
        g_host_controller_kinds |= hci->kind;
        log_printf("[USB  ] Registered host controller: %s\n", hci->name ? hci->name : "Unknown");
    }
}

const char* usb_host_controller_summary(void)
{
    int ehci = (g_host_controller_kinds & USB_HCI_KIND_EHCI) != 0;
    int uhci = (g_host_controller_kinds & USB_HCI_KIND_UHCI) != 0;

    if (ehci && uhci) return "USB2.0 EHCI + USB1.1 UHCI";
    if (ehci) return "USB2.0 EHCI";
    if (uhci) return "USB1.1 UHCI";
    if (usb_firmware_legacy_active()) return "BIOS USB Legacy (PS/2)";
    return "No native host controller";
}

usb_device_t* usb_alloc_device(void)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!g_devices[i].hci) {
            usb_memset(&g_devices[i], 0, sizeof(usb_device_t));
            return &g_devices[i];
        }
    }
    return (usb_device_t*)0;
}

void usb_release_device(usb_device_t* dev)
{
    if (dev) {
        dev->hci = (usb_hci_t*)0;
    }
}

void usb_disconnect_port(usb_hci_t* hci, int root_port)
{
    if (!hci || root_port <= 0) return;

    int remove[USB_MAX_DEVICES] = {0};
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_device_t* dev = &g_devices[i];
        if (dev->hci == hci && dev->parent_hub == 0 &&
            dev->port == root_port) {
            remove[i] = 1;
        }
    }

    int changed;
    do {
        changed = 0;
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            usb_device_t* dev = &g_devices[i];
            if (!dev->hci || dev->hci != hci || remove[i]) continue;

            for (int j = 0; j < USB_MAX_DEVICES; j++) {
                if (remove[j] && g_devices[j].address == dev->parent_hub) {
                    remove[i] = 1;
                    changed = 1;
                    break;
                }
            }
        }
    } while (changed);

    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!remove[i]) continue;
        usb_device_t* dev = &g_devices[i];
        usb_hid_detach(dev);
        if (hci->device_release) hci->device_release(hci, dev);
        dev->hci = (usb_hci_t*)0;
    }
}

int usb_control_msg(usb_device_t* dev, int dir, unsigned char request,
                    unsigned short value, unsigned short index,
                    void* data, int len)
{
    if (!dev || !dev->hci || !dev->hci->control) return -1;

    usb_setup_packet_t setup;
    setup.bmRequestType = (unsigned char)dir;
    setup.bRequest = request;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = (unsigned short)len;

    int transfer_dir = (dir & USB_DIR_IN) ? USB_DIR_IN : USB_DIR_OUT;
    return dev->hci->control(dev->hci, dev, &setup, data, len, transfer_dir);
}

static void parse_config_descriptors(usb_device_t* dev, const unsigned char* buf, int total_len)
{
    const usb_interface_desc_t* selected = (const usb_interface_desc_t*)0;
    int selected_score = -1;
    int offset = 0;

    /* Prefer a HID interface, then a Hub interface, over unrelated composite
     * functions such as Audio or Video. */
    while (offset + 2 <= total_len) {
        unsigned char len = buf[offset];
        unsigned char type = buf[offset + 1];
        if (len < 2 || offset + len > total_len) break;

        if (type == USB_DT_INTERFACE &&
            len >= sizeof(usb_interface_desc_t)) {
            const usb_interface_desc_t* iface =
                (const usb_interface_desc_t*)&buf[offset];
            int score = 1;
            if (iface->bInterfaceClass == USB_CLASS_HID) score = 3;
            else if (iface->bInterfaceClass == USB_CLASS_HUB) score = 2;
            if (score > selected_score) {
                selected = iface;
                selected_score = score;
            }
        }
        offset += len;
    }

    if (!selected) return;

    dev->class_code = selected->bInterfaceClass;
    dev->subclass = selected->bInterfaceSubClass;
    dev->protocol = selected->bInterfaceProtocol;
    dev->interface_number = selected->bInterfaceNumber;
    dev->interface_protocol = selected->bInterfaceProtocol;
    dev->endpoint_count = 0;

    int current_interface = -1;
    offset = 0;
    while (offset + 2 <= total_len) {
        unsigned char len = buf[offset];
        unsigned char type = buf[offset + 1];
        if (len < 2 || offset + len > total_len) break;

        if (type == USB_DT_INTERFACE &&
            len >= sizeof(usb_interface_desc_t)) {
            const usb_interface_desc_t* iface =
                (const usb_interface_desc_t*)&buf[offset];
            current_interface = iface->bInterfaceNumber;
        } else if (type == USB_DT_ENDPOINT &&
                   len >= sizeof(usb_endpoint_desc_t) &&
                   current_interface == dev->interface_number) {
            const usb_endpoint_desc_t* ep = (const usb_endpoint_desc_t*)&buf[offset];
            if (dev->endpoint_count < USB_MAX_ENDPOINTS) {
                usb_endpoint_t* e = &dev->endpoints[dev->endpoint_count++];
                e->address = ep->bEndpointAddress & 0x0F;
                e->direction = (ep->bEndpointAddress & 0x80) ? USB_DIR_IN : USB_DIR_OUT;
                e->type = ep->bmAttributes & 0x03;
                e->interval = ep->bInterval;
                e->max_packet = ep->wMaxPacketSize & 0x07FF;
                e->toggle = 0;
                e->hcd_priv = (void*)0;
            }
        }
        offset += len;
    }
}

static int usb_enumerate_device(usb_hci_t* hci, usb_device_t* parent_hub, int port, int speed)
{
    usb_device_t* dev = usb_alloc_device();
    if (!dev) {
        log_printf("[USB  ] No free device slots\n");
        return -1;
    }

    dev->hci = hci;
    dev->port = (unsigned char)port;
    dev->speed = (unsigned char)speed;
    dev->address = 0;
    dev->max_packet0 = (speed == USB_SPEED_HIGH) ? 64 : 8;

    if (parent_hub) {
        dev->parent_hub = parent_hub->address;
        dev->parent_port = (unsigned char)port;
        dev->hub_depth = parent_hub->hub_depth + 1;
    } else {
        dev->parent_hub = 0;
        dev->parent_port = 0;
        dev->hub_depth = 0;
    }

    /* 1. Read first 8 bytes of Device Descriptor to get bMaxPacketSize0 */
    usb_device_desc_t desc;
    usb_memset(&desc, 0, sizeof(desc));
    int ret = usb_control_msg(dev, USB_DIR_IN | USB_RT_STANDARD | USB_RT_DEVICE,
                              USB_REQ_GET_DESCRIPTOR, (USB_DT_DEVICE << 8) | 0, 0,
                              &desc, 8);
    if (ret < 8) {
        log_printf("[USB  ] Failed to read initial dev desc (err=%d)\n", ret);
        usb_release_device(dev);
        return -1;
    }

    if (desc.bMaxPacketSize0 > 0) {
        dev->max_packet0 = desc.bMaxPacketSize0;
    }

    /* 2. Assign unique address */
    unsigned char new_addr = g_next_addr++;
    if (g_next_addr >= 127) g_next_addr = 1;

    ret = usb_control_msg(dev, USB_DIR_OUT | USB_RT_STANDARD | USB_RT_DEVICE,
                          USB_REQ_SET_ADDRESS, new_addr, 0, (void*)0, 0);
    if (ret < 0) {
        log_printf("[USB  ] Failed to set address %u\n", new_addr);
        usb_release_device(dev);
        return -1;
    }
    sleep_ms(10);
    dev->address = new_addr;

    /* 3. Read full 18-byte Device Descriptor */
    usb_memset(&desc, 0, sizeof(desc));
    ret = usb_control_msg(dev, USB_DIR_IN | USB_RT_STANDARD | USB_RT_DEVICE,
                          USB_REQ_GET_DESCRIPTOR, (USB_DT_DEVICE << 8) | 0, 0,
                          &desc, sizeof(desc));
    if (ret >= (int)sizeof(usb_device_desc_t) &&
        desc.bLength >= sizeof(usb_device_desc_t) &&
        desc.bDescriptorType == USB_DT_DEVICE) {
        dev->vendor = desc.idVendor;
        dev->product = desc.idProduct;
        dev->class_code = desc.bDeviceClass;
        dev->subclass = desc.bDeviceSubClass;
        dev->protocol = desc.bDeviceProtocol;
    }

    log_printf("[USB  ] Dev %u (port %d, speed %d): VID:PID %X:%X class %X:%X\n",
               dev->address, port, speed, dev->vendor, dev->product,
               dev->class_code, dev->subclass);

    /* 4. Read Configuration Descriptor */
    unsigned char cfg_buf[512];
    usb_memset(cfg_buf, 0, sizeof(cfg_buf));
    ret = usb_control_msg(dev, USB_DIR_IN | USB_RT_STANDARD | USB_RT_DEVICE,
                          USB_REQ_GET_DESCRIPTOR, (USB_DT_CONFIG << 8) | 0, 0,
                          cfg_buf, 9);
    if (ret >= 9) {
        const usb_config_desc_t* cfg = (const usb_config_desc_t*)cfg_buf;
        unsigned short total_len = cfg->wTotalLength;
        if (total_len > sizeof(cfg_buf)) total_len = sizeof(cfg_buf);
        dev->config_value = cfg->bConfigurationValue;

        ret = usb_control_msg(dev, USB_DIR_IN | USB_RT_STANDARD | USB_RT_DEVICE,
                              USB_REQ_GET_DESCRIPTOR, (USB_DT_CONFIG << 8) | 0, 0,
                              cfg_buf, total_len);
        if (ret >= 9) {
            parse_config_descriptors(dev, cfg_buf, ret);
        }
    }

    /* 5. Set Configuration */
    if (dev->config_value == 0) dev->config_value = 1;
    ret = usb_control_msg(dev, USB_DIR_OUT | USB_RT_STANDARD | USB_RT_DEVICE,
                          USB_REQ_SET_CONFIGURATION, dev->config_value, 0,
                          (void*)0, 0);
    if (ret < 0) {
        log_printf("[USB  ] Failed to configure device %u\n", dev->address);
        usb_release_device(dev);
        return -1;
    }
    sleep_ms(10);

    /* 6. Probe device class */
    if (dev->class_code == USB_CLASS_HUB) {
        log_printf("[USB  ] Device %u is a USB Hub (RMH or external)\n", dev->address);
        usb_hub_probe(dev);
    } else if (dev->class_code == USB_CLASS_HID || dev->interface_protocol != 0) {
        usb_hid_attach(dev);
    }

    return 0;
}

int usb_enumerate(usb_hci_t* hci, int port, int speed)
{
    return usb_enumerate_device(hci, (usb_device_t*)0, port, speed);
}

int usb_enumerate_hub(usb_hci_t* hci, usb_device_t* hub, int port, int speed)
{
    return usb_enumerate_device(hci, hub, port, speed);
}

void usb_init(void)
{
    log_printf("[USB  ] Initializing USB subsystem...\n");

    /* Scan PCI for USB Host Controllers (Class 0x0C, SubClass 0x03) */
    pci_scan_all();
    int count = pci_get_device_count();

    /*
     * A CSM with Legacy USB Support can emulate the 8042 ports through SMM.
     * Resetting or owning either controller behind that firmware path makes
     * some Intel 5-series boards lose both keyboard and mouse. Keep the working
     * firmware path intact and let the existing PS/2 IRQ pipeline receive it.
     */
    if (usb_firmware_legacy_active()) {
        log_printf("[USB  ] firmware-owned controllers left untouched; "
                   "using BIOS USB keyboard/mouse emulation\n");
        return;
    }
    log_printf("[USB  ] Legacy USB inactive; enabling native EHCI/UHCI ownership\n");

    /* 1. Probe EHCI (High-Speed USB 2.0, prog_if == 0x20) */
    for (int i = 0; i < count; i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (dev && dev->class_code == 0x0C && dev->subclass == 0x03 && dev->prog_if == 0x20) {
            ehci_probe(dev);
        }
    }

    /* 2. Probe UHCI (Universal Host Controller, prog_if == 0x00) */
    for (int i = 0; i < count; i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (dev && dev->class_code == 0x0C && dev->subclass == 0x03 && dev->prog_if == 0x00) {
            uhci_probe(dev);
        }
    }

    log_printf("[USB  ] Initialization complete. Active controllers: %d\n", g_hci_count);
}

void usb_poll(void)
{
    for (int i = 0; i < g_hci_count; i++) {
        if (g_hcis[i] && g_hcis[i]->poll) {
            g_hcis[i]->poll(g_hcis[i]);
        }
    }
    usb_hid_poll();
}
