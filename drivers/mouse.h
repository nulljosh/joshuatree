#ifndef MOUSE_H
#define MOUSE_H
/* Enables the PS/2 mouse (the second port on the 8042 keyboard controller)
   and starts accepting its IRQ12 packets. Call once at boot, after
   irq_install(). */
void mouse_init(void);

/* Feeds one raw byte from IRQ12 into the 3-byte packet state machine.
   Called from irq.c's irq_handler, not meant to be called directly. */
void mouse_handle_byte(unsigned char byte);

/* Fetches accumulated movement and wheel since the last call and clears it.
   Returns 1 if the mouse moved, a button changed, or the wheel scrolled since
   the last call; 0 if nothing happened (dx/dy/dz/buttons are still written
   either way, just 0). */
int mouse_get_delta(int *dx, int *dy, int *buttons);

/* Fetches accumulated wheel delta since the last call and clears it. Returns
   the signed wheel delta (-16 to +15 on each call, negative = scroll down).
   Called independently, doesn't clear dx/dy; call mouse_get_delta to get
   those and clear the entire accumulator. This is here so callers that only
   care about wheel scroll don't need a dummy dx/dy buffer. */
int mouse_get_wheel(void);

/* Absolute position, only when the VMware backdoor (vmmouse.h) is live
   and a new packet arrived since the last call: writes the pointer's
   position scaled into a w x h pixel space and returns 1. Returns 0 (x/y
   untouched) on plain PS/2 hardware, so callers apply the relative delta
   first and then let this override it, and the same code runs on both. */
int mouse_get_absolute(int *x, int *y, int w, int h);

/* Edge-triggered left-click detection, separate from mouse_get_delta's own
   continuous "is it currently held" reporting (which real dragging needs).
   Real, reported bug this exists to fix: a "click to close" check that
   just tests mouse_get_delta's buttons field fires every single poll for
   as long as last_buttons stays nonzero, and it stays nonzero until a
   fresh packet clears it, real PS/2 hardware or a host trackpad's press/
   release packets translated through QEMU are not guaranteed to always
   arrive as a clean pair. One missed or coalesced release packet leaves
   it looking permanently "held", so every future wait-for-input poll
   (typing a retry key included) sees a phantom click and exits instantly.
   mouse_click_edge_sync() latches the CURRENT held state as the new
   baseline (call once when an app that waits on this starts, so a button
   already down from the very click that opened it isn't mistaken for a
   fresh click); mouse_click_edge() then returns 1 only on a genuine
   transition from up to down since that baseline, once per transition. */
void mouse_click_edge_sync(void);
int mouse_click_edge(void);
#endif
