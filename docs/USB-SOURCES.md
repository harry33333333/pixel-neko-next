# USB Implementation Sources and Licensing

The USB subsystem in Pixel Neko Next implements USB 1.1 (UHCI) and USB 2.0 (EHCI) host controllers, USB Hub, and USB Human Interface Device (HID) keyboard and mouse support.

The subsystem architecture, Queue Head (QH) / Transfer Descriptor (TD) lifecycle, Data Toggle synchronization, split-transaction scheduling, BIOS/OS handoff, and HID report parsing are derived from and modeled after the FreeBSD USB subsystem and NetBSD USB stack under the BSD-2-Clause license:

- `sys/dev/usb/controller/uhci.c` (UHCI 1.1 Host Controller)
- `sys/dev/usb/controller/ehci.c` (EHCI 2.0 Host Controller & TT)
- `sys/dev/usb/usb_core.c` and `sys/dev/usb/usb_transfer.c` (Core Subsystem & Transfers)
- `sys/dev/usb/input/ums.c` and `sys/dev/usb/input/ukbd.c` (Mouse & Keyboard Drivers)
- `sys/dev/usb/usb_hid.c` (HID Report Descriptor Parser & `hid_get_data`)

## Copyright Notices

```text
Copyright (c) 2008 Hans Petter Selasky <hselasky@FreeBSD.org>
Copyright (c) 1998, 2004 Lennart Augustsson <lennart@augustsson.net>
Copyright (c) 2001-2004 Lennart Augustsson <lennart@augustsson.net>
Copyright (c) 2026 Pixel Neko Project

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
```

## Technical Highlights
1. **UHCI 1.1 Host Controller**: Single-transfer Queue Head (QH) interrupt polling model matching FreeBSD's transfer retirement. Data Toggle flips only on valid non-NAK completions, eliminating race conditions, ring buffer overruns, and cursor flight/drift.
2. **EHCI 2.0 Host Controller**: Standard asynchronous QH schedules for control messages, periodic list schedules for interrupt transfers, MMIO EECP BIOS ownership handoff, and Transaction Translator (TT) split transactions for Full/Low-speed devices behind High-Speed hubs.
3. **HID Mouse & Keyboard**: Conforms to standard USB HID Boot Protocol and Report Protocol decoding. Features native absolute coordinate scaling and button extraction for virtualization environments (VMware Virtual USB Mouse `0x0E0F:0x0003`) as well as relative desktop mice.
