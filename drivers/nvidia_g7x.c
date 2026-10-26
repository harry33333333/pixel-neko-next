#include "nvidia_g7x.h"
#include "pci.h"
#include "log.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

#define NVIDIA_VENDOR_ID       0x10DE
#define PCI_DEV_7300GT_G73     0x0393
#define PCI_DEV_7300GT_G73_B   0x0395
#define PCI_DEV_6200LE_NV44    0x0163
#define PCI_DEV_6600LE_NV43_A  0x00F4
#define PCI_DEV_6600LE_NV43_B  0x0142
#define PCI_CAP_ID_EXP         0x10

extern int screen_w;
extern int screen_h;
extern int screen_bpp;
extern int screen_pitch;
extern unsigned char* lfb;
extern unsigned int lfb_ptr;

static const pci_device_t* g_pci;
static int g_active;
static unsigned int g_vram_size;
static unsigned int g_bar0;
static unsigned int g_bar1;
static unsigned int g_bar1_size;
static const char* g_name = "NVIDIA PCIe";
static const char* g_status = "Not detected";

static unsigned int nvidia_bytes_per_pixel(void)
{
    if (screen_bpp <= 8) return 1;
    if (screen_bpp <= 16) return 2;
    if (screen_bpp <= 24) return 3;
    return 4;
}

static int nvidia_framebuffer_in_bar(unsigned int framebuffer,
                                     unsigned int size)
{
    if (!g_bar1 || !framebuffer || framebuffer < g_bar1) return 0;
    unsigned int offset = framebuffer - g_bar1;
    if (size && offset >= size) return 0;
    return 1;
}

int nvidia_g7x_probe(void)
{
    g_active = 0;
    g_pci = 0;
    g_vram_size = 0;
    g_bar0 = 0;
    g_bar1 = 0;
    g_bar1_size = 0;
    g_status = "NVIDIA G7x PCIe device not found";

    pci_scan_all();
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->vendor != NVIDIA_VENDOR_ID) continue;

        unsigned int vram = 0;
        switch (dev->device) {
            case PCI_DEV_7300GT_G73:
                g_name = "NVIDIA GeForce 7300 GT (G73 PCIe)";
                vram = 256U * 1024U * 1024U;
                break;
            case PCI_DEV_7300GT_G73_B:
                g_name = "NVIDIA GeForce 7300 GT (G73 PCIe)";
                vram = 256U * 1024U * 1024U;
                break;
            case PCI_DEV_6200LE_NV44:
                g_name = "NVIDIA GeForce 6200 LE (NV44 PCIe)";
                vram = 128U * 1024U * 1024U;
                break;
            case PCI_DEV_6600LE_NV43_A:
            case PCI_DEV_6600LE_NV43_B:
                g_name = "NVIDIA GeForce 6600 LE (NV43 PCIe)";
                vram = 128U * 1024U * 1024U;
                break;
            default:
                continue;
        }

        unsigned char pcie = pci_find_capability(dev, PCI_CAP_ID_EXP);
        if (!pcie) {
            log_printf("[NV  ] skipping non-PCIe %X:%X rev=%u\n",
                       dev->vendor, dev->device, dev->revision);
            continue;
        }
        if (!dev->bar_valid[0] || dev->bar_is_io[0] ||
            !dev->bar_valid[1] || dev->bar_is_io[1]) {
            log_printf("[NV  ] %X:%X has unsupported BAR layout\n",
                       dev->vendor, dev->device);
            continue;
        }

        unsigned int bar_size = pci_get_bar_size(dev, 1);
        if (vram && bar_size < (64U * 1024U * 1024U)) {
            bar_size = vram;
        }
        if (!nvidia_framebuffer_in_bar(lfb_ptr, bar_size)) {
            log_printf("[NV  ] VBE LFB 0x%X outside BAR1 0x%X size=0x%X\n",
                       lfb_ptr, dev->bar[1], bar_size);
            continue;
        }
        if (!lfb || screen_w <= 0 || screen_h <= 0 || screen_pitch <= 0) {
            continue;
        }

        pci_enable_memory(dev);
        pci_enable_bus_master(dev);
        g_pci = dev;
        g_bar0 = dev->bar[0];
        g_bar1 = dev->bar[1];
        g_bar1_size = bar_size;
        g_vram_size = vram ? vram : bar_size;
        g_active = 1;
        g_status = "PCIe native LFB; software 2D fallback";
        log_printf("[NV  ] active %s id=%X:%X rev=%X BAR0=%X BAR1=%X size=%uMB\n",
                   g_name, dev->vendor, dev->device, dev->revision,
                   g_bar0, g_bar1, g_vram_size / (1024U * 1024U));
        return 1;
    }
    return 0;
}

int nvidia_g7x_is_active(void)
{
    return g_active;
}

const char* nvidia_g7x_get_name(void)
{
    return g_name;
}

const char* nvidia_g7x_get_status(void)
{
    return g_status;
}

unsigned int nvidia_g7x_get_vram_size(void)
{
    return g_vram_size;
}

void nvidia_g7x_present_rect(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void nvidia_g7x_fill_rect(int x, int y, int width, int height,
                          unsigned int color)
{
    if (!g_active || !lfb) return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > screen_w) width = screen_w - x;
    if (y + height > screen_h) height = screen_h - y;
    if (width <= 0 || height <= 0) return;

    unsigned int bytes = nvidia_bytes_per_pixel();
    for (int row = y; row < y + height; row++) {
        unsigned char* destination = lfb + row * screen_pitch + x * bytes;
        if (bytes == 4) {
            unsigned int* pixel = (unsigned int*)destination;
            for (int column = 0; column < width; column++) {
                pixel[column] = color;
            }
        } else if (bytes == 2) {
            unsigned short* pixel = (unsigned short*)destination;
            for (int column = 0; column < width; column++) {
                pixel[column] = (unsigned short)color;
            }
        } else {
            for (int column = 0; column < width; column++) {
                unsigned char* pixel = destination + column * bytes;
                pixel[0] = (unsigned char)color;
                if (bytes > 1) pixel[1] = (unsigned char)(color >> 8);
                if (bytes > 2) pixel[2] = (unsigned char)(color >> 16);
            }
        }
    }
}

void nvidia_g7x_screen_blt(int sx, int sy, int dx, int dy,
                           int width, int height)
{
    if (!g_active || !lfb) return;
    if (sx < 0) { width += sx; dx -= sx; sx = 0; }
    if (sy < 0) { height += sy; dy -= sy; sy = 0; }
    if (dx < 0) { width += dx; sx -= dx; dx = 0; }
    if (dy < 0) { height += dy; sy -= dy; dy = 0; }
    if (sx + width > screen_w) width = screen_w - sx;
    if (sy + height > screen_h) height = screen_h - sy;
    if (dx + width > screen_w) width = screen_w - dx;
    if (dy + height > screen_h) height = screen_h - dy;
    if (width <= 0 || height <= 0) return;

    unsigned int bytes = nvidia_bytes_per_pixel();
    int row = height - 1;
    int row_end = -1;
    int row_step = -1;
    if (dy < sy) {
        row = 0;
        row_end = height;
        row_step = 1;
    }
    for (; row != row_end; row += row_step) {
        unsigned char* source = lfb + (sy + row) * screen_pitch + sx * bytes;
        unsigned char* destination =
            lfb + (dy + row) * screen_pitch + dx * bytes;
        int count = width * (int)bytes;
        for (int i = 0; i < count; i++) destination[i] = source[i];
    }
}

void nvidia_g7x_sync(void)
{
    /* VBE mode owns synchronization until the G7x GPU command path is enabled. */
}
