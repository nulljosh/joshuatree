/* Classic Bluetooth (btc.c, 12.1): `bt find` and `bt connect`, headphones, speakers and keyboards, on bt.c's UART. */
#ifndef BTC_H
#define BTC_H
int btc_command(const char *q, unsigned n);                     /* bt.c: what follows "bt "; 1 when it was find, connect or disconnect */
void btc_event(unsigned code, const unsigned char *p, unsigned n);   /* bt.c: every HCI event */
void btc_done(unsigned op, const unsigned char *r, unsigned n); /* bt.c: Command Complete (returns) or Status (status) for a non-step command */
int btc_poll(void);                                             /* bt.c, when its command slot is free: 1 when a command went out */
int btc_busy(void);                                             /* a find, a connection or a link: keep the UART read */
int btc_acl(const unsigned char *p, unsigned n);                /* bt.c: an ACL packet, 1 when it was the classic link's */
void btc_flush(void);                                           /* bt.c's loop: ACL packets waiting for the radio's buffers */
void btc_status(void);                                          /* bt.c's `bt`: the saved classic device */
int btc_audio_live(void);                                      /* 13.0: headphones ready to play */
int btc_audio_room(void);                                      /* a frame fits */
void btc_audio(const short *pcm);                               /* audio.c: 128 stereo frames at 44.1 kHz */
#endif
