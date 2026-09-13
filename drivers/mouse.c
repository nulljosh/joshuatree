/* PS/2 mouse: the 8042 keyboard controller's second port. Same controller
   the keyboard already uses (ports 0x60/0x64), just addressed through the
   0xD4 "write to the mouse, not the keyboard" prefix. */
#include "mouse.h"

typedef unsigned char  u8;
typedef unsigned short u16;

static inline u8   inb(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outb(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }

#define CTRL_STATUS 0x64
#define CTRL_CMD    0x64
#define CTRL_DATA   0x60

static void wait_input_clear(void)  { int t = 100000; while (t-- && (inb(CTRL_STATUS) & 0x02)) {} }
static void wait_output_ready(void) { int t = 100000; while (t-- && !(inb(CTRL_STATUS) & 0x01)) {} }

static void mouse_write(u8 val) {
    wait_input_clear();
    outb(CTRL_CMD, 0xD4);   /* "next byte on 0x60 is for the mouse" */
    wait_input_clear();
    outb(CTRL_DATA, val);
}

static u8 mouse_read(void) {
    wait_output_ready();
    return inb(CTRL_DATA);
}

void mouse_init(void) {
    wait_input_clear();
    outb(CTRL_CMD, 0xA8); /* enable the auxiliary (mouse) device */

    wait_input_clear();
    outb(CTRL_CMD, 0x20); /* read controller command byte */
    u8 status = mouse_read();
    status |= 0x02;   /* enable IRQ12 */
    status &= ~0x20;  /* enable the mouse clock */
    wait_input_clear();
    outb(CTRL_CMD, 0x60);
    wait_input_clear();
    outb(CTRL_DATA, status);

    mouse_write(0xF6); mouse_read(); /* set defaults, ack */
    mouse_write(0xF4); mouse_read(); /* enable data reporting, ack */
}

/* ponytail: no wheel/5-byte packet support, just the standard 3-byte
   packet every PS/2 mouse speaks out of the box. */
static u8 packet[3];
static int packet_index = 0;
static int accum_dx = 0, accum_dy = 0, last_buttons = 0;
static int dirty = 0;

void mouse_handle_byte(u8 byte) {
    /* byte 0 of a real packet always has bit3 set; resync if we're out of
       phase (e.g. IRQ12 fired once for something that wasn't a real packet
       start) instead of quietly misinterpreting every byte after. */
    if (packet_index == 0 && !(byte & 0x08)) return;

    packet[packet_index++] = byte;
    if (packet_index < 3) return;
    packet_index = 0;

    int x_sign = packet[0] & 0x10;
    int y_sign = packet[0] & 0x20;
    int dx = packet[1] - ((packet[0] << 4) & 0x100);
    int dy = packet[2] - ((packet[0] << 3) & 0x100);
    (void)x_sign; (void)y_sign; /* sign is already folded into dx/dy above */

    accum_dx += dx;
    accum_dy += -dy; /* PS/2 reports +y as up; screen coordinates want +y as down */
    last_buttons = packet[0] & 0x07;
    dirty = 1;
}

int mouse_get_delta(int *dx, int *dy, int *buttons) {
    *dx = accum_dx;
    *dy = accum_dy;
    *buttons = last_buttons;
    int was_dirty = dirty;
    accum_dx = 0; accum_dy = 0; dirty = 0;
    return was_dirty;
}
