#ifndef MOUSE_H
#define MOUSE_H
/* Enables the PS/2 mouse (the second port on the 8042 keyboard controller)
   and starts accepting its IRQ12 packets. Call once at boot, after
   irq_install(). */
void mouse_init(void);

/* Feeds one raw byte from IRQ12 into the 3-byte packet state machine.
   Called from irq.c's irq_handler, not meant to be called directly. */
void mouse_handle_byte(unsigned char byte);

/* Fetches accumulated movement since the last call and clears it. Returns
   1 if the mouse moved or a button changed since the last call, 0 if
   nothing happened (dx/dy/buttons are still written either way, just 0). */
int mouse_get_delta(int *dx, int *dy, int *buttons);
#endif
