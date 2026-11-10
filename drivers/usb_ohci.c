#include "usb_ohci.h"
#include "usb_core.h"
#include "usb_compat.h"
#include "dma.h"
#include "log.h"
#include "string.h"

#define OHCI_MAX_CONTROLLERS 4
#define OHCI_MAX_INTR_SLOTS  12

typedef struct {
    volatile uint32_t control;
    volatile uint32_t tail;
    volatile uint32_t head;
    volatile uint32_t next;
} __attribute__((aligned(16))) ohci_ed_t;

typedef struct {
    volatile uint32_t control;
    volatile uint32_t current_buffer;
    volatile uint32_t next_td;
    volatile uint32_t buffer_end;
} __attribute__((aligned(16))) ohci_td_t;

typedef struct {
    bool used;
    usb_device_t* device;
    usb_endpoint_t* endpoint;
    int transfer_length;
    ohci_ed_t ed;
    ohci_td_t td;
    ohci_td_t dummy;
    uint8_t buffer[64] __attribute__((aligned(16)));
} ohci_intr_t;

typedef struct {
    volatile uint32_t interrupt_table[32];
    volatile uint16_t frame_number;
    volatile uint16_t pad1;
    volatile uint32_t done_head;
    uint8_t reserved[116];
    uint32_t pad2;
} __attribute__((aligned(256))) ohci_hcca_t;

typedef struct {
    ohci_hcca_t hcca;
    ohci_ed_t control_ed;
    ohci_td_t setup_td;
    ohci_td_t data_td;
    ohci_td_t status_td;
    ohci_td_t dummy_td;
    uint8_t setup_buffer[8] __attribute__((aligned(16)));
    uint8_t data_buffer[1024] __attribute__((aligned(16)));
    ohci_intr_t interrupts[OHCI_MAX_INTR_SLOTS];
} ohci_dma_t;

typedef struct {
    volatile uint8_t* mmio;
    const pci_device_t* pci;
    dma_buffer_t dma;
    ohci_dma_t* mem;
    usb_hci_t hci;
    uint16_t ports;
    uint32_t ports_seen;
} ohci_controller_t;

static ohci_controller_t g_ohci[OHCI_MAX_CONTROLLERS];
static uint32_t g_ohci_count = 0;

#define OHCI_REVISION         0x00
#define OHCI_CONTROL          0x04
#define OHCI_COMMAND_STATUS   0x08
#define OHCI_INTERRUPT_STATUS 0x0C
#define OHCI_INTERRUPT_ENABLE 0x10
#define OHCI_INTERRUPT_DISABLE 0x14
#define OHCI_HCCA             0x18
#define OHCI_PERIOD_CURRENT   0x1C
#define OHCI_CONTROL_HEAD     0x20
#define OHCI_CONTROL_CURRENT  0x24
#define OHCI_BULK_HEAD        0x28
#define OHCI_BULK_CURRENT     0x2C
#define OHCI_DONE_HEAD        0x30
#define OHCI_FM_INTERVAL      0x34
#define OHCI_PERIODIC_START   0x40
#define OHCI_LS_THRESHOLD     0x44
#define OHCI_RH_DESCRIPTOR_A  0x48
#define OHCI_RH_DESCRIPTOR_B  0x4C
#define OHCI_RH_STATUS        0x50
#define OHCI_RH_PORT_STATUS   0x54

#define OHCI_CONTROL_PLE      (1u << 2)
#define OHCI_CONTROL_CLE      (1u << 4)
#define OHCI_CONTROL_BLE      (1u << 5)
#define OHCI_CONTROL_HCFS     (3u << 6)
#define OHCI_CONTROL_OPERATIONAL (2u << 6)
#define OHCI_CONTROL_IR       (1u << 8)

#define OHCI_COMMAND_HCR      (1u << 0)
#define OHCI_COMMAND_CLF      (1u << 1)
#define OHCI_COMMAND_OCR      (1u << 3)

#define OHCI_ED_SKIP          (1u << 14)

#define OHCI_PORT_CONNECT     (1u << 0)
#define OHCI_PORT_ENABLE      (1u << 1)
#define OHCI_PORT_SUSPEND     (1u << 2)
#define OHCI_PORT_RESET       (1u << 4)
#define OHCI_PORT_POWER       (1u << 8)
#define OHCI_PORT_LOW         (1u << 9)
#define OHCI_PORT_CHANGES     ((1u << 16) | (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20))

static uint32_t ohci_read(ohci_controller_t* c, uint32_t offset) {
    return *(volatile uint32_t*)(c->mmio + offset);
}

static void ohci_write(ohci_controller_t* c, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(c->mmio + offset) = value;
}

static uint32_t ohci_phys(ohci_controller_t* c, const void* pointer) {
    return (uint32_t)(c->dma.phys_addr +
                      ((const uint8_t*)pointer - (uint8_t*)c->mem));
}

static void ohci_td_init(ohci_controller_t* c, ohci_td_t* td,
                         uint32_t pid, const void* buffer, int length,
                         bool toggle) {
    memset((void*)td, 0, sizeof(*td));
    /* Bit 18: Buffer Rounding (R), Bits 20:19: PID, Bits 25:24: Data Toggle (2 | toggle), CC = 0xF */
    td->control = (1u << 18) | (pid << 19) | ((2u | (toggle ? 1u : 0u)) << 24) |
                  (7u << 21) | (0xFu << 28);
    if (length > 0 && buffer) {
        td->current_buffer = ohci_phys(c, buffer);
        td->buffer_end = td->current_buffer + (uint32_t)length - 1u;
    }
}

static uint32_t ohci_td_cc(const ohci_td_t* td) {
    return (td->control >> 28) & 0xF;
}

static void ohci_set_ed(ohci_controller_t* c, ohci_ed_t* ed,
                        usb_device_t* device, uint8_t endpoint,
                        uint8_t direction, uint16_t max_packet) {
    uint32_t low_speed = device->speed == USB_SPEED_LOW ? (1u << 13) : 0;
    ed->control = (device->address & 0x7F) |
                  ((uint32_t)(endpoint & 0xF) << 7) |
                  ((uint32_t)(direction & 3) << 11) |
                  low_speed |
                  ((uint32_t)(max_packet & 0x7FF) << 16);
    ed->head = 0;
    ed->tail = 0;
    ed->next = 0;
    (void)c;
}

static void ohci_rebuild_periodic(ohci_controller_t* c) {
    uint32_t ctrl = ohci_read(c, OHCI_CONTROL);
    ohci_write(c, OHCI_CONTROL, ctrl & ~OHCI_CONTROL_PLE);
    ohci_write(c, OHCI_PERIOD_CURRENT, 0);

    ohci_ed_t* first = NULL;
    ohci_ed_t* prev = NULL;
    for (int i = 0; i < OHCI_MAX_INTR_SLOTS; i++) {
        ohci_intr_t* slot = &c->mem->interrupts[i];
        if (!slot->used) continue;
        slot->ed.next = 0;
        if (prev) prev->next = ohci_phys(c, &slot->ed);
        else first = &slot->ed;
        prev = &slot->ed;
    }

    uint32_t phys = first ? ohci_phys(c, first) : 0;
    for (int i = 0; i < 32; i++) {
        c->mem->hcca.interrupt_table[i] = phys;
    }
    ohci_write(c, OHCI_CONTROL, ctrl | OHCI_CONTROL_PLE);
}

static int ohci_control(usb_hci_t* hci, usb_device_t* device,
                        const usb_setup_packet_t* setup, void* data,
                        int length, int direction) {
    ohci_controller_t* c = hci->priv;
    if (!c || !device || !setup || length < 0 || length > 1024) return -1;
    dma_sync_for_device(&c->dma);

    memcpy(c->mem->setup_buffer, setup, 8);
    if (length > 0 && data && direction == USB_DIR_OUT) {
        memcpy(c->mem->data_buffer, data, (size_t)length);
    }

    uint16_t max_packet = device->max_packet0 ? device->max_packet0 : 8;
    ohci_set_ed(c, &c->mem->control_ed, device, 0, 0, max_packet);

    ohci_td_t* setup_td = &c->mem->setup_td;
    ohci_td_t* data_td = &c->mem->data_td;
    ohci_td_t* status_td = &c->mem->status_td;
    ohci_td_t* dummy_td = &c->mem->dummy_td;

    memset((void*)dummy_td, 0, sizeof(*dummy_td));
    dummy_td->control = 0;

    /* PID 0: SETUP (toggle=0), PID 1: OUT, PID 2: IN */
    ohci_td_init(c, setup_td, 0, c->mem->setup_buffer, 8, false);

    if (length > 0) {
        uint32_t data_pid = (direction == USB_DIR_IN) ? 2 : 1;
        ohci_td_init(c, data_td, data_pid, c->mem->data_buffer, length, true);
        uint32_t status_pid = (direction == USB_DIR_IN) ? 1 : 2;
        ohci_td_init(c, status_td, status_pid, NULL, 0, true);

        setup_td->next_td = ohci_phys(c, data_td);
        data_td->next_td = ohci_phys(c, status_td);
        status_td->next_td = ohci_phys(c, dummy_td);
    } else {
        ohci_td_init(c, status_td, 2, NULL, 0, true);
        setup_td->next_td = ohci_phys(c, status_td);
        status_td->next_td = ohci_phys(c, dummy_td);
    }

    c->mem->control_ed.head = ohci_phys(c, setup_td);
    c->mem->control_ed.tail = ohci_phys(c, dummy_td);
    c->mem->control_ed.next = 0;

    ohci_write(c, OHCI_CONTROL_HEAD, ohci_phys(c, &c->mem->control_ed));
    ohci_write(c, OHCI_CONTROL_CURRENT, 0);
    ohci_write(c, OHCI_COMMAND_STATUS, OHCI_COMMAND_CLF);
    dma_sync_for_device(&c->dma);

    int result = -1;
    for (int timeout = 0; timeout < 500; timeout++) {
        dma_sync_for_cpu(&c->dma);
        uint32_t status_cc = ohci_td_cc(status_td);
        if (status_cc == 0) {
            result = length;
            break;
        }
        if (ohci_td_cc(setup_td) != 0xF && ohci_td_cc(setup_td) != 0) break;
        if (length > 0 && ohci_td_cc(data_td) != 0xF && ohci_td_cc(data_td) != 0) break;
        if (status_cc != 0xF && status_cc != 0) break;
        usb_delay_ms(1);
    }

    c->mem->control_ed.control |= OHCI_ED_SKIP;
    ohci_write(c, OHCI_CONTROL_HEAD, 0);
    dma_sync_for_device(&c->dma);

    if (result >= 0 && length > 0 && data && direction == USB_DIR_IN) {
        int transferred = length;
        if (data_td->current_buffer != 0) {
            transferred = (int)(data_td->current_buffer - ohci_phys(c, c->mem->data_buffer));
            if (transferred < 0) transferred = 0;
            if (transferred > length) transferred = length;
        }
        memcpy(data, c->mem->data_buffer, (size_t)transferred);
        result = transferred;
    }
    return result;
}

static int ohci_bulk(usb_hci_t* hci, usb_device_t* device,
                     usb_endpoint_t* endpoint, void* data,
                     int length, int direction) {
    (void)hci; (void)device; (void)endpoint; (void)data; (void)length; (void)direction;
    return -1;
}

static ohci_intr_t* ohci_find_slot(ohci_controller_t* c,
                                   usb_device_t* dev,
                                   usb_endpoint_t* ep) {
    for (int i = 0; i < OHCI_MAX_INTR_SLOTS; i++) {
        if (c->mem->interrupts[i].used &&
            c->mem->interrupts[i].device == dev &&
            c->mem->interrupts[i].endpoint == ep) {
            return &c->mem->interrupts[i];
        }
    }
    for (int i = 0; i < OHCI_MAX_INTR_SLOTS; i++) {
        if (!c->mem->interrupts[i].used) {
            memset(&c->mem->interrupts[i], 0, sizeof(ohci_intr_t));
            c->mem->interrupts[i].used = true;
            c->mem->interrupts[i].device = dev;
            c->mem->interrupts[i].endpoint = ep;
            return &c->mem->interrupts[i];
        }
    }
    return NULL;
}

static int ohci_intr_in(usb_hci_t* hci, usb_device_t* dev,
                        usb_endpoint_t* endpoint, void* data, int length) {
    ohci_controller_t* c = hci->priv;
    if (!c || !dev || !endpoint || !data || length <= 0 || length > 64) {
        return -1;
    }
    ohci_intr_t* slot = ohci_find_slot(c, dev, endpoint);
    if (!slot) return -1;

    dma_sync_for_cpu(&c->dma);
    if (slot->ed.tail != 0) {
        uint32_t cc = ohci_td_cc(&slot->td);
        if (cc == 0) {
            int received = slot->transfer_length;
            if (slot->td.current_buffer != 0) {
                received = (int)(slot->td.current_buffer - ohci_phys(c, slot->buffer));
                if (received < 0) received = 0;
                if (received > length) received = length;
            }
            if (received > 0) memcpy(data, slot->buffer, (size_t)received);
            endpoint->toggle ^= 1;
            ohci_td_init(c, &slot->td, 2, slot->buffer, length, endpoint->toggle != 0);
            memset((void*)&slot->dummy, 0, sizeof(slot->dummy));
            slot->td.next_td = ohci_phys(c, &slot->dummy);
            slot->ed.head = ohci_phys(c, &slot->td);
            slot->ed.tail = ohci_phys(c, &slot->dummy);
            slot->transfer_length = length;
            dma_sync_for_device(&c->dma);
            return received;
        }
        if (cc != 0xF) {
            slot->ed.control |= OHCI_ED_SKIP;
            dma_sync_for_device(&c->dma);
            return -1;
        }
        return 0;
    }

    uint16_t max_packet = endpoint->max_packet ? endpoint->max_packet : 8;
    ohci_set_ed(c, &slot->ed, dev, endpoint->number, 2, max_packet);
    ohci_td_init(c, &slot->td, 2, slot->buffer, length, endpoint->toggle != 0);
    memset((void*)&slot->dummy, 0, sizeof(slot->dummy));
    slot->td.next_td = ohci_phys(c, &slot->dummy);
    slot->ed.head = ohci_phys(c, &slot->td);
    slot->ed.tail = ohci_phys(c, &slot->dummy);
    slot->transfer_length = length;
    dma_sync_for_device(&c->dma);
    ohci_rebuild_periodic(c);
    return 0;
}

static void ohci_release(usb_hci_t* hci, usb_device_t* device) {
    ohci_controller_t* c = hci->priv;
    if (!c || !device) return;
    bool rebuild = false;
    for (int i = 0; i < OHCI_MAX_INTR_SLOTS; i++) {
        if (c->mem->interrupts[i].used &&
            c->mem->interrupts[i].device == device) {
            memset(&c->mem->interrupts[i], 0, sizeof(ohci_intr_t));
            rebuild = true;
        }
    }
    if (rebuild) {
        dma_sync_for_device(&c->dma);
        ohci_rebuild_periodic(c);
    }
}

static int ohci_port_reset(usb_hci_t* hci, uint8_t port) {
    ohci_controller_t* c = hci->priv;
    if (!c || port == 0 || port > c->ports) return USB_SPEED_UNKNOWN;
    uint32_t reg = OHCI_RH_PORT_STATUS + 4u * (port - 1);
    uint32_t status = ohci_read(c, reg);
    if (!(status & OHCI_PORT_CONNECT)) return USB_SPEED_UNKNOWN;

    /* Assert Port Reset (bit 4) */
    ohci_write(c, reg, OHCI_PORT_RESET);
    usb_delay_ms(50);
    for (int timeout = 0; timeout < 100; timeout++) {
        status = ohci_read(c, reg);
        if (!(status & OHCI_PORT_RESET)) break;
        usb_delay_ms(1);
    }

    /* Clear PortResetStatusChange (bit 20) and any other status change bits */
    ohci_write(c, reg, OHCI_PORT_CHANGES);
    if (!(status & OHCI_PORT_CONNECT)) return USB_SPEED_UNKNOWN;
    if (!(status & OHCI_PORT_ENABLE)) {
        ohci_write(c, reg, OHCI_PORT_ENABLE);
        usb_delay_ms(10);
        status = ohci_read(c, reg);
    }
    return (status & OHCI_PORT_LOW) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static void ohci_poll(usb_hci_t* hci) {
    ohci_controller_t* c = hci->priv;
    if (!c) return;
    for (uint8_t port = 1; port <= c->ports; port++) {
        uint32_t reg = OHCI_RH_PORT_STATUS + 4u * (port - 1);
        uint32_t bit = 1u << (port - 1);
        uint32_t status = ohci_read(c, reg);
        if (status & OHCI_PORT_CHANGES) {
            ohci_write(c, reg, status & OHCI_PORT_CHANGES);
        }
        if (!(status & OHCI_PORT_CONNECT)) {
            if (c->ports_seen & bit) {
                usb_disconnect_port(hci, port);
                c->ports_seen &= ~bit;
            }
            continue;
        }
        if (c->ports_seen & bit) continue;
        c->ports_seen |= bit;
        uint8_t speed = (uint8_t)ohci_port_reset(hci, port);
        if (speed) {
            log_printf("[OHCI] root port %u %s connected\n",
                       port, usb_speed_name(speed));
            (void)usb_enumerate(hci, port, speed);
        }
    }
}

int ohci_probe(const pci_device_t* device) {
    if (!device || g_ohci_count >= OHCI_MAX_CONTROLLERS) return -1;
    int bar = -1;
    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        if (pci_bar_is_valid(device, i) && !pci_bar_is_io(device, i)) {
            bar = i;
            break;
        }
    }
    if (bar < 0 || pci_bar_address(device, bar) == 0) return -1;
    if (usb_legacy_handoff(device) != 0) return -1;

    ohci_controller_t* c = &g_ohci[g_ohci_count];
    memset(c, 0, sizeof(*c));
    c->pci = device;
    c->mmio = usb_map_mmio(pci_bar_address(device, bar), 0x1000);
    if (!c->mmio) return -1;
    pci_enable_memory(device);
    pci_enable_bus_master(device);
    if (usb_dma_alloc_zero(sizeof(ohci_dma_t), UINT32_MAX, PAGE_SIZE,
                           &c->dma) != 0) {
        return -1;
    }
    c->mem = (ohci_dma_t*)c->dma.virt_addr;
    memset(c->mem, 0, sizeof(*c->mem));

    uint32_t revision = ohci_read(c, OHCI_REVISION) & 0xFF;
    if (revision < 0x10) {
        log_printf("[OHCI] Unsupported revision 0x%x\n", revision);
        return -1;
    }

    /* FreeBSD ohci_controller_init: SMM/BIOS ownership handoff */
    uint32_t ctrl = ohci_read(c, OHCI_CONTROL);
    if (ctrl & OHCI_CONTROL_IR) {
        ohci_write(c, OHCI_COMMAND_STATUS, OHCI_COMMAND_OCR);
        for (int i = 0; i < 100 && (ohci_read(c, OHCI_CONTROL) & OHCI_CONTROL_IR); i++) {
            usb_delay_ms(1);
        }
    }

    ohci_write(c, OHCI_INTERRUPT_DISABLE, 0xFFFFFFFFu);
    ohci_write(c, OHCI_CONTROL, 0);
    usb_delay_ms(10);

    ohci_write(c, OHCI_COMMAND_STATUS, OHCI_COMMAND_HCR);
    for (int i = 0; i < 100 && (ohci_read(c, OHCI_COMMAND_STATUS) & OHCI_COMMAND_HCR); i++) {
        usb_delay_ms(1);
    }

    ohci_write(c, OHCI_HCCA, ohci_phys(c, &c->mem->hcca));
    ohci_write(c, OHCI_CONTROL_HEAD, 0);
    ohci_write(c, OHCI_CONTROL_CURRENT, 0);
    ohci_write(c, OHCI_BULK_HEAD, 0);
    ohci_write(c, OHCI_BULK_CURRENT, 0);
    ohci_write(c, OHCI_INTERRUPT_DISABLE, 0xFFFFFFFFu);
    ohci_write(c, OHCI_INTERRUPT_STATUS, 0xFFFFFFFFu);

    /* FreeBSD ohci.c: 90% periodic start (0x2A2F for 0x2EDF frame interval) */
    ohci_write(c, OHCI_FM_INTERVAL, 0xA7782EDFu);
    ohci_write(c, OHCI_PERIODIC_START, 0x00002A2Fu);
    ohci_write(c, OHCI_LS_THRESHOLD, 0x00000628u);
    ohci_write(c, OHCI_CONTROL,
               OHCI_CONTROL_PLE | OHCI_CONTROL_CLE | OHCI_CONTROL_BLE |
               OHCI_CONTROL_OPERATIONAL);
    usb_delay_ms(20);

    uint32_t descriptor_a = ohci_read(c, OHCI_RH_DESCRIPTOR_A);
    c->ports = descriptor_a & 0xFF;
    if (c->ports == 0 || c->ports > 15) c->ports = 2;
    /* Set LPSC (SetGlobalPower) in RH_STATUS AND individual port power */
    ohci_write(c, OHCI_RH_STATUS, 1u << 16);
    for (uint8_t port = 1; port <= c->ports; port++) {
        uint32_t reg = OHCI_RH_PORT_STATUS + 4u * (port - 1);
        ohci_write(c, reg, OHCI_PORT_POWER);
    }
    uint32_t potpgt = (descriptor_a >> 24) & 0xFFu;
    uint32_t power_delay = potpgt * 2u;
    if (power_delay < 50) power_delay = 50;
    usb_delay_ms(power_delay);

    c->hci.name = "OHCI 1.1";
    c->hci.kind = USB_HCI_KIND_OHCI;
    c->hci.priv = c;
    c->hci.root_ports = (uint8_t)c->ports;
    c->hci.supports_hubs = true;
    c->hci.control = ohci_control;
    c->hci.bulk = ohci_bulk;
    c->hci.intr_in = ohci_intr_in;
    c->hci.port_reset = ohci_port_reset;
    c->hci.device_release = ohci_release;
    c->hci.poll = ohci_poll;
    usb_register_hci(&c->hci);
    g_ohci_count++;
    ohci_poll(&c->hci);
    return 0;
}
