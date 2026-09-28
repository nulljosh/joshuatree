#ifndef CALCULATOR_H
#define CALCULATOR_H

/* Calculator: the fourth app moved out of kernel.c, same pattern as
   Keyrate, Toroid and Quotestreak (drivers/app_calculator.c). */
void calculator_open(void);
int calculator_test(void); /* kernel.c's "calctest" console command: 1 pass, 0 fail (prints its own diagnostics) */

#endif
