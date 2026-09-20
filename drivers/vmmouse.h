#ifndef VMMOUSE_H
#define VMMOUSE_H
/* VMware absolute-pointer backdoor ("vmmouse"), the protocol VMware, QEMU
   (hw/input/vmmouse.c, on by default with the pc machine's vmport) and v86
   (src/vmware.js) all speak on I/O port 0x5658. Once enabled, the host
   hands the guest the pointer's ABSOLUTE position (0..65535 across the
   whole screen) instead of PS/2 deltas, so a tap on a phone lands the
   cursor exactly under the finger with no relative walk to get there.

   Probe once at boot, after mouse_init(): returns 1 if a backdoor answered
   and absolute mode is now on, 0 if nothing is there (real hardware, or a
   QEMU run with -machine vmport=off), in which case the PS/2 relative path
   stays the only mouse. Probing is safe on hardware with no backdoor: an
   `inl` of an unclaimed port just returns 0xFFFFFFFF. */
int vmmouse_init(void);
int vmmouse_active(void);

/* Drains every packet the host has queued since the last call. Returns 1
   if anything was read. Called from the PS/2 driver's own poll points
   (mouse_get_delta / mouse_click_edge), never from an IRQ: v86 raises no
   IRQ12 at all for an absolute move (only for a click's PS/2 echo), so the
   queue has to be polled from the GUI's own loop, which the 100Hz PIT
   already wakes. */
int vmmouse_pump(void);

/* The latest absolute position, once per new packet: returns 1 and clears
   the "new" flag, 0 if nothing arrived since the last take. x/y are the
   raw 0..65535 host range; mouse_get_absolute() scales them to pixels. */
int vmmouse_take_absolute(unsigned int *x, unsigned int *y);
/* Accumulated relative packets (the backdoor can also carry deltas, e.g.
   v86 under pointer lock), PS/2 sign convention (+y is up), cleared on take. */
int vmmouse_take_relative(int *dx, int *dy);
/* Button state from the last packet, PS/2 bit layout: 1 left, 2 right, 4 middle. */
int vmmouse_buttons(void);
/* How many left-button presses (0->1 transitions in the packet stream)
   arrived since the last take, cleared on take. Lets a click be detected
   even when its press and release were drained by the same poll, which
   a sampled "is it down right now" check would miss entirely. */
int vmmouse_take_presses(void);
/* Accumulated wheel steps (the backdoor's EDX/z word on every packet,
   always a relative step even while x/y are absolute), PS/2 sign
   convention (positive = scroll up), cleared on take. */
int vmmouse_take_wheel(void);
#endif
