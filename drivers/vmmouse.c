/* VMware absolute-pointer backdoor. Wire protocol taken from three real
   implementations that all agree, not guessed: Linux
   drivers/input/mouse/vmmouse.c (the guest side, the register discipline
   below is its VMMOUSE_CMD macro), QEMU hw/input/vmmouse.c + vmport.c (the
   host side the native app talks to) and v86 src/vmware.js (the host side
   the landing page's in-browser demo talks to, read in the vendored
   landing/v86/libv86.js: port 22104 = 0x5658, magic 1447909480 =
   0x564D5868, commands 10/39/40/41, enable 1161905490 = 0x45414552,
   request-absolute 1396851026 = 0x53424152, ID 876762442 = 0x3442554A,
   relative flag 65536, data returned as EAX=status EBX=x ECX=y EDX=z).

   The call: EAX = magic, ECX = command, EBX = argument, DX = port, then a
   single 32-bit `inl`. The hypervisor answers by rewriting EAX/EBX/ECX/EDX
   behind the instruction's back, so all four are in/out operands, and ESI/
   EDI are listed too because real VMware clobbers them (Linux does the
   same). Two host-specific traps this file exists to get right:
   (1) QEMU checks the FULL ECX against its command table, so the command
       must be written with no high bits set, not OR'd into anything;
   (2) QEMU's vmmouse_data() disables the device (status 0xFFFF) if the
       guest asks for more words than are queued, so reads are exactly 1
       word for the ID and exactly 4 per packet, only while status says
       at least that many are waiting, the same loop shape Linux uses. */
#include "vmmouse.h"
#include "serial.h"

typedef unsigned int u32;

#define VMMOUSE_PORT   0x5658
#define VMMOUSE_MAGIC  0x564D5868u /* "VMXh" */

#define CMD_GETVERSION          10
#define CMD_ABSPOINTER_DATA     39
#define CMD_ABSPOINTER_STATUS   40
#define CMD_ABSPOINTER_COMMAND  41

#define VMMOUSE_CMD_ENABLE            0x45414552u
#define VMMOUSE_CMD_REQUEST_ABSOLUTE  0x53424152u
#define VMMOUSE_ID                    0x3442554Au
#define VMMOUSE_ERROR                 0xFFFF0000u
#define VMMOUSE_RELATIVE_PACKET       0x00010000u
#define VMMOUSE_LEFT_BUTTON           0x20
#define VMMOUSE_RIGHT_BUTTON          0x10
#define VMMOUSE_MIDDLE_BUTTON         0x08

static u32 backdoor(u32 cmd, u32 arg, u32 *out_ebx, u32 *out_ecx, u32 *out_edx) {
    u32 eax = VMMOUSE_MAGIC, ebx = arg, ecx = cmd, edx = VMMOUSE_PORT, esi = 0, edi = 0;
    __asm__ volatile ("inl %%dx, %%eax"
                      : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx), "+S"(esi), "+D"(edi)
                      : : "memory");
    if (out_ebx) *out_ebx = ebx;
    if (out_ecx) *out_ecx = ecx;
    if (out_edx) *out_edx = edx;
    return eax;
}

static int active = 0;
static int have_abs = 0;
static u32 abs_x = 0, abs_y = 0;
static int rel_dx = 0, rel_dy = 0, rel_dz = 0;
static int buttons = 0;
static int presses = 0;   /* left-button 0->1 transitions seen in the packet stream, not yet taken */
static u32 prev_st = 0;   /* button bits of the previous packet, for the transition checks */
static int logged_first = 0;
static int logged_first_wheel = 0;

/* Linux vmmouse_enable(): enable, confirm the ID word the enable queued,
   then ask for absolute mode. Returns 1 only if every step answered as
   a real vmmouse would; a bare vmport with no mouse behind it (or nothing
   at all) fails the ID check and leaves PS/2 in charge. */
static int vmmouse_enable(void) {
    backdoor(CMD_ABSPOINTER_COMMAND, VMMOUSE_CMD_ENABLE, 0, 0, 0);
    u32 status = backdoor(CMD_ABSPOINTER_STATUS, 0, 0, 0, 0);
    if ((status & VMMOUSE_ERROR) == VMMOUSE_ERROR) return 0;
    if ((status & 0xFFFF) == 0) return 0;
    u32 id = backdoor(CMD_ABSPOINTER_DATA, 1, 0, 0, 0);
    if (id != VMMOUSE_ID) return 0;
    backdoor(CMD_ABSPOINTER_COMMAND, VMMOUSE_CMD_REQUEST_ABSOLUTE, 0, 0, 0);
    return 1;
}

int vmmouse_init(void) {
    u32 ebx = 0;
    u32 version = backdoor(CMD_GETVERSION, 0, &ebx, 0, 0);
    /* Linux vmmouse_detect(): a real backdoor echoes the magic in EBX and
       never answers 0xFFFFFFFF; an unclaimed port leaves EBX alone. */
    if (ebx != VMMOUSE_MAGIC || version == 0xFFFFFFFFu) { active = 0; return 0; }
    active = vmmouse_enable();
    return active;
}

int vmmouse_active(void) { return active; }

static void put_u32(u32 v) {
    char b[12]; int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    char s[12]; int j = 0;
    while (i) s[j++] = b[--i];
    s[j] = 0;
    serial_puts(s);
}

int vmmouse_pump(void) {
    if (!active) return 0;
    int got = 0;
    for (int guard = 0; guard < 64; guard++) {
        u32 status = backdoor(CMD_ABSPOINTER_STATUS, 0, 0, 0, 0);
        if ((status & VMMOUSE_ERROR) == VMMOUSE_ERROR) {
            /* Both hosts drop the device on queue overflow (v86: >1024
               words unread; QEMU: an over-long data request). Linux
               re-runs the enable sequence on this; so do we, once per pump. */
            active = vmmouse_enable();
            if (!active) serial_puts("vmmouse: host reported error, re-enable failed, back to PS/2\n");
            return got;
        }
        if ((status & 0xFFFF) < 4) break;
        u32 x = 0, y = 0, z = 0;
        u32 st = backdoor(CMD_ABSPOINTER_DATA, 4, &x, &y, &z);
        /* v0.76.58: the EDX word of every absolute-pointer packet (both
           hosts, real VMware, QEMU's hw/input/vmmouse.c and v86's own
           src/vmware.js) is always the wheel's signed relative step, even
           while x/y are absolute -- confirmed live: a QMP "wheel-up"
           button event landed here as z=-1 (0xFFFFFFFF) on every one of
           5 real presses sent, while pure motion packets carried z=0.
           This code read it into a local and threw it away, so
           mouse_get_wheel() never had anything to return whenever
           vmmouse is active (the default under QEMU: mouse_handle_byte
           already drops the raw PS/2 packet contents once vmmouse takes
           over, see its own "phase kept, contents ignored" comment), and
           real PS/2 hardware has its own separate bug (mouse_init below
           never sends the IntelliMouse 0xF3 magic sequence, so a real
           3-byte mouse never emits a 4th/wheel byte at all either). Sign
           flips relative to the raw z: the PS/2 path's own convention
           (mouse.c, wheel_nibble folded straight into accum_dz) is
           "negative dz = scroll down", i.e. positive = up, while this
           backdoor's z came back -1 for a wheel-UP press, the opposite
           sign. Negate here so both input paths feed accum_dz the same
           polarity. */
        rel_dz += -(int)z;
        if (z && !logged_first_wheel) {
            /* One line, once: the real artifact tools/checks/vmmouse-wheel-check.sh
               asserts on, proof a real wheel step reached this driver at all. */
            logged_first_wheel = 1;
            serial_puts("vmmouse: first wheel step z="); put_u32(z); serial_puts("\n");
        }
        if (st & VMMOUSE_RELATIVE_PACKET) {
            rel_dx += (int)x; rel_dy += (int)y;
        } else {
            abs_x = x & 0xFFFF; abs_y = y & 0xFFFF; have_abs = 1;
            if (!logged_first) {
                /* One line, once: the real artifact tools/vmmouse-check.sh
                   and mobiletest.mjs both assert on, proof the host's
                   absolute position actually reached this driver. */
                logged_first = 1;
                serial_puts("vmmouse: first absolute packet x="); put_u32(abs_x);
                serial_puts(" y="); put_u32(abs_y); serial_puts("\n");
            }
        }
        buttons = ((st & VMMOUSE_LEFT_BUTTON) ? 1 : 0)
                | ((st & VMMOUSE_RIGHT_BUTTON) ? 2 : 0)
                | ((st & VMMOUSE_MIDDLE_BUTTON) ? 4 : 0);
        got = 1;
        /* Real bug, caught by the kernel's own serial trace under
           mobiletest.mjs, not by reasoning: a tap's press and release
           (80ms apart) both sat in the queue by the time the GUI loop
           next polled, one drain swallowed both, and every consumer that
           samples "is the button down now" (gui_run's just_pressed /
           just_released, mouse_click_edge's baseline compare) saw 0 -> 0
           and the click never happened. Two guards: count every 0->1 of
           the left button here, where the packet stream is still visible
           (mouse_click_edge takes the count, so a whole press+release
           inside one drain still fires exactly once); and stop draining
           at any button change, so the next poll sees the release as its
           own step and the sampled consumers see held, then not held,
           as two real states. The rest of the queue is not lost, only
           deferred to the next poll a few ms later. */
        u32 btn = VMMOUSE_LEFT_BUTTON | VMMOUSE_RIGHT_BUTTON | VMMOUSE_MIDDLE_BUTTON;
        int changed = ((st ^ prev_st) & btn) != 0;
        if ((st & VMMOUSE_LEFT_BUTTON) && !(prev_st & VMMOUSE_LEFT_BUTTON)) presses++;
        prev_st = st & btn;
        if (changed) break;
    }
    return got;
}

int vmmouse_take_presses(void) {
    int n = presses; presses = 0;
    return n;
}

int vmmouse_take_absolute(u32 *x, u32 *y) {
    if (!have_abs) return 0;
    *x = abs_x; *y = abs_y; have_abs = 0;
    return 1;
}

int vmmouse_take_relative(int *dx, int *dy) {
    if (!rel_dx && !rel_dy) return 0;
    *dx = rel_dx; *dy = rel_dy; rel_dx = rel_dy = 0;
    return 1;
}

/* v0.76.58: wheel steps accumulated in vmmouse_pump above, taken the same
   drain-and-reset shape vmmouse_take_relative already uses. */
int vmmouse_take_wheel(void) {
    int z = rel_dz; rel_dz = 0;
    return z;
}

int vmmouse_buttons(void) { return buttons; }
