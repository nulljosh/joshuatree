/* Fieldbook: Every field of science and math explained plainly, running
   natively instead of as an HTML card. Left: ranked list with selected row
   highlighted; up/down or click selects. Right: field name, studies line,
   and 2-3 sentence explanation wrapped to fit. Same field list as
   fieldbook.heyitsmejosh.com, compiled in. Included by kernel.c after
   render_wrapped_text. */
typedef struct {
    const char *name;
    const char *studies;
    const char *explanation;
} FbField;

static const FbField FB_FIELDS[] = {
    {"Physics", "studies matter, energy, and forces", "Examines how objects move and interact through gravity, electromagnetism, and other fundamental forces. Builds the foundation for understanding everything from atoms to galaxies."},
    {"Chemistry", "studies atoms, bonds, and reactions", "Explores how elements combine into molecules and how they transform through reactions. Explains why materials behave as they do and how new substances form."},
    {"Biology", "studies living organisms and systems", "Investigates how cells, organisms, and ecosystems work from the molecular level to entire populations. Explains how life adapts, reproduces, and evolves."},
    {"Astronomy", "studies stars, planets, and galaxies", "Examines the structure of the universe, the life cycles of stars, and the physics of cosmic phenomena. Reveals our place in an expanding cosmos of billions of galaxies."},
    {"Geology", "studies rocks, minerals, and Earth's structure", "Investigates the composition and history of the planet, from surface rocks to the molten core. Explains how continents drift, mountains form, and the Earth evolves over deep time."},
    {"Ecology", "studies organisms and their environments", "Examines how species interact with each other and their surroundings through food webs and nutrient cycles. Shows how energy flows through nature and ecosystems maintain balance."},
    {"Neuroscience", "studies the brain and nervous system", "Investigates how neurons communicate, how signals travel through the brain, and how thought and sensation arise. Bridges biology and psychology by studying the physical basis of mind."},
    {"Computer Science", "studies computation and algorithms", "Explores how problems can be solved by step-by-step logical procedures and how to make those procedures efficient. Underlies all digital technology, from phones to the internet."},
    {"Statistics", "studies data, uncertainty, and probability", "Teaches how to extract meaning from data, estimate unknown quantities, and understand variability. Essential for science, medicine, economics, and any field dealing with real-world uncertainty."},
    {"Calculus", "studies rates of change and accumulation", "Analyzes how quantities change continuously and how to compute areas under curves. Powers physics, engineering, and economics by handling change mathematically."},
    {"Topology", "studies shapes, surfaces, and continuity", "Examines properties that stay the same when shapes are stretched or bent without tearing. Discovers surprising connections between different geometric objects."},
    {"Number Theory", "studies integers and prime numbers", "Investigates the structure of whole numbers, divisibility, and patterns hidden in sequences. Provides the mathematical foundation for cryptography and computer security."},
};
#define FB_COUNT ((int)(sizeof(FB_FIELDS) / sizeof(FB_FIELDS[0])))
#define FB_LIST_X  20
#define FB_LIST_W  220
#define FB_INFO_X  260
#define FB_INFO_W  ((int)window_width() - FB_INFO_X - 24)

static int fb_sel = 0; /* currently selected field index */

static void fb_draw(void){
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Fieldbook");

    /* Left: ranked list */
    int list_top = 56, item_h = 28, max_items = (int)window_height() - list_top - 50;
    int items_shown = FB_COUNT < max_items / item_h ? FB_COUNT : max_items / item_h;

    for (int i = 0; i < items_shown; i++) {
        int y = list_top + i * item_h;
        unsigned int bg = (i == fb_sel) ? 0x00E2D8CC : 0x00F1EDE7;
        unsigned int fg = (i == fb_sel) ? 0x001C1C1E : 0x0075726E;
        window_rect(FB_LIST_X, y, FB_LIST_W, item_h - 2, bg);

        char rank[4]; int r = 0, n = i + 1;
        if (n >= 10) rank[r++] = (char)('0' + n / 10);
        rank[r++] = (char)('0' + n % 10);
        rank[r++] = '.'; rank[r] = 0;
        font_draw_string(rank, FB_LIST_X + 6, y + 6, fg, -1);

        /* Trim by real pixel width, not character count, so a long title
           ends in "..." inside its row instead of running into the summary. */
        const char *name = FB_FIELDS[i].name;
        char short_name[52]; int len = 0;
        while (name[len] && len < 47) { short_name[len] = name[len]; len++; }
        short_name[len] = 0;
        int room = FB_LIST_W - 36;
        if (font_string_width(short_name) > room) {
            while (len > 0 && (short_name[len] = 0, font_string_width(short_name) + font_string_width("...") > room)) len--;
            while (len > 0 && short_name[len - 1] == ' ') len--;
            short_name[len] = '.'; short_name[len + 1] = '.'; short_name[len + 2] = '.'; short_name[len + 3] = 0;
        }
        font_draw_string(short_name, FB_LIST_X + 30, y + 6, fg, -1);
    }

    /* Right: field info */
    if (fb_sel < FB_COUNT) {
        int info_top = 56;
        const FbField *f = &FB_FIELDS[fb_sel];

        font_draw_string(f->name, FB_INFO_X, info_top, 0x001C1C1E, -1);
        font_draw_string(f->studies, FB_INFO_X, info_top + 20, 0x0075726E, -1);

        int wrap_y = info_top + 44;
        render_wrapped_text(f->explanation, FB_INFO_X, wrap_y, FB_INFO_W,
                           (int)window_height() - wrap_y - 50, 0x001C1C1E);
    }

    font_draw_string("up/down or click to select   esc closes", 20, (int)window_height() - 30, 0x0075726E, -1);
}

static void gui_launch_fieldbook(void){
    fb_sel = 0;
    mouse_click_edge_sync();
    for (;;) {
        fb_draw();
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;

        if (k == KEY_UP && fb_sel > 0) fb_sel--;
        else if (k == KEY_DOWN && fb_sel < FB_COUNT - 1) fb_sel++;
        else if (k == KEY_CLICK) {
            int vx = app_cursor_x - app_view_x, vy = app_cursor_y - app_view_y;
            int item_h = 28, list_top = 56;
            if (vy >= list_top && vx >= FB_LIST_X && vx < FB_LIST_X + FB_LIST_W) {
                int sel = (vy - list_top) / item_h;
                if (sel < FB_COUNT) fb_sel = sel;
                else return;
            } else return; /* titlebar X or off the app */
        }
    }
}
