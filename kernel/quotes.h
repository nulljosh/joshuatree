/* Quotes: Quotestreak running natively. A line from a film, four titles,
   pick the right one and the streak grows; miss and it resets. Same game as
   quotestreak.heyitsmejosh.com, replacing the one-line HTML card the dock
   tile used to open. Included by kernel.c after wx_put_int (streak counter).
   ponytail: a fixed table of well-known lines, answer order rotated per
   round instead of shuffled. Add rows here to grow the deck. */
typedef struct { const char *line; const char *film; } QsQuote;
static const QsQuote QS_DECK[] = {
    {"I'm going to make him an offer he can't refuse.", "The Godfather"},
    {"May the Force be with you.", "Star Wars"},
    {"You talking to me?", "Taxi Driver"},
    {"Here's looking at you, kid.", "Casablanca"},
    {"I'll be back.", "The Terminator"},
    {"You can't handle the truth!", "A Few Good Men"},
    {"Why so serious?", "The Dark Knight"},
    {"Life is like a box of chocolates.", "Forrest Gump"},
    {"I see dead people.", "The Sixth Sense"},
    {"Houston, we have a problem.", "Apollo 13"},
    {"There's no place like home.", "The Wizard of Oz"},
    {"To infinity and beyond!", "Toy Story"},
    {"Roads? Where we're going we don't need roads.", "Back to the Future"},
    {"Sell me this pen.", "The Wolf of Wall Street"},
    {"The first rule of Fight Club is: you do not talk about Fight Club.", "Fight Club"},
    {"Just keep swimming.", "Finding Nemo"},
};
#define QS_COUNT ((int)(sizeof(QS_DECK) / sizeof(QS_DECK[0])))
#define QS_OPT_Y0 150
#define QS_OPT_H  34

static int qs_round, qs_streak, qs_best, qs_pick; /* qs_pick: -1 unanswered, else the option chosen */
static int qs_cur(void){ return (qs_round * 7) % QS_COUNT; } /* 7 is coprime with 16, so every line comes up once per lap */
/* Option slot -> deck index. The right answer sits in slot (round % 4); the
   other three are the next deck rows that are not the answer. */
static int qs_option(int slot){
    int right = qs_round % 4, cur = qs_cur();
    if (slot == right) return cur;
    int n = slot < right ? slot : slot - 1;
    return (cur + 3 + n * 5) % QS_COUNT == cur ? (cur + 1) % QS_COUNT : (cur + 3 + n * 5) % QS_COUNT;
}
static void qs_draw(void){
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Quotes");
    font_draw_string("Which film is this from?", 20, 56, 0x0075726E, -1);
    font_draw_string(QS_DECK[qs_cur()].line, 20, 96, 0x001C1C1E, -1);
    int right = qs_round % 4;
    for (int s = 0; s < 4; s++) {
        int y = QS_OPT_Y0 + s * QS_OPT_H;
        unsigned int bg = 0x00F1EDE7;
        if (qs_pick >= 0 && s == right) bg = 0x00CFE8D2;          /* the answer, shown once you have picked */
        else if (qs_pick == s) bg = 0x00F0D0CC;                    /* your miss */
        window_rect(20, y, (int)window_width() - 40, QS_OPT_H - 6, bg);
        char lab[4] = {(char)('1' + s), ' ', ' ', 0};
        font_draw_string(lab, 30, y + 6, 0x0075726E, -1);
        font_draw_string(QS_DECK[qs_option(s)].film, 56, y + 6, 0x001C1C1E, -1);
    }
    char line[96]; char *o = line; const char *t;
    for (t = "streak "; *t; t++) *o++ = *t;
    o = wx_put_int(o, qs_streak);
    for (t = "   best "; *t; t++) *o++ = *t;
    o = wx_put_int(o, qs_best);
    for (t = qs_pick < 0 ? "   1-4 or click to answer   esc closes" : (qs_pick == right ? "   right. any key for the next one" : "   missed. any key for the next one"); *t; t++) *o++ = *t;
    *o = 0;
    font_draw_string(line, 20, (int)window_height() - 30, 0x0075726E, -1);
}
static void gui_launch_quotes(void){
    qs_pick = -1;
    mouse_click_edge_sync();
    for (;;) {
        qs_draw();
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;
        int slot = -1;
        if (k == KEY_CLICK) {
            int vx = app_cursor_x - app_view_x, vy = app_cursor_y - app_view_y;
            if (vy >= QS_OPT_Y0 && vy < QS_OPT_Y0 + 4 * QS_OPT_H && vx >= 20 && vx < (int)window_width() - 20) slot = (vy - QS_OPT_Y0) / QS_OPT_H;
            else return; /* the titlebar X, or anywhere off the answers, same as every other app */
        } else if (k >= '1' && k <= '4') slot = k - '1';
        if (qs_pick >= 0) { qs_pick = -1; qs_round++; continue; } /* any input after an answer moves on */
        if (slot < 0) continue;
        qs_pick = slot;
        if (slot == qs_round % 4) { qs_streak++; if (qs_streak > qs_best) qs_best = qs_streak; }
        else qs_streak = 0;
    }
}
