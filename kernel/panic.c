/* The panic screen (moved out of kernel.c in 4.20.4): what a ring-0 exception leaves on the GUI. */
#include "window.h"
#include "font.h"
#include "version.h"

/* Draw a readable panic screen on the GUI when a ring-0 exception occurs.
   Called from kernel/idt.c's exception handler. Displays the exception name,
   fault address (for page faults), EIP, kernel version, and system message. */
void gui_panic_screen(const char *name, unsigned int fault_addr, unsigned int eip) {
    /* Check if GUI is active by seeing if window dimensions are non-zero */
    if (window_width() == 0 || window_height() == 0) return;

    /* Whole screen, not whatever app viewport was current when it faulted
       (the first version painted inside the Terminal's window). */
    window_clear_viewport();
    window_clear(0x00FAF8F6);

    int h = (int)window_height();
    int text_color = 0x001C1C1E;  /* dark text */

    /* Title: exception name at 1/4 down the screen */
    font_draw_string(name, 32, h / 4, text_color, -1);

    /* Exception details */
    int y = h / 4 + 32;

    /* For page faults, show the fault address */
    if (fault_addr != 0) {
        char buf[80];
        int i = 0;
        const char *prefix = "Page fault at: 0x";
        while (*prefix) buf[i++] = *prefix++;
        /* Inline hex conversion */
        unsigned int val = fault_addr;
        for (int j = 0; j < 8; j++) {
            unsigned int nib = (val >> (28 - j * 4)) & 0xF;
            buf[i++] = nib < 10 ? '0' + nib : 'A' + (nib - 10);
        }
        buf[i] = '\0';
        font_draw_string(buf, 32, y, text_color, -1);
        y += 24;
    }

    /* EIP (instruction pointer) */
    {
        char buf[80];
        int i = 0;
        const char *prefix = "EIP: 0x";
        while (*prefix) buf[i++] = *prefix++;
        unsigned int val = eip;
        for (int j = 0; j < 8; j++) {
            unsigned int nib = (val >> (28 - j * 4)) & 0xF;
            buf[i++] = nib < 10 ? '0' + nib : 'A' + (nib - 10);
        }
        buf[i] = '\0';
        font_draw_string(buf, 32, y, text_color, -1);
    }

    /* Kernel version */
    {
        char buf[80];
        int i = 0;
        const char *prefix = "Joshua Tree ";
        while (*prefix) buf[i++] = *prefix++;
        const char *ver = JT_VERSION_STR;
        while (*ver) buf[i++] = *ver++;
        buf[i] = '\0';
        font_draw_string(buf, 32, y + 32, text_color, -1);
    }

    /* Main message lines */
    int message_y = h / 2 + 60;
    font_draw_string("Joshua Tree stopped to protect your files.", 32, message_y, text_color, -1);
    font_draw_string("Hold the power button to restart.", 32, message_y + 32, text_color, -1);

    /* Display the panic screen */
    window_present();
}
