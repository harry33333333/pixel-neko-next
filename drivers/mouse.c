// mouse.c
#include "mouse.h"
#include "port.h"
#include "wm.h"
#include "irq.h"
#include "timer.h"
#include "usb_core.h"
#include "usb_hid.h"
#include "log.h"

static int mx = 160, my = 100;
static int btn = 0;
static int ps2_btn = 0;
static int usb_btn = 0;
static int cycle = 0;
static unsigned char buf[3];
static int init_done = 1;
static int ps2_initialized = 0;

#define MOUSE_EVENT_QUEUE_SIZE 64
static mouse_button_event_t button_events[MOUSE_EVENT_QUEUE_SIZE];
static volatile unsigned int event_head;
static volatile unsigned int event_tail;

static unsigned int saved[16][16];
static int saved_x = -1, saved_y = -1;

int mouse_speed = 2;

void mouse_set_speed(int s) { if(s>=1 && s<=5) mouse_speed = s; }
int mouse_get_speed(void) { return mouse_speed; }

static int kbc_wait_write(void) {
    for (int timeout = 0; timeout < 100000; timeout++) {
        if (!(inb(0x64) & 0x02)) return 1;
    }
    return 0;
}

static inline int kbc_wait_read(void) {
    for (int timeout = 0; timeout < 100000; timeout++) {
        if (inb(0x64) & 0x01) return 1;
    }
    return 0;
}

static void kbc_drain(void) {
    for (int i = 0; i < 64; i++) {
        if (!(inb(0x64) & 0x01)) break;
        (void)inb(0x60);
        for (volatile int delay = 0; delay < 100; delay++);
    }
}

static int kbc_command(unsigned char command) {
    if (!kbc_wait_write()) return 0;
    outb(0x64, command);
    return 1;
}

static int kbc_write_data(unsigned char data) {
    if (!kbc_wait_write()) return 0;
    outb(0x60, data);
    return 1;
}

static int kbc_read_data(unsigned char* data) {
    if (!data || !kbc_wait_read()) return 0;
    *data = inb(0x60);
    return 1;
}

static void kbc_settle(void) {
    for (volatile int delay = 0; delay < 20000; delay++);
    kbc_drain();
}

static int ps2_mouse_command(unsigned char command) {
    unsigned char response;
    if (!kbc_command(0xD4)) return 0;
    if (!kbc_write_data(command)) return 0;
    if (!kbc_read_data(&response)) return 0;
    return response == 0xFA;
}

void mouse_init(void)
{
    if (ps2_initialized) {
        init_done = 1;
        return;
    }

    /*
     * CSM firmware can expose USB input through an SMM-backed 8042. The
     * controller may accept reads but never complete reconfiguration, so keep
     * the firmware's IRQ1/IRQ12 mapping until it explicitly releases USB.
     */
    if (usb_firmware_legacy_active()) {
        ps2_initialized = 2;
        cycle = 0;
        event_head = 0;
        event_tail = 0;
        init_done = 1;
        log_printf("[PS2 ] firmware legacy USB active; preserving PS/2 emulation\n");
        return;
    }

    kbc_drain();

    unsigned char cfg = 0;
    int have_cfg = 0;
    if (kbc_command(0x20) && kbc_read_data(&cfg) && !(cfg & 0x80)) {
        have_cfg = 1;
    }

    if (!have_cfg) {
        ps2_initialized = 2;
        cycle = 0;
        event_head = 0;
        event_tail = 0;
        init_done = 1;
        log_printf("[PS2 ] no i8042 command byte; preserving firmware mapping\n");
        return;
    }

    unsigned char wanted = (unsigned char)((cfg | 0x43) & ~0x30);
    wanted &= 0x7F;

    if (cfg & 0x20) {
        kbc_command(0xA8);
        kbc_settle();
    }

    if (wanted != cfg) {
        if (!kbc_command(0x60) || !kbc_write_data(wanted)) {
            ps2_initialized = 2;
            log_printf("[PS2 ] controller is not safely writable; using firmware mapping\n");
            return;
        }
        kbc_settle();

        unsigned char check = 0;
        if (!kbc_command(0x20) || !kbc_read_data(&check) ||
            (check & 0x7F) != wanted) {
            ps2_initialized = 2;
            log_printf("[PS2 ] command byte verification failed; using firmware mapping\n");
            return;
        }
    }

    /*
     * Do not blindly reset an emulated mouse. Only enable streaming after the
     * device acknowledges the auxiliary-port command.
     */
    if (ps2_mouse_command(0xF6)) {
        kbc_settle();
        if (!ps2_mouse_command(0xF4)) {
            log_printf("[PS2 ] mouse did not acknowledge streaming enable\n");
        }
    } else {
        log_printf("[PS2 ] no mouse ACK; preserving firmware streaming state\n");
    }

    ps2_initialized = 1;
    cycle = 0;
    event_head = 0;
    event_tail = 0;
    ps2_btn = 0;
    usb_btn = 0;
    btn = 0;
    init_done = 1;
}

static void queue_button_event(int old_buttons, int new_buttons, int x, int y)
{
    int diff = (old_buttons ^ new_buttons) & 0x07;
    if (!diff) return;

    int pressed = (diff & new_buttons) != 0;
    int released = (diff & old_buttons) != 0;

    unsigned int next = (event_head + 1U) % MOUSE_EVENT_QUEUE_SIZE;
    if (next == event_tail) {
        event_tail = (event_tail + 1U) % MOUSE_EVENT_QUEUE_SIZE;
    }

    button_events[event_head].x = x;
    button_events[event_head].y = y;
    button_events[event_head].buttons = new_buttons & 0x07;
    button_events[event_head].pressed = pressed;
    button_events[event_head].released = released;
    button_events[event_head].tick = global_tick;
    event_head = next;

    static int s_logged = 0;
    if (s_logged < 32) {
        s_logged++;
        log_printf("[MOUSE] button event: old=%d new=%d press=%d rel=%d at (%d,%d)\n",
                   old_buttons, new_buttons, pressed, released, x, y);
    }
}

void mouse_handle_byte(unsigned char d)
{
    unsigned int flags = irq_save_flags();
    if (cycle == 0) {
        // Bit 3 of byte 0 in a PS/2 packet MUST be 1.
        // Also bits 6 and 7 (overflow) should be 0.
        if (!(d & 0x08) || (d & 0xC0)) {
            irq_restore_flags(flags);
            return;
        }
    }
    buf[cycle++] = d;
    if (cycle == 3) {
        cycle = 0;
        int old_btn = btn;
        ps2_btn = buf[0] & 0x07;
        btn = ps2_btn | usb_btn;

        /* If an active USB mouse is present, USB mouse owns cursor position.
         * Only allow PS/2 deltas to update coordinates when no USB mouse is present. */
        if (usb_hid_mouse_count() == 0) {
            int dx = buf[1];
            int dy = buf[2];
            if (buf[0] & 0x10) dx = dx - 256;
            if (buf[0] & 0x20) dy = dy - 256;
            mx += dx * mouse_speed / 2;
            my -= dy * mouse_speed / 2; // Y axis is inverted
            if (mx < 0) mx = 0;
            if (mx > screen_w - 1) mx = screen_w - 1;
            if (my < 0) my = 0;
            if (my > screen_h - 1) my = screen_h - 1;
        }

        /* Forward button state transitions from PS/2 if button edge changed */
        if (btn != old_btn) {
            queue_button_event(old_btn, btn, mx, my);
        }
    }
    irq_restore_flags(flags);
}

void mouse_handle_usb_report(int buttons, int dx, int dy, int dz)
{
    (void)dz;
    unsigned int flags = irq_save_flags();
    int old_btn = btn;
    usb_btn = buttons & 0x07;
    btn = usb_btn | ps2_btn;

    int step_x = (dx * mouse_speed) / 2;
    if (dx != 0 && step_x == 0) step_x = (dx > 0) ? 1 : -1;
    int step_y = (dy * mouse_speed) / 2;
    if (dy != 0 && step_y == 0) step_y = (dy > 0) ? 1 : -1;

    mx += step_x;
    my += step_y;

    if (mx < 0) mx = 0;
    if (mx > screen_w - 1) mx = screen_w - 1;
    if (my < 0) my = 0;
    if (my > screen_h - 1) my = screen_h - 1;

    if (btn != old_btn) {
        queue_button_event(old_btn, btn, mx, my);
    }
    irq_restore_flags(flags);
}

void mouse_handle_usb_absolute(int buttons, int x, int y)
{
    unsigned int flags = irq_save_flags();
    int old_btn = btn;
    usb_btn = buttons & 0x07;
    btn = usb_btn | ps2_btn;

    mx = x;
    my = y;
    if (mx < 0) mx = 0;
    if (mx > screen_w - 1) mx = screen_w - 1;
    if (my < 0) my = 0;
    if (my > screen_h - 1) my = screen_h - 1;

    if (btn != old_btn) {
        queue_button_event(old_btn, btn, mx, my);
    }
    irq_restore_flags(flags);
}

void mouse_notify_usb_detached(void)
{
    unsigned int flags = irq_save_flags();
    int old_btn = btn;
    usb_btn = 0;
    btn = ps2_btn;
    if (old_btn != btn) {
        queue_button_event(old_btn, btn, mx, my);
    }
    irq_restore_flags(flags);
}

void mouse_get(int* x, int* y, int* b)
{
    unsigned int flags = irq_save_flags();
    if (x) *x = mx;
    if (y) *y = my;
    if (b) *b = btn & 0x07;
    irq_restore_flags(flags);
}

int mouse_take_button_event(mouse_button_event_t* event)
{
    unsigned int flags = irq_save_flags();
    if (!event || event_tail == event_head) {
        irq_restore_flags(flags);
        return 0;
    }

    *event = button_events[event_tail];
    event_tail = (event_tail + 1U) % MOUSE_EVENT_QUEUE_SIZE;
    irq_restore_flags(flags);
    return 1;
}

void mouse_draw(unsigned char* vga)
{
    if (!init_done) return;
    int bpp_bytes = (screen_bpp <= 8) ? 1 : ((screen_bpp <= 16) ? 2 : ((screen_bpp <= 24) ? 3 : 4));
    unsigned int flags = irq_save_flags();
    int x = mx, y = my;
    irq_restore_flags(flags);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) {
            if (y+i < screen_h && x+j < screen_w) {
                int offset = (y+i) * screen_pitch + (x+j) * bpp_bytes;
                if (screen_bpp <= 8) saved[i][j] = vga[offset];
                else if (screen_bpp <= 16) saved[i][j] = *((unsigned short*)&vga[offset]);
                else if (screen_bpp == 24) {
                    saved[i][j] = vga[offset] | (vga[offset+1] << 8) | (vga[offset+2] << 16);
                } else {
                    saved[i][j] = *((unsigned int*)&vga[offset]);
                }
            } else {
                saved[i][j] = 0;
            }
        }
    saved_x = x;
    saved_y = y;

    static const char* cursor_shape[16] = {
        "X               ",
        "XX              ",
        "X.X             ",
        "X..X            ",
        "X...X           ",
        "X....X          ",
        "X.....X         ",
        "X......X        ",
        "X.......X       ",
        "X........X      ",
        "X.........X     ",
        "X......XXXX     ",
        "X..X..X         ",
        "X.X X..X        ",
        "XX  X..X        ",
        "     XX         "
    };

    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            if (cursor_shape[i][j] != ' ') {
                int px = x + j, py = y + i;
                if (px < screen_w && py < screen_h) {
                    PUT_PIXEL(vga, px, py, cursor_shape[i][j] == 'X' ? 15 : 0);
                }
            }
        }
    }
}

void mouse_reset(unsigned char* vga)
{
    if (saved_x >= 0) {
        int bpp_bytes = (screen_bpp <= 8) ? 1 : ((screen_bpp <= 16) ? 2 : ((screen_bpp <= 24) ? 3 : 4));
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++)
                if (saved_y+i < screen_h && saved_x+j < screen_w) {
                    int offset = (saved_y+i) * screen_pitch + (saved_x+j) * bpp_bytes;
                    if (screen_bpp <= 8) vga[offset] = (unsigned char)saved[i][j];
                    else if (screen_bpp <= 16) *((unsigned short*)&vga[offset]) = (unsigned short)saved[i][j];
                    else if (screen_bpp == 24) {
                        vga[offset]   = (unsigned char)(saved[i][j] & 0xFF);
                        vga[offset+1] = (unsigned char)((saved[i][j] >> 8) & 0xFF);
                        vga[offset+2] = (unsigned char)((saved[i][j] >> 16) & 0xFF);
                    } else {
                        *((unsigned int*)&vga[offset]) = saved[i][j];
                    }
                }
    }
    saved_x = -1;
    saved_y = -1;
}

void mouse_invalidate(void)
{
    saved_x = -1;
    saved_y = -1;
}
