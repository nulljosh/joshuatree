/* Bluetooth over HCI (bt.c). Slice BT1: power, the patch load, the version, and an LE scan that lists what it hears.
   Everything is polled from the main loop and every wait ends; nothing here blocks. */
#ifndef BT_H
#define BT_H
void bt_init(void);                          /* main.c, at boot: find and power the radio, start bring-up, return at once */
void bt_poll(void);                          /* main.c's loop: moves bring-up and a scan along */
int bt_busy(void);                           /* 1 while bring-up or a scan needs the loop to keep turning */
int bt_command(const char *q, unsigned n);   /* cmd.c: `bt` and `bt scan`; 1 when the line was one of them */
#endif
