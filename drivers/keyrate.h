#ifndef KEYRATE_H
#define KEYRATE_H

/* Keyrate, the typing test: the first app in its own compiled unit
   (drivers/app_keyrate.c), the pattern for moving the rest out of kernel.c.
   drivers/app_keyrate.h is a different thing, the generated HTML blob
   `serveapp keyrate` hands out. */
void keyrate_open(void);

#endif
