#ifndef STOCKS_H
#define STOCKS_H
/* kernel/stocks.c: the Stocks data side (fetch, parse, STOCKS.TXT); the UI is user/stocks.c. */
void stocks_cmdline(const char *cl);
void stocks_fetch(int range);
void stocks_format_price(int x100, char *buf, int max);
void stocks_format_change(int x100, int *out_dollars, int *out_cents, int *out_sign);
void stocks_ring3_open(void);
extern int stx_sel_hint, stx_range_hint;   /* the row and range the app opens on */
#endif
