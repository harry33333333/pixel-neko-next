/*
 * Pixel Neko Next - USB Hub Class Driver
 *
 * Derived from FreeBSD sys/dev/usb/usb_hub.c
 * Copyright (c) 2008 Hans Petter Selasky <hselasky@FreeBSD.org>
 * Copyright (c) 1998 Lennart Augustsson <lennart@augustsson.net>
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

#include "usb_hub.h"
#include "usb_core.h"
#include "timer.h"
#include "log.h"

int usb_hub_probe(usb_device_t* hub)
{
    if (!hub || !hub->hci) return -1;

    log_printf("[HUB  ] Probing hub at address %u (speed %u)...\n", hub->address, hub->speed);

    /* 1. Read Hub Descriptor (Class Descriptor Type 0x29) */
    unsigned char hub_desc[16];
    usb_memset(hub_desc, 0, sizeof(hub_desc));

    int ret = usb_control_msg(hub, USB_DIR_IN | USB_RT_CLASS | USB_RT_DEVICE,
                              USB_REQ_GET_DESCRIPTOR, (USB_DT_HUB << 8) | 0, 0,
                              hub_desc, 8);
    if (ret < 8 || hub_desc[0] < 7 || hub_desc[1] != USB_DT_HUB) {
        log_printf("[HUB  ] Failed to read hub descriptor (ret=%d)\n", ret);
        return -1;
    }

    unsigned char num_ports = hub_desc[2];
    unsigned char pwr_good_time = hub_desc[5]; /* in 2ms units */
    log_printf("[HUB  ] Hub has %u downstream ports, power-on delay %u ms\n",
               num_ports, (unsigned int)pwr_good_time * 2);

    if (num_ports == 0 || num_ports > 32) {
        num_ports = 8; /* Reasonable fallback */
    }

    /* 2. Power on all downstream ports */
    for (unsigned char p = 1; p <= num_ports; p++) {
        usb_control_msg(hub, USB_DIR_OUT | USB_RT_CLASS | USB_RT_OTHER,
                        USB_REQ_SET_FEATURE, HUB_FEATURE_PORT_POWER, p,
                        (void*)0, 0);
    }

    int pwr_delay = (int)pwr_good_time * 2;
    if (pwr_delay < 150) pwr_delay = 150;
    sleep_ms(pwr_delay);

    /* 3. Scan and reset connected downstream ports */
    for (unsigned char p = 1; p <= num_ports; p++) {
        unsigned int status_buf = 0;
        ret = usb_control_msg(hub, USB_DIR_IN | USB_RT_CLASS | USB_RT_OTHER,
                              USB_REQ_GET_STATUS, 0, p,
                              &status_buf, 4);
        if (ret < 4) continue;

        unsigned short port_status = (unsigned short)(status_buf & 0xFFFF);
        if (port_status & HUB_PORT_CONNECTION) {
            log_printf("[HUB  ] Device connected on port %u (status=0x%X)\n",
                       p, port_status);

            /* Reset downstream port */
            usb_control_msg(hub, USB_DIR_OUT | USB_RT_CLASS | USB_RT_OTHER,
                            USB_REQ_SET_FEATURE, HUB_FEATURE_PORT_RESET, p,
                            (void*)0, 0);
            sleep_ms(50);

            /* Poll until port reset clears and port enable is set */
            int reset_timeout = 0;
            int reset_done = 0;
            while (reset_timeout < 100) {
                status_buf = 0;
                ret = usb_control_msg(hub,
                                      USB_DIR_IN | USB_RT_CLASS | USB_RT_OTHER,
                                      USB_REQ_GET_STATUS, 0, p,
                                      &status_buf, 4);
                if (ret < 4) break;
                port_status = (unsigned short)(status_buf & 0xFFFF);
                if (!(port_status & HUB_PORT_RESET)) {
                    reset_done = 1;
                    break;
                }
                sleep_ms(1);
                reset_timeout++;
            }
            if (!reset_done) {
                log_printf("[HUB  ] Port %u reset timed out\n", p);
                continue;
            }

            /* Clear port change flags */
            usb_control_msg(hub, USB_DIR_OUT | USB_RT_CLASS | USB_RT_OTHER,
                            USB_REQ_CLEAR_FEATURE, HUB_FEATURE_C_RESET, p,
                            (void*)0, 0);
            usb_control_msg(hub, USB_DIR_OUT | USB_RT_CLASS | USB_RT_OTHER,
                            USB_REQ_CLEAR_FEATURE, HUB_FEATURE_C_CONNECTION, p,
                            (void*)0, 0);
            usb_control_msg(hub, USB_DIR_OUT | USB_RT_CLASS | USB_RT_OTHER,
                            USB_REQ_CLEAR_FEATURE, HUB_FEATURE_C_ENABLE, p,
                            (void*)0, 0);

            if (!(port_status & HUB_PORT_CONNECTION)) continue;

            /* Determine downstream device speed */
            int speed = USB_SPEED_FULL;
            if (port_status & HUB_PORT_LOW_SPEED) {
                speed = USB_SPEED_LOW;
            } else if (port_status & HUB_PORT_HIGH_SPEED) {
                speed = USB_SPEED_HIGH;
            }

            log_printf("[HUB  ] Port %u reset complete (status=0x%X, speed=%d)\n",
                       p, port_status, speed);
            sleep_ms(20);

            /* Enumerate downstream device with parent hub information */
            usb_enumerate_hub(hub->hci, hub, p, speed);
        }
    }

    return 0;
}
