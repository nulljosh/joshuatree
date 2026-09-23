/* Plan: Joshua's ten-year education and career roadmap, running
   natively instead of as an HTML card. Left: milestone years with selected row
   highlighted; up/down or click selects. Right: timeline heading and detailed
   explanation wrapped to fit. Same structure as fieldbook. Included by kernel.c
   after render_wrapped_text. */
typedef struct {
    const char *when;
    const char *what;
} PlanMilestone;

static const PlanMilestone PLAN_MILESTONES[] = {
    {"2026 to 27", "Pre-Calculus 12 online through LECSS. The one admission gap."},
    {"2027 to 31", "SFU Computing Science BSc, Burnaby. Calc and physics in year 1, Beedie Business minor declared end of year 2, co-op terms."},
    {"2031 to 33", "Work as a developer, ideally fintech or trading infrastructure. Decide on the MSc Finance."},
    {"2033 to 36", "Engineering: an SFU bridge or a professional master's, aiming at P.Eng eligibility."},
    {"Late 30s", "Optional finance major or MSc Finance. Done before 40."},
};
#define PLAN_COUNT ((int)(sizeof(PLAN_MILESTONES) / sizeof(PLAN_MILESTONES[0])))
#define PLAN_LIST_X  20
#define PLAN_LIST_W  220
#define PLAN_INFO_X  260
#define PLAN_INFO_W  ((int)window_width() - PLAN_INFO_X - 24)

static int plan_sel = 0; /* currently selected milestone index */

static void plan_draw(void){
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Plan");

    /* Left: ranked list */
    int list_top = 56, item_h = 28, max_items = (int)window_height() - list_top - 50;
    int items_shown = PLAN_COUNT < max_items / item_h ? PLAN_COUNT : max_items / item_h;

    for (int i = 0; i < items_shown; i++) {
        int y = list_top + i * item_h;
        unsigned int bg = (i == plan_sel) ? 0x00E2D8CC : 0x00F1EDE7;
        unsigned int fg = (i == plan_sel) ? 0x001C1C1E : 0x0075726E;
        window_rect(PLAN_LIST_X, y, PLAN_LIST_W, item_h - 2, bg);

        /* Trim by real pixel width, not character count, so a long title
           ends in "..." inside its row instead of running into the summary. */
        const char *when = PLAN_MILESTONES[i].when;
        char short_when[52]; int len = 0;
        while (when[len] && len < 47) { short_when[len] = when[len]; len++; }
        short_when[len] = 0;
        int room = PLAN_LIST_W - 12;
        if (font_string_width(short_when) > room) {
            while (len > 0 && (short_when[len] = 0, font_string_width(short_when) + font_string_width("...") > room)) len--;
            while (len > 0 && short_when[len - 1] == ' ') len--;
            short_when[len] = '.'; short_when[len + 1] = '.'; short_when[len + 2] = '.'; short_when[len + 3] = 0;
        }
        font_draw_string(short_when, PLAN_LIST_X + 6, y + 6, fg, -1);
    }

    /* Right: milestone info */
    if (plan_sel < PLAN_COUNT) {
        int info_top = 56;
        const PlanMilestone *m = &PLAN_MILESTONES[plan_sel];

        font_draw_string("The next ten years", PLAN_INFO_X, info_top, 0x0075726E, -1);

        int wrap_y = info_top + 24;
        render_wrapped_text(m->what, PLAN_INFO_X, wrap_y, PLAN_INFO_W,
                           (int)window_height() - wrap_y - 50, 0x001C1C1E);
    }

    font_draw_string("up/down or click to select   esc closes", 20, (int)window_height() - 30, 0x0075726E, -1);
}

static void gui_launch_plan(void){
    plan_sel = 0;
    mouse_click_edge_sync();
    for (;;) {
        plan_draw();
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;

        if (k == KEY_UP && plan_sel > 0) plan_sel--;
        else if (k == KEY_DOWN && plan_sel < PLAN_COUNT - 1) plan_sel++;
        else if (k == KEY_CLICK) {
            int vx = app_cursor_x - app_view_x, vy = app_cursor_y - app_view_y;
            int item_h = 28, list_top = 56;
            if (vy >= list_top && vx >= PLAN_LIST_X && vx < PLAN_LIST_X + PLAN_LIST_W) {
                int sel = (vy - list_top) / item_h;
                if (sel < PLAN_COUNT) plan_sel = sel;
                else return;
            } else return; /* titlebar X or off the app */
        }
    }
}
