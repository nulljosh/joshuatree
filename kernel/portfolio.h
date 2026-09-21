/* Portfolio. The /os landing page frames this whole kernel full screen, so
   the OS itself needs one place that shows off the real fleet: every other
   shipped app under heyitsmejosh.com, grouped the way a person would ask
   for them (Life/Read/Make/Play/Dev), not the build order they landed in.

   Same shape as search.h/contacts.h: a fixed-size static table, no
   persistence (there is nothing here to persist, it is a static catalog),
   gui_prompt.h's chrome/content split so scrolling and selection never
   redraw the titlebar. This kernel has no general "open any URL live"
   hook: the fleet apps already ported in as gui_launch_html are baked
   static snapshots, not a browser, and Search's own scope is the local
   filesystem, not the network. So selecting an entry does not try to
   launch anything; it shows the entry's URL on a detail line, same
   fallback the request itself called for when no clean hook exists. */

typedef struct {
    const char *name;   /* NULL for a header row */
    const char *desc;   /* header text when name is NULL, else one-liner */
    const char *url;    /* "" when the app has no web app */
    int is_header;
} pf_row_t;

static const pf_row_t PF_ROWS[] = {
    {0, "Life", "", 1},
    {"Epiphany",  "finance dashboard",         "epiphany.heyitsmejosh.com", 0},
    {"Healstack", "health and supplement tracker", "healstack.heyitsmejosh.com", 0},
    {"Windgate",  "guided breathing",          "windgate.heyitsmejosh.com", 0},
    {"Talli",     "benefits admin",            "talli.heyitsmejosh.com", 0},
    {"Homeward",  "lost and found pets",       "homeward.heyitsmejosh.com", 0},
    {"Roost",     "real estate browsing",      "roost.heyitsmejosh.com", 0},
    {"HomeQi",    "feng shui home check",      "homeqi.heyitsmejosh.com", 0},
    {"Weather",   "forecast and live conditions", "weather.heyitsmejosh.com", 0},

    {0, "Read", "", 1},
    {"Bookrank",  "book summaries",            "bookrank.heyitsmejosh.com", 0},
    {"Inkpress",  "RSS reader",                "", 0},
    {"Sidewise",  "news bias reader",          "sidewise.heyitsmejosh.com", 0},
    {"Wordroot",  "etymology",                 "wordroot.heyitsmejosh.com", 0},
    {"Fieldbook", "science explained plainly", "fieldbook.heyitsmejosh.com", 0},
    {"Lexly",     "language learning",         "lexly.heyitsmejosh.com", 0},

    {0, "Make", "", 1},
    {"Block Frame", "wireframes in text",      "wiretext.heyitsmejosh.com", 0},
    {"Curvely",   "equation grapher",          "curvely.heyitsmejosh.com", 0},
    {"Numen",     "calculator canvas",         "numen.heyitsmejosh.com", 0},
    {"Plain",     "text editor",               "", 0},
    {"Voxprint",  "on-device transcription",   "", 0},
    {"Dream",     "dream journal",             "dream.heyitsmejosh.com", 0},
    {"Costanza",  "poetry",                    "costanza.heyitsmejosh.com", 0},
    {"Sparkjar",  "idea forum",                "sparkjar.heyitsmejosh.com", 0},

    {0, "Play", "", 1},
    {"Quotestreak", "quote guessing",          "quotestreak.heyitsmejosh.com", 0},
    {"Keyrate",   "typing test",               "keyrate.heyitsmejosh.com", 0},
    {"NYC",       "Times Square sim",          "nyc.heyitsmejosh.com", 0},
    {"Conway",    "Game of Life on a torus",   "toroid.heyitsmejosh.com", 0},
    {"Swing",     "random video chat",         "swing.heyitsmejosh.com", 0},

    {0, "Dev", "", 1},
    {"Nimble",    "instant answers",           "nimble.heyitsmejosh.com", 0},
    {"Cadence",   "commit tracker",            "cadence.heyitsmejosh.com", 0},
    {"Tripwire",  "API drift watcher",         "tripwire.heyitsmejosh.com", 0},
    {"Seamark",   "read values off charts",    "seamark.heyitsmejosh.com", 0},
    {"Siftbox",   "inbox triage",              "siftbox.heyitsmejosh.com", 0},
    {"Curbfind",  "Craigslist browser",        "curbfind.heyitsmejosh.com", 0},
    {"Turing",    "local LLM",                 "", 0},
    {"Conveyer",  "AI plays Factorio",         "", 0},
};
#define PF_ROW_COUNT (int)(sizeof(PF_ROWS) / sizeof(PF_ROWS[0]))
#define PF_ROW_H 20

static int pf_sel; /* index into PF_ROWS, always a non-header row */
static int pf_scroll; /* first visible row */

static int pf_first_app_row(void) {
    for (int i = 0; i < PF_ROW_COUNT; i++) if (!PF_ROWS[i].is_header) return i;
    return 0;
}

static void pf_clamp_scroll(int vis_rows) {
    if (pf_sel < pf_scroll) pf_scroll = pf_sel;
    if (pf_sel >= pf_scroll + vis_rows) pf_scroll = pf_sel - vis_rows + 1;
    if (pf_scroll < 0) pf_scroll = 0;
    int max_scroll = PF_ROW_COUNT - vis_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (pf_scroll > max_scroll) pf_scroll = max_scroll;
}

/* Chrome (titlebar + legend) is drawn once by gui_launch_portfolio; this
   redraws only the scrollable list and the detail line, same split
   search_draw_content uses so scrolling/typing never flashes the whole
   window. */
static void pf_draw_content(void) {
    int w = (int)window_width(), h = (int)window_height();
    int list_top = 68, list_bot = h - 34;
    int list_h = list_bot - list_top;
    if (list_h < 0) list_h = 0;
    int vis_rows = list_h / PF_ROW_H;
    if (vis_rows < 1) vis_rows = 1;
    pf_clamp_scroll(vis_rows);

    window_rect(16, list_top, w - 32, list_h, GUI_BG);
    for (int r = 0; r < vis_rows; r++) {
        int i = pf_scroll + r;
        if (i >= PF_ROW_COUNT) break;
        int y = list_top + r * PF_ROW_H;
        const pf_row_t *row = &PF_ROWS[i];
        if (row->is_header) {
            font_draw_string(row->desc, 20, y + 2, 0x00807468, -1);
            continue;
        }
        if (i == pf_sel) window_rect(20, y - 2, w - 40, PF_ROW_H - 2, 0x00EDE6DC);
        font_draw_string(row->name, 32, y, 0x001C1C1E, -1);
        int nx = 32 + font_string_width(row->name) + 14;
        font_draw_string(row->desc, nx, y, 0x0075726E, -1);
    }

    /* Detail line: the URL fallback the task calls for since this kernel
       has no live "open a URL" hook to wire a selection into. */
    window_rect(16, h - 26, w - 32, 18, GUI_BG);
    const pf_row_t *sel = &PF_ROWS[pf_sel];
    if (sel->url[0]) font_draw_string(sel->url, 20, h - 24, 0x00234A78, -1);
    else font_draw_string("no web app", 20, h - 24, 0x00807468, -1);
}

static void gui_launch_portfolio(void) {
    pf_sel = pf_first_app_row();
    pf_scroll = 0;
    mouse_click_edge_sync();

    window_clear(GUI_BG);
    gui_draw_app_titlebar("Portfolio");
    font_draw_string("up/down or scroll to browse   esc closes", 20, 48, 0x0075726E, -1);

    for (;;) {
        pf_draw_content();
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;
        if (k == KEY_CLICK) {
            int list_top = 68, list_bot = (int)window_height() - 34;
            int list_h = list_bot - list_top;
            if (list_h < 0) list_h = 0;
            int vis_rows = list_h / PF_ROW_H;
            if (vis_rows < 1) vis_rows = 1;
            int click_vx = app_cursor_x - app_view_x, click_vy = app_cursor_y - app_view_y;
            int hit = -1;
            for (int r = 0; r < vis_rows; r++) {
                int i = pf_scroll + r;
                if (i >= PF_ROW_COUNT) break;
                if (PF_ROWS[i].is_header) continue;
                int y = list_top + r * PF_ROW_H;
                if (click_vx >= 20 && click_vx < (int)window_width() - 20 && click_vy >= y - 2 && click_vy < y + PF_ROW_H - 4) { hit = i; break; }
            }
            if (hit < 0) return;
            pf_sel = hit;
        } else if (k == KEY_UP || k == KEY_WHEEL_UP) {
            int i = pf_sel;
            while (--i >= 0) if (!PF_ROWS[i].is_header) { pf_sel = i; break; }
        } else if (k == KEY_DOWN || k == KEY_WHEEL_DOWN) {
            int i = pf_sel;
            while (++i < PF_ROW_COUNT) if (!PF_ROWS[i].is_header) { pf_sel = i; break; }
        }
        /* Enter is a no-op on purpose: no live browser hook to open into,
           the detail line already shows the URL for the selected row. */
    }
}
