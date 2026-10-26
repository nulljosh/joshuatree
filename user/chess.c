/* chess: Chess against the computer, as a ring-3 program (roadmap "Chess, fully playable in the OS").
 *
 * You are White. Move with the arrows and Enter (once on the piece, once on its square), a click on each, or by
 * typing the move, "e2e4" (a pawn that reaches the last rank becomes a queen). The rules and the opponent live in
 * user/chess_core.h, which tools/checks/chess-core-check.py builds on the host and counts against the standard
 * perft numbers. The opponent searches 2, 3 then 4 plies (with captures played out) and stops deepening once a
 * depth costs more than 40,000 positions, so it answers in about a second and the same game always goes the same way.
 * Checkmate, stalemate and bare kings end the game; n starts a new one, esc closes.
 *
 * Every move goes to the serial line ("chess: you e2e4", "chess: tree e7e5"), so tools/checks/ring3-chess-check.py
 * can replay the game through the same rules and know it was legal.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"
#define CH_DATA JT_DATA
#include "chess_core.h"

#define BG     0x00FAF8F6
#define LINE   0x00D9D3CA
#define LIGHT  0x00F1E6D6
#define DARK   0x00C29A74
#define LASTL  0x00E8C9A0
#define LASTD  0x00B98455
#define SEL    0x00B5502C
#define INK    0x001C1C1E
#define CREAM  0x00FAF8F4
#define EBONY  0x00302C29
#define HINT   0x0075726E
#define WIN    0x002F7B4F
#define TOP    40

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct ch_pos game JT_DATA;
static int sq JT_DATA = 0, bx JT_DATA = 0, by JT_DATA = 0;
static int cur JT_DATA = 12, sel JT_DATA = -1, last_from JT_DATA = -1, last_to JT_DATA = -1, over JT_DATA = 0, moves JT_DATA = 0;
static char typed[6] JT_DATA = {0}, said[48] JT_DATA = {0};

/* 14 by 14 silhouettes, P N B R Q K: drawn in blocks, each block outlined by a pixel. */
static const char *const art[6][14] = {
    {"..............", "......##......", ".....####.....", ".....####.....", "......##......", ".....####.....", "....######....",
     ".....####.....", ".....####.....", "....######....", "...########...", "..##########..", "..##########..", ".............."},
    {"..............", "......##......", ".....####.....", "....######....", "...#######....", "..########....", "..###.####....",
     "......####....", ".....#####....", "....######....", "...########...", "..##########..", "..##########..", ".............."},
    {"......##......", ".....####.....", "....###.##....", "....##.###....", "....######....", ".....####.....", "......##......",
     ".....####.....", ".....####.....", "....######....", "...########...", "..##########..", "..##########..", ".............."},
    {"..............", "..##..##..##..", "..##########..", "..##########..", "...########...", "....######....", "....######....",
     "....######....", "....######....", "...########...", "..##########..", "..##########..", "..............", ".............."},
    {".#....##....#.", ".#....##....#.", ".##..####..##.", ".###.####.###.", ".############.", "..##########..", "...########...",
     "....######....", ".....####.....", "....######....", "...########...", "..##########..", "..##########..", ".............."},
    {"......##......", "....######....", "....######....", "......##......", "..##..##..##..", "..##########..", "...########...",
     "....######....", ".....####.....", "....######....", "...########...", "..##########..", "..##########..", ".............."},
};

static void say(const char *s) { unsigned n = 0; while (s[n]) n++; jt_write(1, s, n); }
static void rect(int x, int y, int w, int h, unsigned c) { jt_fill(&win, x, y, w, h, c); }
static int text(const char *s, int x, int y, unsigned fg, int face) { return jt_text_draw(&win, face, x, y, fg, s); }
static void copy(char *d, const char *s) { while ((*d++ = *s++)) {} }

static void piece(int pc, int x, int y) {
    int u = sq / 16 > 0 ? sq / 16 : 1, ox = x + (sq - 14 * u) / 2, oy = y + (sq - 14 * u) / 2;
    const char *const *a = art[CH_TYPE(pc) - 1];
    for (int r = 0; r < 14; r++) for (int c = 0; c < 14; c++) if (a[r][c] == '#') rect(ox + c * u - 1, oy + r * u - 1, u + 2, u + 2, INK);
    for (int r = 0; r < 14; r++) for (int c = 0; c < 14; c++) if (a[r][c] == '#') rect(ox + c * u, oy + r * u, u, u, CH_SIDE(pc) ? EBONY : CREAM);
}
static void square(int i) {   /* a1 is bottom left: White plays up the screen */
    int f = i & 7, r = i >> 3, x = bx + f * sq, y = by + (7 - r) * sq, light = (f + r) & 1;
    int last = i == last_from || i == last_to;
    rect(x, y, sq, sq, light ? (last ? LASTL : LIGHT) : (last ? LASTD : DARK));
    if (game.b[i]) piece(game.b[i], x, y);
    if (i == sel) { rect(x, y, sq, 3, SEL); rect(x, y + sq - 3, sq, 3, SEL); rect(x, y, 3, sq, SEL); rect(x + sq - 3, y, 3, sq, SEL); }
    else if (i == cur && !over) { rect(x + 2, y + 2, sq - 4, 2, INK); rect(x + 2, y + sq - 4, sq - 4, 2, INK); rect(x + 2, y + 2, 2, sq - 4, INK); rect(x + sq - 4, y + 2, 2, sq - 4, INK); }
}
static void band(void) {
    rect(0, 0, (int)win.width, TOP, BG);
    rect(0, TOP - 1, (int)win.width, 1, LINE);
    int st = ch_status(&game);
    const char *msg = st == 1 ? (game.side ? "Checkmate. You win. n plays again." : "Checkmate. The computer wins. n plays again.")
                    : st == 2 ? "Stalemate, a draw. n plays again." : st == 3 ? "Only kings left, a draw. n plays again."
                    : typed[0] ? typed : ch_in_check(&game) ? "Check. Your move." : said[0] ? said : "Your move: arrows and Enter, a click, or type e2e4";
    int x = text(msg, 16, 12, st == 1 && game.side ? WIN : HINT, JT_FACE_BODY);
    if (!st && !typed[0] && !said[0]) text("   n new game   esc closes", x, 12, HINT, JT_FACE_BODY);
}
static void full(void) {
    sq = ((int)win.height - TOP - 32) / 8;
    if (((int)win.width - 48) / 8 < sq) sq = ((int)win.width - 48) / 8;
    if (sq < 16) sq = 16;
    bx = ((int)win.width - 8 * sq) / 2; by = TOP + 10;
    rect(0, TOP, (int)win.width, (int)win.height - TOP, BG);
    for (int i = 0; i < 64; i++) square(i);
    for (int k = 0; k < 8; k++) {
        char c[2] = {(char)('a' + k), 0}, r[2] = {(char)('8' - k), 0};
        text(c, bx + k * sq + sq / 2 - 4, by + 8 * sq + 4, HINT, JT_FACE_BODY);
        text(r, bx - 16, by + k * sq + sq / 2 - 8, HINT, JT_FACE_BODY);
    }
    band();
}
static void log_move(const char *who, unsigned short m) {
    char line[24] = "chess: ", nm[6]; int l = 7;
    ch_name(m, nm);
    for (const char *s = who; *s; s++) line[l++] = *s;
    line[l++] = ' ';
    for (const char *s = nm; *s; s++) line[l++] = *s;
    line[l++] = '\n'; line[l] = 0;
    say(line);
}
static void play(unsigned short m) {   /* m is legal for the side to move */
    struct ch_pos q;
    ch_make(&game, m, &q); game = q;
    last_from = CH_FROM(m); last_to = CH_TO(m);
    for (int i = 0; i < 64; i++) square(i);   /* castling and en passant move a third piece; the board is cheap to repaint */
}
static void finish(void) {
    int st = ch_status(&game);
    if (st) {
        over = 1;
        say(st == 1 ? (game.side ? "chess: checkmate, you win\n" : "chess: checkmate, tree wins\n") : st == 2 ? "chess: stalemate\n" : "chess: bare kings\n");
    }
    band();
}
static void reply(void) {   /* the computer's move, Black */
    copy(said, "Thinking..."); band();
    { struct jt_event ev; jt_window_poll(&ev, JT_POLL_PRESENT); }   /* show it before the search; a key pressed now is dropped */
    unsigned short m = 0;
    for (int d = 2; d <= 4; d++) { m = ch_best(&game, d); if (ch_nodes > 40000) break; }
    char nm[6]; ch_name(m, nm);
    copy(said, "The computer played "); { int l = 0; while (said[l]) l++; copy(said + l, nm); }
    log_move("tree", m);
    play(m);
}
static void try_move(int from, int to) {
    unsigned short *ms = ch_moves[CH_MAXPLY];
    int n = ch_legal(&game, ms);
    for (int i = 0; i < n; i++) {
        if (CH_FROM(ms[i]) != from || CH_TO(ms[i]) != to || (CH_PROMO(ms[i]) && CH_PROMO(ms[i]) != CH_Q)) continue;
        unsigned short m = ms[i];
        sel = -1; moves++;
        log_move("you", m);
        play(m);
        if (!ch_status(&game)) reply();
        finish();
        return;
    }
    copy(said, "That piece cannot go there."); band();
}
static void pick(int i) {   /* Enter or a click on square i */
    int was = sel;
    if (sel < 0 || (game.b[i] && !CH_SIDE(game.b[i]) && i != sel)) {
        sel = game.b[i] && !CH_SIDE(game.b[i]) ? i : -1;
        if (was >= 0) square(was);
        square(i); said[0] = 0; band();
        return;
    }
    if (i == sel) { sel = -1; square(i); return; }
    try_move(sel, i);
    if (was >= 0) square(was);
}
static void new_game(void) {
    ch_start(&game);
    cur = 12; sel = last_from = last_to = -1; over = moves = 0; typed[0] = said[0] = 0;
    full();
    say("chess: new game\n");
}
static void key_typed(int k) {   /* "e2e4" a letter or digit at a time */
    int l = 0; while (typed[l]) l++;
    int want_file = l == 0 || l == 2;
    if (want_file ? (k < 'a' || k > 'h') : (k < '1' || k > '8')) return;
    typed[l] = (char)k; typed[l + 1] = 0;
    if (l == 3) {
        int from = (typed[1] - '1') * 8 + (typed[0] - 'a'), to = (typed[3] - '1') * 8 + (typed[2] - 'a');
        typed[0] = 0;
        if (sel >= 0) { int s = sel; sel = -1; square(s); }
        try_move(from, to);
    } else band();
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open_hires(&win) != 0 || !win.pixels) { say("chess: no window\n"); jt_exit(1); }
    say("chess: ring-3 window\n");
    new_game();
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { full(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_KEY) {
                int k = ev.a, was = cur;
                if (k == '`') { say("chess: crashing on purpose\n"); *(volatile unsigned *)0 = 1; }
                if (k == JT_KEY_ESC) break;
                if (k == 'n') { new_game(); flags = JT_POLL_PRESENT; continue; }
                if (over) { flags = JT_POLL_PRESENT; continue; }
                if (k == JT_KEY_LEFT && (cur & 7) > 0) cur--;
                else if (k == JT_KEY_RIGHT && (cur & 7) < 7) cur++;
                else if (k == JT_KEY_UP && cur < 56) cur += 8;
                else if (k == JT_KEY_DOWN && cur > 7) cur -= 8;
                else if (k == JT_KEY_ENTER || k == '\n' || k == ' ') pick(cur);
                else if (k == 8 && typed[0]) { typed[0] = 0; band(); }   /* backspace clears a half-typed move */
                else key_typed(k);
                if (cur != was) { square(was); square(cur); }
            } else if (ev.kind == JT_EV_CLICK && !over && sq) {
                int f = (ev.a - bx) / sq, r = 7 - (ev.b - by) / sq;
                if (ev.a >= bx && ev.b >= by && f < 8 && r >= 0) { int was = cur; cur = r * 8 + f; square(was); pick(cur); }
            }
            flags = JT_POLL_PRESENT;
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        jt_sched_yield();
    }
    say("chess: closed\n");
    jt_exit(0);
}
