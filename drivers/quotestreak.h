#ifndef QUOTESTREAK_H
#define QUOTESTREAK_H

/* Quotestreak, the film-quote guessing game: the third app moved out of
   kernel.c after Keyrate and Toroid, same pattern (drivers/app_quotestreak.c).
   drivers/app_quotestreak.h is a different thing, the generated HTML blob
   `serveapp quotestreak` hands out. */
void quotestreak_open(void);

#endif
