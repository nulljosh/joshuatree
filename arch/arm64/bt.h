/* Bluetooth over HCI (bt.c). BT1: power, the patch load, the version, and an LE scan that lists what it hears. BT2:
   `bt pair` connects the mouse from that scan, pairs (Just Works) and feeds its boot reports to the pointer.
   BT3: the pairing is saved to JT/BTPAIR.BIN and a later boot reconnects it with no scan (`bt reconnect`, `bt forget`).
   Everything is polled from the main loop and every wait ends; nothing here blocks. */
#ifndef BT_H
#define BT_H
void bt_init(void);                          /* main.c, at boot: find and power the radio, start bring-up, return at once */
void bt_poll(void);                          /* main.c's loop: moves bring-up and a scan along */
int bt_busy(void);
int bt_settling(void);                       /* 6.87.0: bring-up, a scan, pairing or reconnect tries: not the time for a slow idle job */                           /* 1 while bring-up, a scan or the mouse needs the loop to keep turning */
int bt_starting(void);                       /* 1 while the radio is still loading its patch, the boot splash waits for it */
int bt_command(const char *q, unsigned n);   /* cmd.c: `bt`, `bt scan`, `bt pair [NAME]`, `bt reconnect`, `bt forget`; 1 when the line was one */
#endif
