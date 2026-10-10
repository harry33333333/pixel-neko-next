#include "port.h"
#include "mouse.h"
#include "wm.h"
#include "keyboard.h"

int screen_w = 320;
int screen_h = 200;
int screen_bpp = 8;
int screen_pitch = 320;
unsigned char backbuffer[1024 * 768 * 2];
static unsigned int lfb_ptr = 0xA0000;
unsigned short palette16[256];

extern int vbe_set_mode(int mode);
extern int vbe_get_mode_info(int mode);

struct vbe_mode_info {
    short attributes;
    char winA, winB;
    short granularity;
    short win_size;
    short segmentA, segmentB;
    int win_func_ptr;
    short bytes_per_scanline;
    short x_res, y_res;
    char x_char_size, y_char_size;
    char planes, bpp, banks;
    char memory_model, bank_size, image_pages;
    char reserved1;
    char red_mask, red_pos;
    char green_mask, green_pos;
    char blue_mask, blue_pos;
    char reserved_mask, reserved_pos;
    char direct_color_attributes;
    int phys_base_ptr;
    int reserved2;
    short reserved3;
} __attribute__((packed));

extern void check_revert_timer(void);

static void set_vga_palette_full(void)
{
    outb(0x3C8, 0);
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        
        if (i < 16) {
            // 标准VGA 16色
            static const unsigned char vga16[16][3] = {
                {0x00,0x00,0x00}, {0x00,0x00,0x2A}, {0x00,0x2A,0x00}, {0x00,0x2A,0x2A},
                {0x2A,0x00,0x00}, {0x2A,0x00,0x2A}, {0x2A,0x15,0x00}, {0x2A,0x2A,0x2A},
                {0x15,0x15,0x15}, {0x15,0x15,0x3F}, {0x15,0x3F,0x15}, {0x15,0x3F,0x3F},
                {0x3F,0x15,0x15}, {0x3F,0x15,0x3F}, {0x3F,0x3F,0x15}, {0x3F,0x3F,0x3F}
            };
            r = vga16[i][0];
            g = vga16[i][1];
            b = vga16[i][2];
        } else if (i < 32) {
            // 16级灰度
            int level = (i - 16) * 4;
            r = g = b = level;
        } else {
            // 其余颜色：均匀分布
            int n = i - 32;
            r = (n % 6) * 12;
            g = ((n / 6) % 6) * 12;
            b = ((n / 36) % 6) * 12;
        }
        
        outb(0x3C9, r);
        outb(0x3C9, g);
        outb(0x3C9, b);
        palette16[i] = RGB16(r * 4, g * 4, b * 4); // VGA DAC is 6-bit (0-63), scale to 8-bit (0-255)
    }
}


static void wait_vsync(void) {
    int timeout = 50000;
    while ((inb(0x3DA) & 8) && --timeout);
    timeout = 50000;
    while (!(inb(0x3DA) & 8) && --timeout);
}



#include "idt.h"
#include "mm.h"

extern void bios_set_mode(int mode);

void set_resolution(int w, int h, int bpp)
{
    // Bounds check to ensure resolution fits within backbuffer
    if (w <= 0 || h <= 0 || w > 1024 || h > 768) return;
    if ((unsigned int)(w * h * (bpp / 8)) > sizeof(backbuffer)) return;

    if (w == 320 && h == 200 && bpp == 8) {
        asm volatile("cli");
        bios_set_mode(0x13);
        idt_reload();
        pic_remap();
        asm volatile("sti");

        screen_w = 320; screen_h = 200; screen_bpp = 8; screen_pitch = 320;
        lfb_ptr = 0xA0000;
        
        // Restore DAC palette for 8-bit mode
        set_vga_palette_full();
        return;
    }
    
    int mode = 0;
    if (w == 640 && h == 480 && bpp == 16) mode = 0x111;
    else if (w == 800 && h == 600 && bpp == 16) mode = 0x114;
    else if (w == 1024 && h == 768 && bpp == 16) mode = 0x117;
    else return;
    
    asm volatile("cli");
    if (vbe_get_mode_info(mode) == 0x004F) {
        struct vbe_mode_info* info = (struct vbe_mode_info*)0x9000;
        lfb_ptr = info->phys_base_ptr;
        screen_pitch = info->bytes_per_scanline;
    }
    
    vbe_set_mode(mode | 0x4000); // 0x4000 for LFB
    idt_reload();
    pic_remap();
    asm volatile("sti");

    screen_w = w; screen_h = h; screen_bpp = bpp;

    // 16-bit 模式也需要初始化 palette16[] 映射表
    set_vga_palette_full();
}

void kernel_main(void)
{
    pmm_init();
    set_resolution(800, 600, 16);
    idt_init();
    
    unsigned char* v = backbuffer;
    int mx, my, mb, last_mx = -1, last_my = -1, last_mb = 0;

    // 用正确的 16-bit 颜色填充 backbuffer
    unsigned short* v16 = (unsigned short*)v;
    unsigned short gray = palette16[7];
    for (int i = 0; i < 1024 * 768; i++) {
        v16[i] = gray;
    }

    wm_init();
    wm_draw(v);
    mouse_init();

    while (1) {
        check_revert_timer();
        
        while (inb(0x64) & 0x01) {
            unsigned char st = inb(0x64);
            unsigned char data = inb(0x60);
            if (st & 0x20) mouse_handle_byte(data);
            else keyboard_handle_byte(data);
        }
        mouse_get(&mx, &my, &mb);
        int mb_press = (mb & 1) && !(last_mb & 1);
        int mb_release = !(mb & 1) && (last_mb & 1);
        last_mb = mb;

        int mr = wm_need_mouse_reset();
        int moved = (mx != last_mx || my != last_my);
        
        if (mr || moved) {
            if (last_mx >= 0) mouse_reset(v);
        }

        wm_update(v, mx, my, mb, mb_press, mb_release);
        int new_mr = wm_need_mouse_reset();
        if (new_mr) {
            mr = 1;
            if (last_mx >= 0) mouse_reset(v);
        }
        
        wm_draw(v);

        if (!wm_is_dragging() && (mr || moved || last_mx == -1)) {
            mouse_draw(v);
            last_mx = mx; last_my = my;
        } else if (wm_is_dragging()) {
            last_mx = -1;
        }

        wait_vsync();
        int bytes_per_line = screen_w * (screen_bpp / 8);
        if (bytes_per_line == screen_pitch) {
            int dwords = (bytes_per_line * screen_h) / 4;
            unsigned int* s = (unsigned int*)v;
            unsigned int* d = (unsigned int*)lfb_ptr;
            for (int i = 0; i < dwords; i++) {
                d[i] = s[i];
            }
        } else {
            // Line-by-line copy handling hardware scanline pitch padding
            for (int y = 0; y < screen_h; y++) {
                unsigned char* s = v + y * bytes_per_line;
                unsigned char* d = (unsigned char*)lfb_ptr + y * screen_pitch;
                int dwords = bytes_per_line / 4;
                for (int i = 0; i < dwords; i++) {
                    ((unsigned int*)d)[i] = ((unsigned int*)s)[i];
                }
                for (int i = dwords * 4; i < bytes_per_line; i++) {
                    d[i] = s[i];
                }
            }
        }
    }
}