/* Host stub for the parser fuzz build: drivers/http.c reads the PIT tick
   counter through kernel/irq.h, and http_wait_idle only ever runs inside
   the kernel. A counter that advances per call keeps every timeout loop
   finite on the host. */
#pragma once
static inline unsigned int ticks(void) { static unsigned int t; return t += 16; }
