/* v71+ (0.7x.0): Stocks. Same shape as weather.h and reminders.h: a simple list
   of a few real well-known tickers with plausible static price/change data,
   clearly presented as demo data, not live. No network fetch (real plain-HTTP
   stock sources all force HTTPS per the real curl tests documented in
   roadmap.md), no persistence (this is read-only demo data baked in), just
   a clean list view: up/down to pick, enter to see full details, esc closes.

   Follows the exact same app-shape precedent Weather/Mail/Calendar set (dock
   icon, window, esc closes).

   Static data: a few well-known tickers (AAPL, MSFT, GOOGL, AMZN, TSLA) with
   plausible price and daily change data, clearly marked as demo. No real
   market data, no live quote fetches, no API calls. The point is to show that
   the kernel can render a real app with live user interaction (selecting a
   stock, viewing details) with data baked in at build time, same way the
   codebase's own Stocks/Weather/Keyrate apps do for demo purposes.
   Real implementations in the codebase (epiphany, etc) have real backends and
   would do live fetches; this is the local-only, read-only precedent. */

#define STOCKS_MAX 5
#define STOCKS_SYMBOL_MAX 8
#define STOCKS_NAME_MAX 32

typedef struct {
    char symbol[STOCKS_SYMBOL_MAX];     /* e.g. "AAPL" */
    char name[STOCKS_NAME_MAX];         /* e.g. "Apple Inc." */
    int price_x100;                     /* $150.42 as 15042, so price is always x100 for exact cents */
    int change_x100;                    /* +$5.34 as +534, negative for down */
} stocks_entry_t;

static stocks_entry_t stocks_entries[STOCKS_MAX] = {
    {"AAPL", "Apple Inc.", 23800, 250},      /* $238.00, +$2.50 (+1.06%) */
    {"MSFT", "Microsoft", 41900, -180},      /* $419.00, -$1.80 (-0.43%) */
    {"GOOGL", "Alphabet Inc.", 14200, 350},  /* $142.00, +$3.50 (+2.52%) */
    {"AMZN", "Amazon.com", 19100, -320},     /* $191.00, -$3.20 (-1.65%) */
    {"TSLA", "Tesla Inc.", 24200, 870},      /* $242.00, +$8.70 (+3.73%) */
};

static void stocks_format_price(int x100, char *buf, int max) {
    /* Format $15042 as "150.42" */
    int dollars = x100 / 100;
    int cents = x100 % 100;
    int pos = 0;

    if (dollars == 0) {
        buf[pos++] = '0';
    } else {
        char tmp[16]; int ti = 0;
        int v = dollars;
        if (v == 0) tmp[ti++] = '0';
        while (v > 0) { tmp[ti++] = '0' + v % 10; v /= 10; }
        while (ti > 0 && pos < max - 1) buf[pos++] = tmp[--ti];
    }

    if (pos < max - 1) buf[pos++] = '.';
    if (pos < max - 1) buf[pos++] = '0' + (cents / 10);
    if (pos < max - 1) buf[pos++] = '0' + (cents % 10);

    if (pos >= max) pos = max - 1;
    buf[pos] = 0;
}

static void stocks_format_change(int x100, int *out_dollars, int *out_cents, int *out_sign) {
    *out_sign = (x100 < 0) ? -1 : 1;
    x100 = x100 < 0 ? -x100 : x100;
    *out_dollars = x100 / 100;
    *out_cents = x100 % 100;
}

static void gui_launch_stocks(void) {
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Stocks");

    int sel = 0;

    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Stocks");

        font_draw_string("Demo data   up/down to pick   enter views details   esc closes", 20, 52, 0x00807468, -1);

        for (int i = 0; i < STOCKS_MAX; i++) {
            int y = 84 + i * 22;
            stocks_entry_t *stock = &stocks_entries[i];

            if (i == sel) {
                window_rect(16, y - 4, (int)window_width() - 32, 20, 0x00EDE6DC);
            }

            /* Symbol (e.g., "AAPL") */
            font_draw_string(stock->symbol, 28, y, 0x001C1C1E, -1);

            /* Name (e.g., "Apple Inc.") */
            font_draw_string(stock->name, 100, y, 0x001C1C1E, -1);

            /* Price ($238.00) */
            static char price_str[16];
            stocks_format_price(stock->price_x100, price_str, sizeof(price_str));
            font_draw_string(price_str, 260, y, 0x001C1C1E, -1);

            /* Change (+2.50 or -1.80) in color (green=up, red=down) */
            int change_dollars, change_cents, change_sign;
            stocks_format_change(stock->change_x100, &change_dollars, &change_cents, &change_sign);

            static char change_str[32];
            int pos = 0;
            if (change_sign < 0) change_str[pos++] = '-';
            else change_str[pos++] = '+';

            if (change_dollars == 0) {
                change_str[pos++] = '0';
            } else {
                char tmp[8]; int ti = 0;
                int v = change_dollars;
                while (v > 0) { tmp[ti++] = '0' + v % 10; v /= 10; }
                while (ti > 0 && pos < 30) change_str[pos++] = tmp[--ti];
            }
            change_str[pos++] = '.';
            change_str[pos++] = '0' + (change_cents / 10);
            change_str[pos++] = '0' + (change_cents % 10);
            change_str[pos] = 0;

            unsigned int change_color = (change_sign > 0) ? 0x0041854B : 0x00B51616;  /* green or red */
            font_draw_string(change_str, 340, y, change_color, -1);
        }

        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();

        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel < STOCKS_MAX - 1) sel++;
        else if (k == KEY_ENTER) {
            /* Show full stock details */
            stocks_entry_t *stock = &stocks_entries[sel];
            for (;;) {
                window_clear(GUI_BG);
                gui_draw_app_titlebar("Stocks");

                font_draw_string(stock->symbol, 20, 48, 0x00807468, -1);
                font_draw_string(stock->name, 20, 68, 0x001C1C1E, -1);

                window_rect(20, 92, (int)window_width() - 40, 1, 0x00E0D8CE);

                /* Price line */
                static char detail_str[64];
                int pos = 0;
                const char *price_label = "Price: $";
                while (*price_label && pos < 60) detail_str[pos++] = *price_label++;
                stocks_format_price(stock->price_x100, &detail_str[pos], 64 - pos);
                while (detail_str[pos]) pos++;
                font_draw_string(detail_str, 20, 110, 0x001C1C1E, -1);

                /* Change line */
                pos = 0;
                const char *change_label = "Change: ";
                while (*change_label && pos < 60) detail_str[pos++] = *change_label++;

                int change_dollars, change_cents, change_sign;
                stocks_format_change(stock->change_x100, &change_dollars, &change_cents, &change_sign);

                if (change_sign < 0) detail_str[pos++] = '-';
                else detail_str[pos++] = '+';
                detail_str[pos++] = '$';

                if (change_dollars == 0) {
                    detail_str[pos++] = '0';
                } else {
                    char tmp[8]; int ti = 0;
                    int v = change_dollars;
                    while (v > 0) { tmp[ti++] = '0' + v % 10; v /= 10; }
                    while (ti > 0 && pos < 60) detail_str[pos++] = tmp[--ti];
                }
                detail_str[pos++] = '.';
                detail_str[pos++] = '0' + (change_cents / 10);
                detail_str[pos++] = '0' + (change_cents % 10);
                detail_str[pos] = 0;

                unsigned int change_color = (change_sign > 0) ? 0x0041854B : 0x00B51616;
                font_draw_string(detail_str, 20, 135, change_color, -1);

                /* Disclaimer */
                pos = 0;
                const char *pct_label = "Note: This is demo data, not live market quotes.";
                while (*pct_label && pos < 60) detail_str[pos++] = *pct_label++;
                detail_str[pos] = 0;
                font_draw_string(detail_str, 20, 170, 0x0075726E, -1);

                gui_wait_close();
                return;
            }
        }
    }
}
