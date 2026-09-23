/* Lexly: Spanish vocabulary flashcard drill, running natively. A Spanish
   word, four English choices, pick the right one and the streak grows; miss
   and it resets. Replaces the one-line HTML card the dock tile used to open.
   Included by kernel.c after wx_put_int (streak counter). */
typedef struct { const char *spanish; const char *english; } LxWord;
static const LxWord LX_DECK[] = {
    {"agua", "water"},
    {"libro", "book"},
    {"casa", "house"},
    {"gato", "cat"},
    {"perro", "dog"},
    {"sol", "sun"},
    {"luna", "moon"},
    {"estrella", "star"},
    {"mesa", "table"},
    {"puerta", "door"},
    {"ventana", "window"},
    {"arbol", "tree"},
    {"flor", "flower"},
    {"pan", "bread"},
    {"queso", "cheese"},
    {"leche", "milk"},
    {"carne", "meat"},
    {"pollo", "chicken"},
    {"pescado", "fish"},
    {"vino", "wine"},
    {"cerveza", "beer"},
    {"cafe", "coffee"},
    {"te", "tea"},
    {"azucar", "sugar"},
    {"sal", "salt"},
    {"pimienta", "pepper"},
    {"mantequilla", "butter"},
    {"aceite", "oil"},
    {"vinagre", "vinegar"},
    {"manzana", "apple"},
};
#define LX_COUNT ((int)(sizeof(LX_DECK) / sizeof(LX_DECK[0])))
#define LX_OPT_Y0 150
#define LX_OPT_H  34

static int lx_round, lx_streak, lx_best, lx_pick; /* lx_pick: -1 unanswered, else the option chosen */
static int lx_cur(void){ return (lx_round * 7) % LX_COUNT; } /* 7 is coprime with 30, so every word comes up once per lap */
/* Option slot -> deck index. The right answer sits in slot (round % 4); the
   other three are the next deck rows that are not the answer. */
static int lx_option(int slot){
    int right = (lx_round * 3 + lx_cur()) % 4, cur = lx_cur();
    if (slot == right) return cur;
    int n = slot < right ? slot : slot - 1;
    return (cur + 3 + n * 5) % LX_COUNT == cur ? (cur + 1) % LX_COUNT : (cur + 3 + n * 5) % LX_COUNT;
}
static void lx_draw(void){
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Lexly");
    font_draw_string("What is this Spanish word?", 20, 56, 0x0075726E, -1);
    font_draw_string(LX_DECK[lx_cur()].spanish, 20, 96, 0x001C1C1E, -1);
    int right = (lx_round * 3 + lx_cur()) % 4;
    for (int s = 0; s < 4; s++) {
        int y = LX_OPT_Y0 + s * LX_OPT_H;
        unsigned int bg = 0x00F1EDE7;
        if (lx_pick >= 0 && s == right) bg = 0x00CFE8D2;          /* the answer, shown once you have picked */
        else if (lx_pick == s) bg = 0x00F0D0CC;                    /* your miss */
        window_rect(20, y, (int)window_width() - 40, LX_OPT_H - 6, bg);
        char lab[4] = {(char)('1' + s), ' ', ' ', 0};
        font_draw_string(lab, 30, y + 6, 0x0075726E, -1);
        font_draw_string(LX_DECK[lx_option(s)].english, 56, y + 6, 0x001C1C1E, -1);
    }
    char line[96]; char *o = line; const char *t;
    for (t = "streak "; *t; t++) *o++ = *t;
    o = wx_put_int(o, lx_streak);
    for (t = "   best "; *t; t++) *o++ = *t;
    o = wx_put_int(o, lx_best);
    for (t = lx_pick < 0 ? "   1-4 or click to answer   esc closes" : (lx_pick == right ? "   correct. any key for next" : "   incorrect. any key for next"); *t; t++) *o++ = *t;
    *o = 0;
    font_draw_string(line, 20, (int)window_height() - 30, 0x0075726E, -1);
}
static void gui_launch_lexly(void){
    lx_pick = -1;
    mouse_click_edge_sync();
    for (;;) {
        lx_draw();
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;
        int slot = -1;
        if (k == KEY_CLICK) {
            int vx = app_cursor_x - app_view_x, vy = app_cursor_y - app_view_y;
            if (vy >= LX_OPT_Y0 && vy < LX_OPT_Y0 + 4 * LX_OPT_H && vx >= 20 && vx < (int)window_width() - 20) slot = (vy - LX_OPT_Y0) / LX_OPT_H;
            else return; /* the titlebar X, or anywhere off the answers, same as every other app */
        } else if (k >= '1' && k <= '4') slot = k - '1';
        if (lx_pick >= 0) { lx_pick = -1; lx_round++; continue; } /* any input after an answer moves on */
        if (slot < 0) continue;
        lx_pick = slot;
        if (slot == lx_round % 4) { lx_streak++; if (lx_streak > lx_best) lx_best = lx_streak; }
        else lx_streak = 0;
    }
}
