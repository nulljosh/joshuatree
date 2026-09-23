/* Bookrank: Curated non-fiction book ranking, running natively instead of
   as an HTML card. Left: ranked list with selected row highlighted; up/down
   or click selects. Right: book title, author, and 2-3 sentence summary
   wrapped to fit. Same book list as bookrank.heyitsmejosh.com, compiled in.
   Included by kernel.c after render_wrapped_text. */
typedef struct {
    const char *title;
    const char *author;
    const char *summary;
} BrBook;

static const BrBook BR_BOOKS[] = {
    {"Thinking, Fast and Slow", "Daniel Kahneman", "Explores how our minds make decisions through fast intuitive thinking and slow deliberate reasoning. Reveals systematic biases and heuristics that shape human judgment."},
    {"Sapiens", "Yuval Noah Harari", "Chronicles humanity's rise from hunter-gatherers to modern civilization. Examines how myths and shared beliefs shaped society."},
    {"The Selfish Gene", "Richard Dawkins", "Proposes that genes, not organisms, are the primary units of evolution and self-interest. Challenges how we understand natural selection and behavior."},
    {"Educated", "Tara Westover", "Memoir of a woman who grew up in an isolated survivalist family with no formal education. Recounts her journey to escape and eventually earn a PhD."},
    {"Atomic Habits", "James Clear", "Breaks down habit formation into tiny incremental changes that compound over time. Practical framework for building better routines and breaking bad ones."},
    {"The Lean Startup", "Eric Ries", "Introduces rapid iteration and validated learning for building businesses efficiently. Challenges traditional business planning with a startup methodology."},
    {"Freakonomics", "Steven Levitt, Stephen Dubner", "Applies economic thinking to everyday life and hidden incentives. Reveals surprising connections between seemingly unrelated phenomena."},
    {"The Art of War", "Sun Tzu", "Ancient military treatise on strategy, tactics, and the nature of conflict. Principles apply to business, negotiation, and competition."},
    {"Grit", "Angela Duckworth", "Argues that passion and perseverance matter more than raw talent for success. Research shows sustained effort and resilience predict achievement."},
    {"The Structure of Scientific Revolutions", "Thomas Kuhn", "Explains how science progresses through paradigm shifts rather than linear accumulation. Challenges the idea that science simply discovers pre-existing truth."},
};
#define BR_COUNT ((int)(sizeof(BR_BOOKS) / sizeof(BR_BOOKS[0])))
#define BR_LIST_X  20
#define BR_LIST_W  140
#define BR_INFO_X  180
#define BR_INFO_W  420

static int br_sel = 0; /* currently selected book index */

static void br_draw(void){
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Bookrank");

    /* Left: ranked list */
    int list_top = 56, item_h = 28, max_items = (int)window_height() - list_top - 50;
    int items_shown = BR_COUNT < max_items / item_h ? BR_COUNT : max_items / item_h;

    for (int i = 0; i < items_shown; i++) {
        int y = list_top + i * item_h;
        unsigned int bg = (i == br_sel) ? 0x00FCCBDC : 0x00F1EDE7;
        unsigned int fg = (i == br_sel) ? 0x00C41E6C : 0x0075726E;
        window_rect(BR_LIST_X, y, BR_LIST_W, item_h - 2, bg);

        char rank[4];
        rank[0] = (char)('1' + i);
        rank[1] = '.';
        rank[2] = ' ';
        rank[3] = 0;
        font_draw_string(rank, BR_LIST_X + 6, y + 6, fg, -1);

        const char *title = BR_BOOKS[i].title;
        int title_len = 0;
        while (title[title_len] && title_len < 24) title_len++;
        char short_title[25];
        for (int j = 0; j < title_len; j++) short_title[j] = title[j];
        short_title[title_len] = 0;
        font_draw_string(short_title, BR_LIST_X + 28, y + 6, fg, -1);
    }

    /* Right: book info */
    if (br_sel < BR_COUNT) {
        int info_top = 56;
        const BrBook *b = &BR_BOOKS[br_sel];

        font_draw_string(b->title, BR_INFO_X, info_top, 0x001C1C1E, -1);
        font_draw_string(b->author, BR_INFO_X, info_top + 20, 0x0075726E, -1);

        int wrap_y = info_top + 44;
        render_wrapped_text(b->summary, BR_INFO_X, wrap_y, BR_INFO_W,
                           (int)window_height() - wrap_y - 50, 0x001C1C1E);
    }

    font_draw_string("up/down or click to select   esc closes", 20, (int)window_height() - 30, 0x0075726E, -1);
}

static void gui_launch_bookrank(void){
    br_sel = 0;
    mouse_click_edge_sync();
    for (;;) {
        br_draw();
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;

        if (k == KEY_UP && br_sel > 0) br_sel--;
        else if (k == KEY_DOWN && br_sel < BR_COUNT - 1) br_sel++;
        else if (k == KEY_CLICK) {
            int vx = app_cursor_x - app_view_x, vy = app_cursor_y - app_view_y;
            int item_h = 28, list_top = 56;
            if (vy >= list_top && vx >= BR_LIST_X && vx < BR_LIST_X + BR_LIST_W) {
                int sel = (vy - list_top) / item_h;
                if (sel < BR_COUNT) br_sel = sel;
                else return;
            } else return; /* titlebar X or off the app */
        }
    }
}
