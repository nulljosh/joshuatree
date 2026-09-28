/* Settings app UI: sidebar + grouped detail pane, modeled on macOS System
   Settings (System Settings.app's own left-hand section list plus a
   right-hand grouped-rows detail pane). Pulled out of kernel.c into its
   own file in the 1.7.x Settings redesign pass so kernel.c doesn't grow
   past its godfile-check.sh ceiling -- this whole screen used to live
   inline in kernel.c (a flat 8-row list, no sections, no sidebar); moving
   it here is a pure relocation of that logic plus the sidebar/grouping
   work, not a rewrite of what each row does. #included straight into
   kernel.c at the exact spot the inline version used to sit, so every
   symbol it touches (wind_enabled, dock_scale_pct, wall_theme, llm_*,
   auth_*, loc_*, settings_save/settings_load, window_*, font_draw_string,
   gui_draw_app_titlebar, get_key_or_click, ...) is already declared by
   the time this file is read, same as every other kernel.c #include. */

/* v47 (0.47.0): a real Settings screen, not a hidden shell command. Two
   rows, each a live toggle/stepper that writes through settings_save()
   immediately, the same "no separate Apply step" behaviour every setting
   in this kernel already has (fsuse, diskuse, wind). up/down picks a row,
   left/right (a/d, since there's no numpad here) changes it, a tap on a
   row also toggles/steps it, matching the touch-first contract every
   other screen in this GUI already keeps. */
/* v85: settings_prompt_line, the same shape contacts_prompt_line and
   mail_prompt_line already established (live-render, backspace, enter
   confirms, esc or a click cancels), pulled in here rather than shared
   across files since every app in this kernel keeps its own copy of this
   small loop already. Used to edit the two string LLM settings, since a
   toggle/stepper doesn't fit free text the way it fits wind/dock/wall.

   security pass: added a `masked` parameter. The password-change and
   add-user rows below used to call this with the typed password rendered
   in the clear on screen, the exact thing auth_field_input's dot-echo in
   auth.h was built to avoid for the login/first-run screens -- a real gap
   (shoulder-surfing, screen recording, the v86 landing demo) since this is
   the same secret, just entered through a different door. Masked draws a
   fixed-width dot per character, same convention, same length-not-content
   leak trade-off already accepted for login. */
static int settings_prompt_line(const char *prompt, char *out, int max, int masked) {
    unsigned int n = 0;
    while (out[n] && (int)n < max - 1) n++; /* start from the current value, not empty, so editing is a tweak not a retype */
    mouse_click_edge_sync();
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Settings");
        font_draw_string(prompt, 20, 52, 0x0075726E, -1);
        window_rect(20, 76, (int)window_width() - 40, 20, 0x00FFFFFF);
        out[n] = 0;
        if (masked) {
            char dots[AUTH_PASSWORD_MAX + 1];
            unsigned int dn = n; if (dn > AUTH_PASSWORD_MAX) dn = AUTH_PASSWORD_MAX;
            for (unsigned int i = 0; i < dn; i++) dots[i] = '*';
            dots[dn] = 0;
            font_draw_string(dots, 24, 78, 0x001C1C1E, -1);
        } else {
            font_draw_string(out, 24, 78, 0x001C1C1E, -1);
        }
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return 0;
        if (k == KEY_ENTER) break;
        if (k == '\b') { if (n > 0) n--; }
        else if ((int)n < max - 1 && k >= 32 && k < 127) out[n++] = (char)k;
    }
    out[n] = 0;
    return 1;
}

#define SETTINGS_ROW_COUNT 8 /* v75: + wallpaper source; v85: + LLM model, + LLM host:port; v0.77: + Account (change password), + Add user; v0.85.5: + Location */

/* 1.8: redesign modeled on macOS System Settings -- a left sidebar of
   sections (icon + label, selected row highlighted) and a right detail
   pane of grouped rows, instead of the old flat 8-row list. The 8 rows
   and their meaning (index 0..7, same order settings_save/settings_load
   and every `sel ==` branch below already key off) are unchanged; only
   how they're grouped and drawn is new, so this is a real UI pass, not a
   behavior rewrite -- every setting that worked before still works and
   still persists the same way. */
#define SETTINGS_SECTION_COUNT 3
static const char *SETTINGS_SECTION_NAMES[SETTINGS_SECTION_COUNT] = {"General", "Assistant", "Account"};
/* Row indices belonging to each section, in the order they're drawn top
   to bottom in the detail pane. -1 pads unused slots in the fixed-size
   array (plain C array, no VLAs in this freestanding build). */
static const int SETTINGS_SECTION_ROWS[SETTINGS_SECTION_COUNT][4] = {
    {0, 1, 2, 7},   /* General: Wind, Dock size, Wallpaper, Location */
    {3, 4, -1, -1}, /* Assistant: LLM model, LLM host:port */
    {5, 6, -1, -1}, /* Account: Account, Add user */
};
static int settings_section_row_count(int sec){
    int n = 0;
    while (n < 4 && SETTINGS_SECTION_ROWS[sec][n] >= 0) n++;
    return n;
}

#define SETTINGS_SIDEBAR_W 176
#define SETTINGS_DETAIL_X (SETTINGS_SIDEBAR_W + 28)
#define SETTINGS_DETAIL_Y0 92
#define SETTINGS_ROW_H 36
#define SETTINGS_SIDEBAR_Y0 60
#define SETTINGS_SIDEBAR_ROW_H 34
#define SETTINGS_TILE_S 22   /* sidebar icon tile side, logical px */
#define SETTINGS_SWITCH_W 40 /* boolean-row switch track width */
#define SETTINGS_SWITCH_H 22
/* Titlebar (traffic-light dots at y=20 r=6, title text baseline y=12,
   drawn by gui_draw_app_titlebar) occupies roughly the top 34px -- the
   sidebar must start below it, not at y=0, or it paints over the dots
   and title, a real bug caught in this pass's own before/after crops. */
#define SETTINGS_TITLEBAR_H 34

/* Pure, hardware/GUI-free: given a real click's full-screen logical
   coordinates, the window's current width, and which section is showing
   in the detail pane, returns the absolute row index (0..SETTINGS_ROW_COUNT-1)
   it lands in, or -1 if it misses every visible row's own highlight rect
   (the gui_rounded_rect_gradient(SETTINGS_DETAIL_X, y-7, ..., SETTINGS_ROW_H-4,
   ...) call drawn below -- the polish pass that added the grouped card
   also moved this rect's left edge onto SETTINGS_DETAIL_X itself, the
   same x every divider and every row's own text already use, so this
   hit-test's own left bound moved with it). Only the rows the current
   section actually draws can be hit -- a row belonging to a different,
   not-currently-shown section can never be clicked, same as a real
   grouped list. Extracted into its own function so this real hit-test
   math is unit-testable without a mouse or a boot, the same shape
   rtl8139_clamp_len's own extraction used for exactly this reason
   (v0.72.1: "so it's unit-testable without a NIC"). */
static int settings_row_at(int cx, int cy, int ww, int cur_section){
    if (cx < SETTINGS_DETAIL_X || cx >= ww - 20) return -1;
    int n = settings_section_row_count(cur_section);
    for (int p = 0; p < n; p++) {
        int ry = SETTINGS_DETAIL_Y0 + p * SETTINGS_ROW_H;
        if (cy >= ry - 7 && cy < ry - 7 + SETTINGS_ROW_H - 4) return SETTINGS_SECTION_ROWS[cur_section][p];
    }
    return -1;
}

/* Same shape for the sidebar's own 3 section rows: returns 0..SETTINGS_SECTION_COUNT-1
   or -1. Independent of window width -- the sidebar is a fixed-width
   column pinned to the left edge, same as System Settings' own. */
static int settings_sidebar_at(int cx, int cy){
    if (cx < 8 || cx >= SETTINGS_SIDEBAR_W - 8) return -1;
    for (int s = 0; s < SETTINGS_SECTION_COUNT; s++) {
        int ry = SETTINGS_SIDEBAR_Y0 + s * SETTINGS_SIDEBAR_ROW_H;
        if (cy >= ry - 6 && cy < ry - 6 + SETTINGS_SIDEBAR_ROW_H - 4) return s;
    }
    return -1;
}

/* Sidebar glyphs: real drawn primitives in a rounded-square tile, the
   same macOS System Settings convention (a colored rounded-square icon
   tile with a white glyph inside) instead of the old letter-in-circle
   ("G"/"A"/"U") this pass replaced -- house rule is no text in icons.
   Built from the exact same primitives every other icon in this file
   already uses (gui_rounded_rect_gradient for the flat tile,
   gui_fill_circle/gui_draw_capsule for the glyph itself), so these sit
   at one consistent style with the rest of the kernel's iconography, no
   new drawing primitive needed. Flat colors only, no gradients on the
   glyph itself (the tile fill call takes top==bottom on purpose) and no
   purple/teal in the palette. */
static void settings_tile_bg(int cx, int cy, int s, unsigned int color, unsigned int bg){
    gui_rounded_rect_gradient(cx - s / 2, cy - s / 2, s, s, color, color, bg, s / 4);
}
/* General: a gear. A ring hub (fill then punch a same-bg hole, the same
   donut technique gui_icon_search's own magnifying glass ring already
   uses) plus 8 capsule teeth around it, 4 axis-aligned and 4 diagonal --
   the same ray layout gui_icon_weather's sun already establishes for
   "spokes radiating from a circle", just short and blunt instead of long
   and thin so it reads as teeth, not sunbeams. */
static void settings_icon_gear(int cx, int cy, int s, unsigned int bg){
    settings_tile_bg(cx, cy, s, 0x005B8A72, bg);
    int hub_r = s / 5, tooth_r = s / 16, gap = hub_r + 1, tooth = s / 7;
    int diag = (tooth * 7) / 10; /* ~cos(45deg), same constant gui_icon_weather uses */
    gui_fill_circle(cx, cy, hub_r, 0x00FFFFFF, 0x005B8A72);
    gui_draw_capsule(cx, cy - gap,       cx, cy - gap - tooth,       tooth_r, 0x00FFFFFF, 0x005B8A72);
    gui_draw_capsule(cx, cy + gap,       cx, cy + gap + tooth,       tooth_r, 0x00FFFFFF, 0x005B8A72);
    gui_draw_capsule(cx - gap, cy,       cx - gap - tooth, cy,       tooth_r, 0x00FFFFFF, 0x005B8A72);
    gui_draw_capsule(cx + gap, cy,       cx + gap + tooth, cy,       tooth_r, 0x00FFFFFF, 0x005B8A72);
    gui_draw_capsule(cx - gap, cy - gap, cx - gap - diag, cy - gap - diag, tooth_r, 0x00FFFFFF, 0x005B8A72);
    gui_draw_capsule(cx + gap, cy - gap, cx + gap + diag, cy - gap - diag, tooth_r, 0x00FFFFFF, 0x005B8A72);
    gui_draw_capsule(cx - gap, cy + gap, cx - gap - diag, cy + gap + diag, tooth_r, 0x00FFFFFF, 0x005B8A72);
    gui_draw_capsule(cx + gap, cy + gap, cx + gap + diag, cy + gap + diag, tooth_r, 0x00FFFFFF, 0x005B8A72);
}
/* Assistant: a speech bubble with a sparkle dot, not the full green Messages
   bubble gui_icon_chat already owns elsewhere (that one's a dock/app icon
   with its own identity) -- a flat white bubble on the tile's own accent
   color keeps this glyph at the same one-color-ink style as the gear and
   person tiles either side of it. */
static void settings_icon_bubble(int cx, int cy, int s, unsigned int bg){
    settings_tile_bg(cx, cy, s, 0x00376E9E, bg);
    int w = (s * 6) / 10, h = (s * 5) / 10;
    int x = cx - w / 2, y = cy - h / 2 - s / 14;
    gui_rounded_rect_gradient(x, y, w, h, 0x00FFFFFF, 0x00FFFFFF, 0x00376E9E, s / 8);
    gui_fill_triangle_down(x + w / 4, y + h - 1, s / 10, s / 8, 0x00FFFFFF);
}
/* Account: a person silhouette, the same head-circle + shoulders-capsule
   shape gui_icon_contacts already establishes elsewhere in this file. */
static void settings_icon_person(int cx, int cy, int s, unsigned int bg){
    settings_tile_bg(cx, cy, s, 0x00A3703B, bg);
    int r = s / 6;
    gui_fill_circle(cx, cy - r, r, 0x00FFFFFF, 0x00A3703B);
    gui_draw_capsule(cx - r - r / 2, cy + r * 2, cx + r + r / 2, cy + r * 2, s / 9, 0x00FFFFFF, 0x00A3703B);
}

/* A real switch control -- rounded pill track plus a round knob -- for
   the one boolean row (Wind) instead of an "On"/"Off" text label. `x,y`
   is the track's own top-left; drawn right-aligned by the caller the
   same way every other value in this pane now is. Same on/off green
   already used for the wind row's old text ("On" in 0x002F7B4F), same
   neutral track gray gui_icon_keyrate's own keycaps already use for
   "not lit". Toggled by the row's existing click/tap and left/right (a/d)
   handling below -- this only changes what gets drawn, not how it's hit. */
static void settings_draw_switch(int x, int y, int on){
    unsigned int track = on ? 0x002F7B4F : 0x00C7C0B4;
    int r = SETTINGS_SWITCH_H / 2;
    gui_rounded_rect_gradient(x, y, SETTINGS_SWITCH_W, SETTINGS_SWITCH_H, track, track, 0x00F1EBE0, r);
    int knob_r = r - 3;
    int knob_cx = on ? x + SETTINGS_SWITCH_W - r : x + r;
    gui_fill_circle(knob_cx, y + r, knob_r, 0x00FFFFFF, track);
}

static void gui_launch_settings(void){
    int sel = 0;
    int cur_section = 0;
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Settings");
        int ww = (int)window_width(), wh = (int)window_height();

        /* Sidebar: three section rows, real icon tile + label, the
           selected section highlighted with a rounded rect (even 8px
           inset both sides) instead of the old square-cornered block.
           Icon tile and label are both centered on the row's own
           vertical middle -- the pre-polish bug had both sitting in the
           row's top half because the icon center (y+3) and the
           highlight's own center (y+9) never agreed; both now key off
           the same `mid` value. A thin vertical divider separates the
           sidebar from the detail pane, matching System Settings' own
           sidebar/detail split. */
        window_rect(0, SETTINGS_TITLEBAR_H, SETTINGS_SIDEBAR_W, wh - SETTINGS_TITLEBAR_H, 0x00F0EAE1);
        window_rect(SETTINGS_SIDEBAR_W - 1, SETTINGS_TITLEBAR_H, 1, wh - SETTINGS_TITLEBAR_H, 0x00DDD5C8);
        for (int s = 0; s < SETTINGS_SECTION_COUNT; s++) {
            int y = SETTINGS_SIDEBAR_Y0 + s * SETTINGS_SIDEBAR_ROW_H;
            int mid = y + 9; /* the highlight rect below is (y-6, h=30): its own true vertical center */
            if (s == cur_section) gui_rounded_rect_gradient(8, y - 6, SETTINGS_SIDEBAR_W - 16, SETTINGS_SIDEBAR_ROW_H - 4, 0x00E2D9C9, 0x00E2D9C9, 0x00F0EAE1, 8);
            unsigned int row_bg = s == cur_section ? 0x00E2D9C9 : 0x00F0EAE1;
            if (s == 0) settings_icon_gear(28, mid, SETTINGS_TILE_S, row_bg);
            else if (s == 1) settings_icon_bubble(28, mid, SETTINGS_TILE_S, row_bg);
            else settings_icon_person(28, mid, SETTINGS_TILE_S, row_bg);
            font_draw_string(SETTINGS_SECTION_NAMES[s], 46, mid - 5, 0x001C1C1E, -1);
        }

        /* Detail pane: only the current section's rows, grouped inside a
           rounded inset card (macOS System Settings' own grouped-list
           look) with a hairline divider between rows, section title
           above the card. Left edge of the card, the row highlight, the
           dividers and the row/value text all key off the one same
           SETTINGS_DETAIL_X constant now -- the pre-polish bug had the
           highlight starting 12px left of where the dividers and text
           themselves started. */
        font_draw_string(SETTINGS_SECTION_NAMES[cur_section], SETTINGS_DETAIL_X, 48, 0x001C1C1E, -1);
        int n_rows = settings_section_row_count(cur_section);
        int detail_right = ww - 20;
        int card_x = SETTINGS_DETAIL_X - 12, card_w = detail_right + 12 - card_x;
        int card_y = SETTINGS_DETAIL_Y0 - 16, card_h = n_rows * SETTINGS_ROW_H + 8;
        gui_rounded_rect_gradient(card_x, card_y, card_w, card_h, 0x00F1EBE0, 0x00F1EBE0, GUI_BG, 10);
        /* Single bottom-margin help line -- the old two-line footer's
           second line clipped at the window's own bottom edge; one line
           with real breathing room below it instead of two stacked to
           the last pixel. */
        font_draw_string("left/right or tap changes a row, esc closes. Saved automatically.", SETTINGS_DETAIL_X, wh - 22, 0x00807468, -1);

        for (int p = 0; p < n_rows; p++) {
            int i = SETTINGS_SECTION_ROWS[cur_section][p];
            int y = SETTINGS_DETAIL_Y0 + p * SETTINGS_ROW_H;
            if (i == sel) gui_rounded_rect_gradient(SETTINGS_DETAIL_X, y - 7, detail_right - SETTINGS_DETAIL_X, SETTINGS_ROW_H - 4, 0x00EDE6DC, 0x00EDE6DC, 0x00F1EBE0, 6);
            if (p > 0) window_rect(SETTINGS_DETAIL_X, y - 9, detail_right - SETTINGS_DETAIL_X, 1, 0x00E2DACB); /* hairline divider above every row but the first in the card */
            if (i == 0) {
                font_draw_string("Wind (swaying wallpaper)", SETTINGS_DETAIL_X, y, 0x001C1C1E, -1);
                /* A real switch control (rounded track + knob), not an
                   On/Off text label -- toggled the same way every other
                   row already is, by a click/tap or left/right (a/d). */
                settings_draw_switch(detail_right - SETTINGS_SWITCH_W, y - 2, wind_enabled);
            } else if (i == 1) {
                font_draw_string("Dock size", SETTINGS_DETAIL_X, y, 0x001C1C1E, -1);
                char sz[8]; int p = 0; int v = dock_scale_pct;
                if (v >= 10) sz[p++] = '0' + v / 10;
                sz[p++] = '0' + v % 10; sz[p++] = '%'; sz[p] = 0;
                font_draw_string(sz, detail_right - font_string_width(sz), y, 0x001C1C1E, -1);
            } else if (i == 2) {
                /* v75/v81: honest label. A map theme's name only shows once
                   a real tile mosaic is on screen; while it's still
                   fetching, or when the fetch failed and the photo is
                   what's actually up, say so instead of claiming a theme
                   that isn't really rendering. */
                font_draw_string("Wallpaper", SETTINGS_DETAIL_X, y, 0x001C1C1E, -1);
                const char *theme_name = wall_theme == WALL_COOL ? "Map (Cool)" : wall_theme == WALL_RAW ? "Map (Raw)" : wall_theme == WALL_SAT ? "Satellite" : (geo_city[0] ? geo_city : "Map (Warm)");
                /* v0.83.x: the v86 demo's own honest label. "photo until
                   then" stopped being true the moment the no-network
                   fallback became the baked satellite capture instead of
                   the tree -- font_is_fallback() is the same real v86
                   signal wall_apply() itself branches on. */
                const char *lbl = wall_theme == WALL_PHOTO ? "Photo" : (wall_map ? theme_name : (font_is_fallback() ? "Satellite (offline demo)" : "Map (fetching, photo until then)"));
                font_draw_string(lbl, detail_right - font_string_width(lbl), y, wall_theme != WALL_PHOTO && wall_map ? 0x002F7B4F : 0x001C1C1E, -1);
            } else if (i == 3) {
                font_draw_string("LLM model", SETTINGS_DETAIL_X, y, 0x001C1C1E, -1);
                font_draw_string(llm_model, detail_right - font_string_width(llm_model), y, 0x001C1C1E, -1);
            } else if (i == 4) {
                font_draw_string("LLM host:port", SETTINGS_DETAIL_X, y, 0x001C1C1E, -1);
                char hp[LLM_HOST_MAX + 8]; int p = 0;
                const char *s = llm_host; while (*s && p < (int)sizeof(hp) - 8) hp[p++] = *s++;
                hp[p++] = ':';
                char digits[8]; int nd = 0; int v = llm_port;
                if (v == 0) digits[nd++] = '0';
                while (v) { digits[nd++] = (char)('0' + v % 10); v /= 10; }
                while (nd) hp[p++] = digits[--nd];
                hp[p] = 0;
                font_draw_string(hp, detail_right - font_string_width(hp), y, 0x001C1C1E, -1);
            } else if (i == 5) {
                /* v0.77: real accounts. Tap/enter here walks old-password
                   ->new-password->confirm through settings_prompt_line
                   (masking not needed for that shared shell-style prompt,
                   the dedicated masked auth_field_input is only used by
                   the login/first-run screens themselves, kept separate on
                   purpose so Settings doesn't need its own copy of the
                   dot-echo loop for one row). */
                font_draw_string("Account", SETTINGS_DETAIL_X, y, 0x001C1C1E, -1);
                font_draw_string(auth_current_user[0] ? auth_current_user : "(none)", detail_right - font_string_width(auth_current_user[0] ? auth_current_user : "(none)"), y, 0x001C1C1E, -1);
            } else if (i == 6) {
                font_draw_string("Add user (new account)", SETTINGS_DETAIL_X, y, 0x001C1C1E, -1);
                font_draw_string("tap or enter", detail_right - font_string_width("tap or enter"), y, 0x00807468, -1);
            } else {
                /* v0.85.5: the Location field roadmap.md asked for. Empty
                   means "no override", the same honest-label convention
                   Wallpaper's own row just above already uses: say what's
                   actually in effect, not what was typed. */
                font_draw_string("Location", SETTINGS_DETAIL_X, y, 0x001C1C1E, -1);
                font_draw_string(loc_have ? loc_name : "(auto, from IP address)", detail_right - font_string_width(loc_have ? loc_name : "(auto, from IP address)"), y, loc_have ? 0x002F7B4F : 0x00807468, -1);
            }
        }

        window_present(); sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        /* Real left/right arrow keys step a row exactly like 'a'/'d' do --
           normalized right here so every branch below that already keys
           off 'a'/'d' (the wallpaper cycle, the LLM model cycle, dock
           size, and now the switch) gets real arrow-key support for
           free, not a second copy of the same dispatch logic. */
        if (k == KEY_LEFT) k = 'a';
        else if (k == KEY_RIGHT) k = 'd';
        if (k == KEY_ESC) return;
        /* Up/down moves within the current section's own rows only, same
           "arrow keys stay inside the visible group" behavior System
           Settings' own detail pane has -- switching sections is the
           sidebar's job (click a row there, or KEY_LEFT/KEY_RIGHT below),
           not something up/down should do by falling off the end of a
           group into an unrelated one. */
        if (k == KEY_UP || k == KEY_DOWN) {
            int pos = 0;
            for (int p = 0; p < n_rows; p++) if (SETTINGS_SECTION_ROWS[cur_section][p] == sel) { pos = p; break; }
            if (k == KEY_UP && pos > 0) pos--;
            else if (k == KEY_DOWN && pos < n_rows - 1) pos++;
            sel = SETTINGS_SECTION_ROWS[cur_section][pos];
        }
        else if (k == KEY_CLICK || k == 'a' || k == 'd') {
            /* A click on the sidebar switches sections (jumping selection
               to that section's first row) and never falls through to the
               detail pane's own toggle/stepper logic below -- clicking
               "Assistant" should show Assistant's rows, not also step
               whatever row happened to be selected in General. */
            if (k == KEY_CLICK) {
                mouse_get_absolute(&app_cursor_x, &app_cursor_y, ww, wh);
                int sidebar_hit = settings_sidebar_at(app_cursor_x, app_cursor_y);
                if (sidebar_hit >= 0) {
                    cur_section = sidebar_hit;
                    sel = SETTINGS_SECTION_ROWS[cur_section][0];
                    continue;
                }
            }
            /* A real click acts on whichever row it actually landed on, not
               whichever row a PRIOR arrow-key press happened to leave
               selected -- before this, a mouse/touch-only visitor with no
               keyboard (this kernel's own browser-demo idle tour included)
               could only ever toggle row 0 (Wind), since `sel` starts at 0
               and a bare click never moved it. Scoped to k==KEY_CLICK only:
               a real 'a'/'d' keypress must keep acting on whatever `sel`
               already is, not get silently overridden by a stale cursor
               position that has nothing to do with the keypress. */
            if (k == KEY_CLICK) {
                /* Real bug, found by tools/checks/auth-flow-check.py driving a real
                   synthetic pointer click (the exact gap walldemo-regression-check.py's
                   own comment already flagged as unconfirmed): app_cursor_x/y is only
                   kept live by gui_app_mouse_tick(), which is gated on gui_app_windowed
                   and therefore only ticks for apps opened through gui_launch_from_dock.
                   gui_launch_settings() is entered straight from the Apple menu
                   (gui_menu_run_item), never through that wrapper, so gui_app_windowed
                   stays 0 the whole time Settings is open and app_cursor_x/y is never
                   seeded or updated -- every click here hit-tested wherever the cursor
                   happened to be frozen at (0,0 if no windowed app had run yet this
                   boot), so settings_row_at() always missed and every click silently
                   fell through to acting on whatever `sel` already was, exactly the
                   pre-fix settingsclick bug this same block's own comment describes,
                   just reachable a different way than that fix covered. Query the real
                   position directly at the moment of the click instead of trusting the
                   stale global. */
                mouse_get_absolute(&app_cursor_x, &app_cursor_y, ww, wh);
                int hit = settings_row_at(app_cursor_x, app_cursor_y, ww, cur_section);
                if (hit >= 0) sel = hit;
            }
            if (sel == 0) { wind_enabled = !wind_enabled; settings_save(); }
            else if (sel == 2) {
                /* v81: cycles all four real themes (Photo -> Warm -> Cool
                   -> Raw -> Photo), not a binary toggle, matching the
                   left/right-steps contract dock size already uses below.
                   A tap (KEY_CLICK) always steps forward, same convention
                   dock size's tap already keeps. */
                int dir = (k == 'a') ? -1 : 1;
                wall_switch_theme((wall_theme + dir + 5) % 5);
                wall_apply(wall_theme != WALL_PHOTO);
            }
            else if (sel == 3) {
                /* v85 (direct feedback, after this landed): a free-text
                   model field can be typo'd to point at a model that
                   isn't actually installed on the host, silently failing
                   every chat. Real fix, checked against `ollama list` on
                   this machine rather than guessed: a bounded cycle over
                   real, known-working models, not free text.
                   nomic-embed-text is on the host too but is an
                   embedding-only model, not a chat model, deliberately
                   left off this list, the same "don't offer what wouldn't
                   work" call the wallpaper theme cycle already makes for
                   its own four real options. A live /api/tags probe
                   (Ollama's own model-list endpoint, same plain-HTTP shape
                   chat_send already uses) would be the more general fix
                   and is a real, scoped-out next step, not done here to
                   keep this pass's actual shipped surface honest about
                   what it covers.
                   1.0.12: "samantha" (Turing's model, the new default) is
                   now index 0 of LLM_MODELS; qwen3:8b/llama3.1:8b (the
                   local-Ollama-on-the-host alternatives) fill the other
                   two slots. `cur` is found by real lookup, not a
                   two-way strcmp against index 0 -- the old shape assumed
                   exactly two entries and silently treated anything past
                   index 0 as "the other one," which broke the moment a
                   third real model joined the list. */
                int cur = 0;
                for (int mi = 0; mi < LLM_MODEL_COUNT; mi++) if (!strcmp(llm_model, LLM_MODELS[mi])) { cur = mi; break; }
                int dir = (k == 'a') ? -1 : 1;
                int next = (cur + dir + LLM_MODEL_COUNT) % LLM_MODEL_COUNT;
                int p = 0; const char *m = LLM_MODELS[next];
                while (m[p] && p < LLM_MODEL_MAX - 1) { llm_model[p] = m[p]; p++; }
                llm_model[p] = 0;
                settings_save();
            }
            else if (sel == 4) {
                char hostbuf[LLM_HOST_MAX];
                int hn = 0; while (llm_host[hn] && hn < LLM_HOST_MAX - 1) { hostbuf[hn] = llm_host[hn]; hn++; }
                hostbuf[hn] = 0;
                if (settings_prompt_line("LLM host (hostname or IP, enter to confirm, esc to cancel):", hostbuf, LLM_HOST_MAX, 0)) {
                    int j = 0; while (hostbuf[j] && j < LLM_HOST_MAX - 1) { llm_host[j] = hostbuf[j]; j++; } llm_host[j] = 0;
                    char portbuf[8]; int pn = 0; int v = llm_port;
                    char digits[8]; int nd = 0;
                    if (v == 0) digits[nd++] = '0';
                    while (v) { digits[nd++] = (char)('0' + v % 10); v /= 10; }
                    while (nd) portbuf[pn++] = digits[--nd];
                    portbuf[pn] = 0;
                    if (settings_prompt_line("LLM port (enter to confirm, esc to cancel):", portbuf, sizeof(portbuf), 0)) {
                        int nv = 0; for (int c = 0; portbuf[c]; c++) if (portbuf[c] >= '0' && portbuf[c] <= '9') nv = nv * 10 + (portbuf[c] - '0');
                        if (nv > 0 && nv <= 65535) llm_port = nv;
                    }
                    settings_save();
                }
            }
            else if (sel == 5 && k != 'a' && k != 'd') {
                /* Change password for the account that's actually logged
                   in this session, not a free-text username field: there
                   is exactly one real "current user" concept in this
                   kernel today (auth_current_user, set by auth_gate at
                   boot), matching the single-machine/single-visitor
                   threat model docs/THREAT-MODEL.md lays out. 'a'/'d'
                   (left/right, the stepper convention every other row
                   uses) don't apply to this row, only a real tap/enter. */
                if (auth_current_user[0]) {
                    char oldbuf[AUTH_PASSWORD_MAX + 1]; oldbuf[0] = 0;
                    if (settings_prompt_line("Current password (enter to confirm, esc to cancel):", oldbuf, sizeof(oldbuf), 1)) {
                        char newbuf[AUTH_PASSWORD_MAX + 1]; newbuf[0] = 0;
                        if (settings_prompt_line("New password (enter to confirm, esc to cancel):", newbuf, sizeof(newbuf), 1)) {
                            char confirmbuf[AUTH_PASSWORD_MAX + 1]; confirmbuf[0] = 0;
                            if (settings_prompt_line("Confirm new password (enter to confirm, esc to cancel):", confirmbuf, sizeof(confirmbuf), 1)) {
                                int ok = !strcmp(newbuf, confirmbuf) && auth_change_password(auth_current_user, oldbuf, newbuf);
                                font_draw_string(ok ? "Password changed." : "That didn't work -- wrong current password or mismatch.",
                                                  20, (int)window_height() - 48, ok ? 0x002F7B4F : 0x00A33B3B, -1);
                                /* Same real bug tools/checks/auth-flow-check.py found in
                                   kernel/auth.h's login rejection: drawing lands in a back
                                   buffer and only window_present() ever flips it visible, and
                                   this status line had no frame boundary of its own before
                                   sleep_ticks -- the next redraw erased it unseen. */
                                window_present();
                                sleep_ticks(60);
                            }
                            memset(newbuf, 0, sizeof(newbuf));
                            memset(confirmbuf, 0, sizeof(confirmbuf));
                        }
                        memset(oldbuf, 0, sizeof(oldbuf));
                    }
                }
            }
            else if (sel == 6 && k != 'a' && k != 'd') {
                /* Adding a second local account. No admin/role concept
                   exists in this kernel (real, honest gap, not modeled
                   here since the direct request scoped this to "create a
                   user, change your own password", not a permissions
                   system) -- any logged-in session can add another
                   account. auth_create_user already refuses a duplicate
                   name, an empty name/password, or a full table (8 max). */
                char ubuf[AUTH_USERNAME_MAX + 1]; ubuf[0] = 0;
                if (settings_prompt_line("New username (enter to confirm, esc to cancel):", ubuf, sizeof(ubuf), 0)) {
                    char pbuf[AUTH_PASSWORD_MAX + 1]; pbuf[0] = 0;
                    if (settings_prompt_line("Password for that user (enter to confirm, esc to cancel):", pbuf, sizeof(pbuf), 1)) {
                        int ok = auth_create_user(ubuf, pbuf);
                        /* v0.77.1: the gate is opt-in (auth_gate is a no-op
                           on an unconfigured system, see kernel/auth.h),
                           so a session that reaches this row with nobody
                           logged in yet is exactly the "creating the very
                           first account" case that used to be the
                           first-run screen's job. Treat this account as
                           the current session's own from here on, the
                           same real effect the old first-run flow had,
                           just moved to Settings instead of gating boot. */
                        if (ok && !auth_current_user[0]) {
                            unsigned int p = 0; while (ubuf[p] && p < AUTH_USERNAME_MAX) { auth_current_user[p] = ubuf[p]; p++; } auth_current_user[p] = 0;
                            auth_logged_in = 1;
                        }
                        font_draw_string(ok ? "Account created." : "Couldn't create that account (name taken, empty, or table full).",
                                          20, (int)window_height() - 48, ok ? 0x002F7B4F : 0x00A33B3B, -1);
                        /* Same missing-present bug as the Change password status line
                           above and kernel/auth.h's login rejection: without this call
                           the message never reaches the visible framebuffer. */
                        window_present();
                        sleep_ticks(60);
                    }
                    memset(pbuf, 0, sizeof(pbuf));
                }
            }
            else if (sel == 7 && k != 'a' && k != 'd') {
                /* v0.85.5: Location, city or postal code, resolved through
                   Open-Meteo's own geocoding endpoint (loc_geocode above),
                   the same house the forecast itself already comes from.
                   Bounded the same way every other free-text Settings row
                   is: settings_prompt_line's max param (LOC_NAME_MAX,
                   matching geo_city's own bound). Empty input clears the
                   override and goes back to the IP lookup; a bad or
                   unknown location shows loc_geocode's own short error and
                   never panics or writes a fabricated coordinate. */
                char lbuf[LOC_NAME_MAX]; int li = 0; while (loc_name[li] && li < LOC_NAME_MAX - 1) { lbuf[li] = loc_name[li]; li++; } lbuf[li] = 0;
                if (settings_prompt_line("Location (city or postal code, enter to confirm, esc to cancel):", lbuf, sizeof(lbuf), 0)) {
                    if (!lbuf[0]) {
                        loc_have = 0; loc_name[0] = 0; loc_lat[0] = 0; loc_lon[0] = 0;
                        geo_have = 0; geo_lat[0] = 0; geo_lon[0] = 0; geo_city[0] = 0;
                        settings_save();
                        /* Same cache drop as the set path below: the old
                           override's weather and map must not outlive it. */
                        weather_tried_once = 0; weather_have = 0;
                        if (wall_map) { kfree(wall_map); wall_map = 0; wall_caches_drop(); }
                        wall_apply(wall_theme != WALL_PHOTO);
                        font_draw_string("Location cleared (using your IP address instead).", 20, (int)window_height() - 48, 0x00807468, -1);
                    } else if (loc_geocode(lbuf)) {
                        settings_save();
                        /* Drop whatever weather/map already have cached so
                           the desktop loop's own ten-minute cycle (the
                           same one that would normally re-check the IP
                           lookup) picks up the new coordinates on its very
                           next tick instead of waiting out the old cache,
                           through the exact same weather_fetch/wall_fetch
                           paths it already runs, nothing called directly
                           from here. */
                        weather_tried_once = 0; weather_have = 0;
                        if (wall_map) { kfree(wall_map); wall_map = 0; wall_caches_drop(); }
                        /* wall_src still pointed at the buffer just freed;
                           wall_apply repoints it (baked satellite or the
                           photo) until the refetch lands, the same way
                           wall_switch_theme is always followed by one. */
                        wall_apply(wall_theme != WALL_PHOTO);
                        char msg[48] = "Location set: "; int mp = 14; /* strlen("Location set: ") */
                        for (const char *c = loc_name; *c && mp < 47; c++) msg[mp++] = *c;
                        msg[mp] = 0;
                        font_draw_string(msg, 20, (int)window_height() - 48, 0x002F7B4F, -1);
                    } else {
                        font_draw_string(loc_err[0] ? loc_err : "Couldn't find that location.", 20, (int)window_height() - 48, 0x00A33B3B, -1);
                    }
                    window_present();
                    sleep_ticks(60);
                }
            }
            else if (sel != 5 && sel != 6 && sel != 7) {
                int dir = (k == 'a') ? -1 : 1; /* a tap always steps up; a real direction only from the keyboard */
                if (k == KEY_CLICK) dir = 1;
                int v = dock_scale_pct + dir;
                if (v > 25) v = 5; if (v < 5) v = 25; /* wraps, so a tap always does something visible */
                dock_scale_pct = v; settings_save();
            }
        }
    }
}
