/* Bluetooth over HCI (bt.c). BT1: power, the patch load, the version, and an LE scan that lists what it hears. BT2:
   `bt pair` connects the mouse from that scan, pairs (Just Works, keys in RAM) and feeds its boot reports to the pointer.
   Everything is polled from the main loop and every wait ends; nothing here blocks. */
#ifndef BT_H
#define BT_H
void bt_init(void);                          /* main.c, at boot: find and power the radio, start bring-up, return at once */
void bt_poll(void);                          /* main.c's loop: moves bring-up and a scan along */
int bt_busy(void);                           /* 1 while bring-up, a scan or the mouse needs the loop to keep turning */
int bt_command(const char *q, unsigned n);   /* cmd.c: `bt`, `bt scan`, `bt pair [NAME]`; 1 when the line was one */
#endif
