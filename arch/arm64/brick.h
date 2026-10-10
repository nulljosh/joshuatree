/* Original brick-breaker rules; fixed-size playfield, no allocator or hardware. */
#define BRICK_W 456
#define BRICK_H 240
#define BRICK_PW 76
#define BRICK_PY 222
#define BRICK_R 4
struct brick_game {
    int paddle, x, y, dx, dy, lives, score, state, paused; /* state: serve, play, won, lost */
    unsigned bricks;
};
static void brick_move(struct brick_game *g, int x) {
    g->paddle = x < BRICK_PW / 2 ? BRICK_PW / 2 : x > BRICK_W - BRICK_PW / 2 ? BRICK_W - BRICK_PW / 2 : x;
    if (!g->state) g->x = g->paddle;
}
static void brick_serve(struct brick_game *g) {
    g->state = 0; g->x = g->paddle; g->y = BRICK_PY - BRICK_R - 2; g->dx = 2; g->dy = -3;
}
static void brick_reset(struct brick_game *g) {
    g->paddle = BRICK_W / 2; g->lives = 3; g->score = g->paused = 0; g->bricks = 0xFFFFFFFFu;
    brick_serve(g);
}
static void brick_launch(struct brick_game *g) {
    if (g->state >= 2) brick_reset(g);
    if (!g->state) g->state = 1;
    else g->paused = !g->paused;
}
static void brick_step(struct brick_game *g) {
    if (g->state != 1 || g->paused) return;
    int ox = g->x, oy = g->y;
    g->x += g->dx; g->y += g->dy;
    if (g->x < BRICK_R) { g->x = 2 * BRICK_R - g->x; g->dx = -g->dx; }
    if (g->x > BRICK_W - BRICK_R) { g->x = 2 * (BRICK_W - BRICK_R) - g->x; g->dx = -g->dx; }
    if (g->y < BRICK_R) { g->y = 2 * BRICK_R - g->y; g->dy = -g->dy; }
    if (g->dy > 0 && oy + BRICK_R <= BRICK_PY && g->y + BRICK_R >= BRICK_PY
        && g->x + BRICK_R >= g->paddle - BRICK_PW / 2 && g->x - BRICK_R <= g->paddle + BRICK_PW / 2) {
        g->y = BRICK_PY - BRICK_R; g->dy = -g->dy;
        g->dx = (g->x - g->paddle) / 10;
        if (!g->dx) g->dx = ox < g->paddle ? -1 : 1;
        if (g->dx < -4) g->dx = -4;
        if (g->dx > 4) g->dx = 4;
    }
    for (unsigned i = 0; i < 32; i++) {
        int bx = 12 + (int)(i % 8) * 54, by = 16 + (int)(i / 8) * 24;
        if ((g->bricks & (1u << i)) && g->x + BRICK_R >= bx && g->x - BRICK_R < bx + 48
            && g->y + BRICK_R >= by && g->y - BRICK_R < by + 16) {
            g->bricks &= ~(1u << i); g->score++;
            if (oy + BRICK_R <= by || oy - BRICK_R >= by + 16) { g->dy = -g->dy; g->y = oy; }
            else { g->dx = -g->dx; g->x = ox; }
            if (!g->bricks) g->state = 2;
            break;
        }
    }
    if (g->y - BRICK_R > BRICK_H) {
        if (--g->lives) brick_serve(g);
        else g->state = 3;
    }
}
