/* Sparkjar: A jar of ideas people post and vote on. Left: ranked list
   sorted by votes, selected row highlighted; up/down or click selects.
   Right: idea name, one-line pitch, and 3-step build plan wrapped to fit.
   Pressing 'u' upvotes the selected idea and re-sorts the list. Vote
   counts are session-only (reset when app closes). Included by kernel.c
   after render_wrapped_text. */
typedef struct {
    const char *name;
    const char *pitch;
    const char *plan;
} SjIdea;

static const SjIdea SJ_IDEAS[] = {
    {"Weather Dashboard", "Display temperature, wind, precipitation for your area", "1. Fetch Open-Meteo data. 2. Parse hourly forecast. 3. Draw graph widget."},
    {"Timer App", "Simple task timer with audio alerts and session logging", "1. Build countdown UI. 2. Emit beep on complete. 3. Track history per session."},
    {"Calculator with Memory", "Basic calculator with M+ M- MR buttons", "1. Parse infix expressions. 2. Add memory stack. 3. Wire buttons to operations."},
    {"Password Generator", "Create memorable and strong passwords", "1. Pattern templates (CVC, leet). 2. Entropy slider. 3. Clipboard copy."},
    {"QR Code Scanner", "Read codes from camera or file", "1. Barcode library integration. 2. Camera capture. 3. Deep link routing."},
    {"Mini Pomodoro", "25 minute focus timer with 5 minute breaks", "1. Strict timing loop. 2. Desktop notifications. 3. Session stats."},
    {"Markdown Preview", "Live HTML rendering from markdown text", "1. Marked.js parser. 2. CSS reset template. 3. Syntax highlight code blocks."},
    {"Expense Logger", "Quick spend log with categories and tags", "1. Local storage DB. 2. Filter by date/tag. 3. CSV export."},
    {"Habit Tracker", "Daily checklist with streak count", "1. IDB for persistence. 2. Calendar view. 3. Fire streak notifications."},
    {"Dice Roller", "RPG-style dice with history and export", "1. Parse xdy notation. 2. Fair randomness. 3. Tape/export results."},
};
#define SJ_COUNT ((int)(sizeof(SJ_IDEAS) / sizeof(SJ_IDEAS[0])))
#define SJ_LIST_X  20
#define SJ_LIST_W  220
#define SJ_INFO_X  260
#define SJ_INFO_W  ((int)window_width() - SJ_INFO_X - 24)

/* Vote counts, mutable in this session. Initialize all to 0, then upvote
   counts are stored in the original idea order, not the sorted order. */
static int sj_votes[SJ_COUNT] = {0};

/* Sorted indices: sj_order[0] is the index of the top-voted idea,
   sj_order[1] is the next, etc. */
static int sj_order[SJ_COUNT];
static int sj_sel = 0; /* currently selected position in sj_order */

static int sj_idea_at(int pos) {
    return sj_order[pos];
}

static void sj_init_order(void) {
    for (int i = 0; i < SJ_COUNT; i++) {
        sj_order[i] = i;
    }
}

/* Bubble sort by vote count descending, keeping sj_sel on the same idea */
static void sj_sort_by_votes(void) {
    int sel_idea = sj_order[sj_sel];
    for (int i = 0; i < SJ_COUNT - 1; i++) {
        for (int j = i + 1; j < SJ_COUNT; j++) {
            if (sj_votes[sj_order[i]] < sj_votes[sj_order[j]]) {
                int tmp = sj_order[i];
                sj_order[i] = sj_order[j];
                sj_order[j] = tmp;
            }
        }
    }
    for (int i = 0; i < SJ_COUNT; i++) {
        if (sj_order[i] == sel_idea) {
            sj_sel = i;
            break;
        }
    }
}

static void sj_draw(void) {
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Sparkjar");

    /* Left: ranked list sorted by votes */
    int list_top = 56, item_h = 28, max_items = (int)window_height() - list_top - 50;
    int items_shown = SJ_COUNT < max_items / item_h ? SJ_COUNT : max_items / item_h;

    for (int i = 0; i < items_shown; i++) {
        int y = list_top + i * item_h;
        int idea_idx = sj_order[i];
        unsigned int bg = (i == sj_sel) ? 0x00E2D8CC : 0x00F1EDE7;
        unsigned int fg = (i == sj_sel) ? 0x001C1C1E : 0x0075726E;
        window_rect(SJ_LIST_X, y, SJ_LIST_W, item_h - 2, bg);

        char rank[4]; int r = 0, n = i + 1;
        if (n >= 10) rank[r++] = (char)('0' + n / 10);
        rank[r++] = (char)('0' + n % 10);
        rank[r++] = '.'; rank[r] = 0;
        font_draw_string(rank, SJ_LIST_X + 6, y + 6, fg, -1);

        /* Trim title by real pixel width */
        const char *name = SJ_IDEAS[idea_idx].name;
        char short_name[52]; int len = 0;
        while (name[len] && len < 47) { short_name[len] = name[len]; len++; }
        short_name[len] = 0;
        int room = SJ_LIST_W - 70; /* leave room for vote count on right */
        if (font_string_width(short_name) > room) {
            while (len > 0 && (short_name[len] = 0, font_string_width(short_name) + font_string_width("...") > room)) len--;
            while (len > 0 && short_name[len - 1] == ' ') len--;
            short_name[len] = '.'; short_name[len + 1] = '.'; short_name[len + 2] = '.'; short_name[len + 3] = 0;
        }
        font_draw_string(short_name, SJ_LIST_X + 30, y + 6, fg, -1);

        /* Vote count, right-aligned in row */
        char vote_str[6]; int v = 0;
        int votes = sj_votes[idea_idx];
        if (votes >= 10) vote_str[v++] = (char)('0' + votes / 10);
        vote_str[v++] = (char)('0' + votes % 10);
        vote_str[v] = 0;
        int vote_w = font_string_width(vote_str);
        font_draw_string(vote_str, SJ_LIST_X + SJ_LIST_W - vote_w - 8, y + 6, fg, -1);
    }

    /* Right: idea detail */
    if (sj_sel < SJ_COUNT) {
        int info_top = 56;
        int idea_idx = sj_order[sj_sel];
        const SjIdea *idea = &SJ_IDEAS[idea_idx];

        font_draw_string(idea->name, SJ_INFO_X, info_top, 0x001C1C1E, -1);
        font_draw_string(idea->pitch, SJ_INFO_X, info_top + 20, 0x0075726E, -1);

        int wrap_y = info_top + 44;
        render_wrapped_text(idea->plan, SJ_INFO_X, wrap_y, SJ_INFO_W,
                           (int)window_height() - wrap_y - 50, 0x001C1C1E);
    }

    font_draw_string("up/down or click to select   u upvotes   esc closes", 20, (int)window_height() - 30, 0x0075726E, -1);
}

static void gui_launch_sparkjar(void) {
    sj_init_order();
    sj_sel = 0;
    mouse_click_edge_sync();
    for (;;) {
        sj_draw();
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;

        if (k == KEY_UP && sj_sel > 0) sj_sel--;
        else if (k == KEY_DOWN && sj_sel < SJ_COUNT - 1) sj_sel++;
        else if (k == 'u' || k == 'U') {
            int idea_idx = sj_order[sj_sel];
            sj_votes[idea_idx]++;
            sj_sort_by_votes();
        }
        else if (k == KEY_CLICK) {
            int vx = app_cursor_x - app_view_x, vy = app_cursor_y - app_view_y;
            int item_h = 28, list_top = 56;
            if (vy >= list_top && vx >= SJ_LIST_X && vx < SJ_LIST_X + SJ_LIST_W) {
                int sel = (vy - list_top) / item_h;
                if (sel < SJ_COUNT) sj_sel = sel;
                else return;
            } else return; /* titlebar X or off the app */
        }
    }
}
