/* Curbfind: Craigslist deal rankings for Vancouver. Left: ranked list
   sorted by deal score, selected row highlighted; up/down or click selects.
   Right: listing title, price, neighbourhood, deal score as a 10-segment bar,
   and a brief reason why it is a good deal. Sample listings compiled in.
   Included by kernel.c after render_wrapped_text. */

typedef struct {
    const char *title;
    const char *price;
    const char *neighbourhood;
    int score;
    const char *reason;
} CfListing;

static const CfListing CF_LISTINGS[] = {
    {"Barely used office desk", "$40", "Kitsilano", 9, "Solid construction, minimal wear, great price for IKEA quality."},
    {"Mountain bike, full suspension", "$120", "Commercial Drive", 9, "Recent tune-up, all gears work, pedals and grips included."},
    {"LED monitor 27-inch", "$60", "Downtown", 8, "IPS panel, 60Hz, perfect for work or gaming, tested fully."},
    {"Bookshelf wooden unit", "$35", "Burnaby", 8, "Solid oak, five shelves, heavy but sturdy, matches decor."},
    {"Gaming headset wireless", "$45", "Coquitlam", 7, "Noise cancellation works, battery lasts 20 hours, minor ear pad wear."},
    {"Office chair leather", "$80", "West Vancouver", 7, "Adjustable height and recline, needs minor wheel replacement soon."},
    {"Coffee table glass top", "$50", "Mount Pleasant", 7, "Modern design, one tiny edge chip not visible when placed against wall."},
    {"Acoustic guitar softcase bundle", "$85", "Victoria", 6, "Tuned and ready, strings good, case has one zipper issue."},
    {"Desk lamp LED", "$25", "Maple Ridge", 6, "Bright 5000K color temperature, three brightness levels, USB charging."},
    {"Shelving unit metal", "$30", "Surrey", 5, "Industrial style, sturdy metal frame, some surface rust but structurally sound."},
};

#define CF_COUNT ((int)(sizeof(CF_LISTINGS) / sizeof(CF_LISTINGS[0])))
#define CF_LIST_X  20
#define CF_LIST_W  220
#define CF_INFO_X  260
#define CF_INFO_W  ((int)window_width() - CF_INFO_X - 24)

/* Sorted indices: cf_order[0] is the index of the highest-scored listing */
static int cf_order[CF_COUNT];
static int cf_sel = 0; /* currently selected position in cf_order */

static void cf_init_order(void) {
    for (int i = 0; i < CF_COUNT; i++) {
        cf_order[i] = i;
    }
    /* Sort by score descending */
    for (int i = 0; i < CF_COUNT - 1; i++) {
        for (int j = i + 1; j < CF_COUNT; j++) {
            if (CF_LISTINGS[cf_order[i]].score < CF_LISTINGS[cf_order[j]].score) {
                int tmp = cf_order[i];
                cf_order[i] = cf_order[j];
                cf_order[j] = tmp;
            }
        }
    }
}

static void cf_draw(void) {
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Curbfind");

    /* Left: ranked list sorted by score */
    int list_top = 56, item_h = 28, max_items = (int)window_height() - list_top - 50;
    int items_shown = CF_COUNT < max_items / item_h ? CF_COUNT : max_items / item_h;

    for (int i = 0; i < items_shown; i++) {
        int y = list_top + i * item_h;
        int listing_idx = cf_order[i];
        const CfListing *listing = &CF_LISTINGS[listing_idx];
        unsigned int bg = (i == cf_sel) ? 0x00E2D8CC : 0x00F1EDE7;
        unsigned int fg = (i == cf_sel) ? 0x001C1C1E : 0x0075726E;
        window_rect(CF_LIST_X, y, CF_LIST_W, item_h - 2, bg);

        /* Rank number */
        char rank[4]; int r = 0, n = i + 1;
        if (n >= 10) rank[r++] = (char)('0' + n / 10);
        rank[r++] = (char)('0' + n % 10);
        rank[r++] = '.'; rank[r] = 0;
        font_draw_string(rank, CF_LIST_X + 6, y + 6, fg, -1);

        /* Trim title by real pixel width */
        const char *title = listing->title;
        char short_title[52]; int len = 0;
        while (title[len] && len < 47) { short_title[len] = title[len]; len++; }
        short_title[len] = 0;
        int room = CF_LIST_W - 70; /* leave room for price on right */
        if (font_string_width(short_title) > room) {
            while (len > 0 && (short_title[len] = 0, font_string_width(short_title) + font_string_width("...") > room)) len--;
            while (len > 0 && short_title[len - 1] == ' ') len--;
            short_title[len] = '.'; short_title[len + 1] = '.'; short_title[len + 2] = '.'; short_title[len + 3] = 0;
        }
        font_draw_string(short_title, CF_LIST_X + 30, y + 6, fg, -1);

        /* Price, right-aligned in row */
        const char *price = listing->price;
        int price_w = font_string_width(price);
        font_draw_string(price, CF_LIST_X + CF_LIST_W - price_w - 8, y + 6, fg, -1);
    }

    /* Right: listing detail */
    if (cf_sel < CF_COUNT) {
        int info_top = 56;
        int listing_idx = cf_order[cf_sel];
        const CfListing *listing = &CF_LISTINGS[listing_idx];

        font_draw_string(listing->title, CF_INFO_X, info_top, 0x001C1C1E, -1);

        /* Price and neighbourhood on second line */
        char price_neighbourhood[128];
        int len = 0;
        const char *p = listing->price;
        while (*p) price_neighbourhood[len++] = *p++;
        price_neighbourhood[len++] = ' ';
        price_neighbourhood[len++] = '|';
        price_neighbourhood[len++] = ' ';
        p = listing->neighbourhood;
        while (*p) price_neighbourhood[len++] = *p++;
        price_neighbourhood[len] = 0;
        font_draw_string(price_neighbourhood, CF_INFO_X, info_top + 20, 0x0075726E, -1);

        /* Deal score label */
        font_draw_string("Deal score:", CF_INFO_X, info_top + 44, 0x001C1C1E, -1);

        /* Draw score bar: 10 segments, each segment is proportional */
        int bar_x = CF_INFO_X;
        int bar_y = info_top + 60;
        int bar_height = 8;
        int segment_width = 16; /* 10 segments * 16 pixels = 160 pixels */
        int score = listing->score;

        /* Background (empty) bar */
        window_rect(bar_x, bar_y, segment_width * 10, bar_height, 0x00E8E6E1);

        /* Filled bar based on score */
        if (score > 0 && score <= 10) {
            window_rect(bar_x, bar_y, segment_width * score, bar_height, 0x002F7B4F);
        }

        /* Score text (e.g., "9/10") */
        char score_str[4];
        int sl = 0;
        if (score >= 10) score_str[sl++] = '1';
        score_str[sl++] = (char)('0' + score % 10);
        score_str[sl++] = '/';
        score_str[sl++] = '1';
        score_str[sl++] = '0';
        score_str[sl] = 0;
        font_draw_string(score_str, bar_x + segment_width * 10 + 8, bar_y, 0x001C1C1E, -1);

        /* Reason why it is a good deal */
        int wrap_y = info_top + 84;
        render_wrapped_text(listing->reason, CF_INFO_X, wrap_y, CF_INFO_W,
                           (int)window_height() - wrap_y - 50, 0x001C1C1E);
    }

    font_draw_string("Sample listings, not live   up/down or click to select   esc closes", 20, (int)window_height() - 30, 0x0075726E, -1);
}

static void gui_launch_curbfind(void) {
    cf_init_order();
    cf_sel = 0;
    mouse_click_edge_sync();
    for (;;) {
        cf_draw();
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;

        if (k == KEY_UP && cf_sel > 0) cf_sel--;
        else if (k == KEY_DOWN && cf_sel < CF_COUNT - 1) cf_sel++;
        else if (k == KEY_CLICK) {
            int vx = app_cursor_x - app_view_x, vy = app_cursor_y - app_view_y;
            int item_h = 28, list_top = 56;
            if (vy >= list_top && vx >= CF_LIST_X && vx < CF_LIST_X + CF_LIST_W) {
                int sel = (vy - list_top) / item_h;
                if (sel < CF_COUNT) cf_sel = sel;
                else return;
            } else return; /* titlebar X or off the app */
        }
    }
}
