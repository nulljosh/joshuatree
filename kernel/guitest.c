/* The desktop's drawing self-tests (cursortest, texttest, wraptest, dockstyletest; gfxtest, fonttest and mousetest since 4.20.3), each a shell
   command a headless check types and reads the serial log of. Moved out of kernel.c's run() in 4.20.2. */
#include "libc.h"
#include "serial.h"
#include "console.h"
#include "window.h"
#include "font.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "dock_geom.h"
#include "dock_draw.h"
#include "mouse.h"
#include "settings.h"
#include "settings_ui.h"
#include "desktop.h"
#include "selftest.h"

/* v77: texttest mirrors every line to serial so tools/textspacing-check.sh can read the verdict headless, the pngtest pattern. */
static void tt_out(const char *s){ puts(s); serial_puts(s); }
static void tt_num(int n){ char b[12]; int i = 0; if (n < 0) { tt_out("-"); n = -n; } if (n == 0) b[i++] = '0'; while (n) { b[i++] = (char)('0' + n % 10); n /= 10; } b[i] = 0; for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; } tt_out(b); }

int guitest_run(const char *line, char *arg) {
    (void)arg;
    if (!strcmp(line, "cursortest")) {
        /* v56.1 gap fix: the notif-panel bitmap-font regression (cursor
           save/restore reading/writing antialiased text through the
           LOGICAL layer, pixel-doubling it) was reproduced and fixed via
           a scripted headless sweep + pmemsave that pass, then fully
           reverted, nothing permanent left to catch the same class of bug
           coming back on any other never-repainted AA surface. Real,
           discriminating: renders real antialiased glyphs (font_draw_string
           genuinely blends at physical resolution) at a scale-2 window,
           captures the true per-physical-pixel content independently
           (window_get_pixel_phys, not through cursor_backup itself),
           then runs the real gui_cursor_save/gui_draw_cursor/
           gui_cursor_restore sequence over it and confirms every physical
           sub-pixel came back byte-exact. The old logical-layer bug would
           sample only each block's top-left pixel and write it back
           across the whole 2x2 block, so any block with real per-pixel
           variation (the entire point of antialiasing) would fail this. */
        if (!window_open_scaled(400, 300, 32, 2)) { puts("no VGA device found or out of page tables\n"); }
        else {
            int mx = 40, my = 40;
            font_set_aa(gui_aa_char, gui_aa_advance); /* real physical-resolution AA text, the exact surface v56.1 fixed; gui_run() normally registers this but this test doesn't call gui_run() */
            window_clear(0x00202020);
            font_draw_string("Wg", mx, my, 0x00F5F5F7, -1);

            int sc = gui_cursor_scale(), pw = CURSOR_W * sc, ph = CURSOR_H * sc;
            int px0 = mx * sc, py0 = my * sc;
            static unsigned int truth[CURSOR_W * CURSOR_MAX_SCALE * CURSOR_H * CURSOR_MAX_SCALE];
            int varied = 0;
            for (int j = 0; j < ph; j++) {
                for (int i = 0; i < pw; i++) {
                    truth[j * pw + i] = window_get_pixel_phys(px0 + i, py0 + j);
                    if (i > 0 && truth[j * pw + i] != truth[j * pw + i - 1]) varied = 1; /* real AA content, not a flat fill */
                }
            }
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
            gui_cursor_restore();
            int mismatches = 0;
            for (int j = 0; j < ph; j++)
                for (int i = 0; i < pw; i++)
                    if (window_get_pixel_phys(px0 + i, py0 + j) != truth[j * pw + i]) mismatches++;
            puts(varied ? "cursor test area has real per-pixel AA variation: ok\n" : "cursor test area has NO variation (test not discriminating): FAILED\n");
            puts((varied && mismatches == 0) ? "cursor save/restore: AA text came back byte-exact through the physical layer: ok\n" : "cursor save/restore: FAILED (pixel-doubled or otherwise wrong)\n");
            window_close();
        }
    }
    else if (!strcmp(line, "texttest")) {
        /* v77 (0.67.1): "Cl oudy". Real, photographed menu bar bug: an
           extra gap between some letter pairs and not others. Root cause
           was the fixed 16px physical cell per glyph on the AA path
           (see font_draw_string's own note). Discriminating: renders the
           exact photographed word through the real font_draw_string +
           gui_aa_char path at scale 2, then measures where the ink
           actually landed, column by column, through window_get_pixel_phys.
           Six ink runs must come back (one per letter), and the spread
           between the widest and narrowest inter-letter gap must be tiny:
           with the bug the C-l gap is ~1 physical px (C overflows its cell
           and is clipped) while l-o is ~9 (l fills 7 of 16), spread 8;
           fixed, every gap is the glyphs own sidebearings, spread <= 3.
           Second check: a lone 'W' (advance 24) must be at least 20 ink
           columns wide; the bug clipped it to 16. */
        if (!window_open_scaled(400, 300, 32, 2)) { tt_out("no VGA device found or out of page tables\n"); }
        else {
            font_set_aa(gui_aa_char, gui_aa_advance);
            unsigned int bg = 0x00202020;
            window_clear(bg);
            font_draw_string("Cloudy", 20, 20, 0x00F5F5F7, -1);
            font_draw_string("W", 20, 60, 0x00F5F5F7, -1);
            int col_ink[200];
            for (int i = 0; i < 200; i++) {
                col_ink[i] = 0;
                for (int j = 0; j < 32; j++) if (window_get_pixel_phys(40 + i, 40 + j) != bg) { col_ink[i] = 1; break; }
            }
            int runs = 0, run_start[16], run_end[16], in_run = 0;
            for (int i = 0; i < 200; i++) {
                if (col_ink[i] && !in_run) { if (runs < 16) run_start[runs] = i; in_run = 1; }
                if (!col_ink[i] && in_run) { if (runs < 16) run_end[runs] = i - 1; runs++; in_run = 0; }
            }
            if (in_run) { if (runs < 16) run_end[runs] = 199; runs++; }
            int gap_min = 999, gap_max = -1;
            tt_out("texttest 'Cloudy' ink runs:");
            for (int r = 0; r < runs && r < 16; r++) {
                tt_out(" "); tt_num(run_start[r]); tt_out("-"); tt_num(run_end[r]);
                if (r > 0) { int gap = run_start[r] - run_end[r - 1] - 1; if (gap < gap_min) gap_min = gap; if (gap > gap_max) gap_max = gap; }
            }
            tt_out("\n");
            int spread = gap_max - gap_min;
            tt_out("texttest: letter gap spread "); tt_num(spread); tt_out(" px (min "); tt_num(gap_min); tt_out(", max "); tt_num(gap_max); tt_out(")\n");
            tt_out((runs == 6 && spread <= 3) ? "texttest 'Cloudy' spacing uniform: ok\n" : "texttest 'Cloudy' spacing uneven (the 'Cl oudy' bug): FAILED\n");
            int w_first = -1, w_last = -1;
            for (int i = 0; i < 64; i++) {
                int ink = 0;
                for (int j = 0; j < 32; j++) if (window_get_pixel_phys(40 + i, 120 + j) != bg) { ink = 1; break; }
                if (ink) { if (w_first < 0) w_first = i; w_last = i; }
            }
            int w_width = w_last - w_first + 1;
            tt_out("texttest: 'W' ink width "); tt_num(w_width); tt_out(" px\n");
            tt_out((w_width >= 20) ? "texttest 'W' not clipped: ok\n" : "texttest 'W' clipped by the fixed cell: FAILED\n");
            window_close();
        }
    }
    else if (!strcmp(line, "wraptest")) {
        /* v78 (0.67.2): real bug, "Curbf ind" on a real framebuffer dump of
           the Curbfind app view (gui_launch_html -> render_wrapped_text).
           v77 fixed font_draw_string's own AA pen but never touched this
           function, which drew every glyph through the single-glyph
           font_draw_char API at a hardcoded 8px advance, exactly v77's bug
           shape, just reachable only through the word-wrap path (every
           HTML app view, every Chat answer) instead of the four sites v77
           actually audited. Same discrimination shape as texttest: render
           a real word through the real render_wrapped_text path at scale
           2, measure ink runs column by column. Uneven gaps (spread > 3)
           or the wrong run count is the "Curbf ind" bug; both are supposed
           to disappear once render_wrapped_text uses font_string_width/
           font_draw_string per word instead of wlen*8/font_draw_char. */
        if (!window_open_scaled(400, 300, 32, 2)) { tt_out("no VGA device found or out of page tables\n"); }
        else {
            font_set_aa(gui_aa_char, gui_aa_advance);
            unsigned int bg = 0x00202020;
            window_clear(bg);
            render_wrapped_text("Curbfind", 20, 20, 360, 40, 0x00F5F5F7);
            int col_ink[200];
            for (int i = 0; i < 200; i++) {
                col_ink[i] = 0;
                for (int j = 0; j < 32; j++) if (window_get_pixel_phys(40 + i, 40 + j) != bg) { col_ink[i] = 1; break; }
            }
            int runs = 0, run_start[16], run_end[16], in_run = 0;
            for (int i = 0; i < 200; i++) {
                if (col_ink[i] && !in_run) { if (runs < 16) run_start[runs] = i; in_run = 1; }
                if (!col_ink[i] && in_run) { if (runs < 16) run_end[runs] = i - 1; runs++; in_run = 0; }
            }
            if (in_run) { if (runs < 16) run_end[runs] = 199; runs++; }
            int gap_min = 999, gap_max = -1;
            tt_out("wraptest 'Curbfind' ink runs:");
            for (int r = 0; r < runs && r < 16; r++) {
                tt_out(" "); tt_num(run_start[r]); tt_out("-"); tt_num(run_end[r]);
                if (r > 0) { int gap = run_start[r] - run_end[r - 1] - 1; if (gap < gap_min) gap_min = gap; if (gap > gap_max) gap_max = gap; }
            }
            tt_out("\n");
            int spread = gap_max - gap_min;
            tt_out("wraptest: letter gap spread "); tt_num(spread); tt_out(" px (min "); tt_num(gap_min); tt_out(", max "); tt_num(gap_max); tt_out(")\n");
            /* "Curbfind" is 8 letters, every pair of adjacent glyphs touches or
               nearly touches at this face (no wide space-shaped sidebearing
               gap like 'l'/'i' produce), so the whole word draws as one ink
               run when spacing is correct; the fixed-cell bug splits it into
               multiple runs with an uneven spread instead. */
            tt_out((runs == 1 || (runs > 1 && spread <= 3)) ? "wraptest 'Curbfind' spacing uniform: ok\n" : "wraptest 'Curbfind' spacing uneven (the 'Curbf ind' bug): FAILED\n");
            window_close();
        }
    }
    else if (!strcmp(line, "dockstyletest")) {
        /* v58/v61 gap fix: both passes verified against a real, one-off
           framebuffer pixel dump, reverted before commit, nothing
           permanent left in the suite (dockhover-check.py, added in v63,
           only covers the hover-magnify bug, a different defect
           entirely). Two real, discriminating checks against the exact
           functions those passes fixed, called directly with real
           geometry rather than needing a QEMU pixel-dump script: (1)
           gui_rounded_rect_on_wallpaper's straight top edge (v61): before
           the fix every non-corner edge pixel was a 100% solid `col =
           color` with zero blend, so the very first row of the rect was
           already the exact fill color; after, the first `band` (3)
           physical rows blend toward the real backdrop first. (2)
           gui_draw_icon_shadow (v58): before the fix it drew through the
           LOGICAL layer at a quarter the physical resolution, so every
           physical pixel pair came out identical (blocky); after, it
           draws physical-resolution with real per-pixel falloff.

           v72 gap fix to check (1), root-caused with a real DEBUG serial
           dump of the actual channel values, not guessed: this check used
           to also assert `row0 != bg_before`, comparing row0 (the very
           top physical row of the tray, py=0) against a real rendered
           wallpaper pixel sampled 2 physical rows further up. At py=0,
           `t = band - py` is exactly `band`, so gui_lerp's own math (t ==
           max) returns its second argument outright: row0 IS, by design,
           100% the real backdrop color at that exact pixel
           (gui_wallpaper_sample(px0+px, py0+py, 0)), zero fill color
           mixed in yet -- the softest point of the AA fringe, fading
           fully into the wallpaper before solidifying by row `band`. A
           real dump confirmed both are the honest, working blend, not a
           regression: bg_before=0x170b06, row0=0x170b06 (equal, because
           the wallpaper gradient barely moves over 2 physical rows at
           this exact spot -- expected, not a bug), row1=0x5f5650 (a real
           partial blend, matches gui_lerp(TRAY, 0x170b06, 2, 3) exactly),
           row3=TRAY (full fill, past the band). Asserting `row0 !=
           bg_before` was backwards: a correct edge blend is SUPPOSED to
           read close to the true backdrop right at its outermost row, so
           demanding it differ from a nearby real backdrop sample fails
           exactly when the fix is working, wherever the gradient happens
           to be locally flat. The two assertions that actually encode the
           v61 regression (a raw, unblended top row) are `row0 !=
           DOCK_TRAY_COLOR` and `row1 != row0`: reverting v61's fix (col =
           color for every non-corner edge pixel, no blend) makes row0
           AND row1 both equal DOCK_TRAY_COLOR outright, failing both;
           restoring the fix, they pass. Dropped the incorrect third
           condition, kept the two that really discriminate. */
        if (!window_open_scaled(960, 540, 32, 2)) { puts("no VGA device found or out of page tables\n"); }
        else {
            gui_draw_wallpaper_rows(0, (int)window_height());
            int y0 = gui_dock_y0(), dock_x = gui_dock_x0(), dock_w = gui_dock_w(), dock_h = DOCK_ICON + 2 * DOCK_PAD;
            int sc = (int)window_scale();
            int sample_x = (dock_x + dock_w / 2) * sc; /* dock centre: far from either rounded corner */
            gui_rounded_rect_on_wallpaper(dock_x, y0, dock_w, dock_h, DOCK_TRAY_COLOR, 20);
            unsigned int row0 = window_get_pixel_phys(sample_x, y0 * sc + 0);
            unsigned int row1 = window_get_pixel_phys(sample_x, y0 * sc + 1);
            unsigned int row3 = window_get_pixel_phys(sample_x, y0 * sc + 3); /* one row past the v61 band (3 physical rows) */
            int seam_ok = row0 != DOCK_TRAY_COLOR && row1 != row0 && row3 == DOCK_TRAY_COLOR;
            puts(seam_ok ? "dock tray top edge: real multi-row blend, not a 1-row solid cut: ok\n" : "dock tray top edge: FAILED (single-row seam)\n");

            window_rect(0, 0, (int)window_width(), (int)window_height(), DOCK_TRAY_COLOR);
            int cx = (int)window_width() / 2, cy = (int)window_height() / 2;
            gui_draw_icon_shadow(cx, cy, DOCK_ICON);
            int ry = (DOCK_ICON * sc) / 9; /* the ellipse's own physical half-height, see gui_draw_icon_shadow */
            int px = cx * sc; /* vertical centreline: dy sweeps distance^2 fast, the axis that best exposes ry's real division headroom (8 physical steps) vs the old ry=4 logical-block version's coarser ~4-5 */
            unsigned int seen[16]; int nseen = 0;
            for (int dy = -ry; dy < ry; dy++) {
                unsigned int v = window_get_pixel_phys(px, cy * sc - sc + dy);
                int found = 0; for (int s = 0; s < nseen; s++) if (seen[s] == v) { found = 1; break; }
                if (!found && nseen < 16) seen[nseen++] = v;
            }
            /* The pre-v58 bug drew this ellipse through the LOGICAL layer
               at ry=4 (real division headroom halved) with each logical
               row blown up into a 2-physical-row block, so a vertical
               sweep only ever showed ~5 distinct levels, each repeated in
               pairs; the real fixed version (ry=8, physical resolution)
               shows close to its own ry+1 distinct levels. */
            int shadow_ok = nseen >= 7;
            puts(shadow_ok ? "dock icon shadow: real per-physical-pixel falloff, not blocky: ok\n" : "dock icon shadow: FAILED (blocky/coarse)\n");
            window_close();
        }
    }
    else if (!strcmp(line, "gfxtest")) {
        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            window_rect(0, 0, 800, 200, 0x00555555);
            window_rect(0, 200, 800, 200, 0x007A2048);
            window_rect(0, 400, 800, 200, 0x00FAF8F6);
            get_key(); /* leave the picture up until a key is pressed */
            window_close();
            clear();
            puts("back in text mode\n");
        }
    }
    else if (!strcmp(line, "fonttest")) {
        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            window_clear(0x00FAF8F6);
            font_draw_string("Joshua Tree", 20, 20, 0x00555555, -1);
            font_draw_string("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 20, 60, 0x001C1C1E, -1);
            font_draw_string("abcdefghijklmnopqrstuvwxyz", 20, 80, 0x001C1C1E, -1);
            font_draw_string("0123456789 !?.,:;()", 20, 100, 0x001C1C1E, -1);
            font_draw_string("the quick brown fox jumps", 20, 140, 0x007A2048, -1);
            get_key();
            window_close();
            clear();
            puts("back in text mode\n");
        }
    }
    else if (!strcmp(line, "mousetest")) {
        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            int cx_pos = 400, cy_pos = 300;
            int buttons = 0;
            do {
                window_clear(0x00FAF8F6);
                window_rect(cx_pos - 5, cy_pos - 5, 10, 10, 0x00555555);
                window_present(); __asm__ volatile ("hlt"); /* wake on the next IRQ (timer, keyboard, or mouse) */
                int dx, dy;
                if (mouse_get_delta(&dx, &dy, &buttons)) {
                    cx_pos += dx; cy_pos += dy;
                    mouse_get_absolute(&cx_pos, &cy_pos, (int)window_width(), (int)window_height());
                    if (cx_pos < 0) cx_pos = 0; if ((unsigned)cx_pos >= window_width())  cx_pos = window_width() - 1;
                    if (cy_pos < 0) cy_pos = 0; if ((unsigned)cy_pos >= window_height()) cy_pos = window_height() - 1;
                }
            } while (!(buttons & 1)); /* left click to exit */
            window_close();
            clear();
            puts("back in text mode\n");
        }
    }
    else if (!strcmp(line, "settingsclicktest")) {
        /* Regression test for a real bug: Settings' own click handler used
           to act on whatever row a PRIOR arrow-key press had left `sel`
           on (starting at row 0, Wind), completely ignoring where the
           click itself actually landed -- a mouse/touch-only visitor with
           no keyboard (this kernel's own browser-demo idle tour included)
           could therefore only ever toggle row 0, since a bare click never
           moved `sel`. settings_row_at is the exact pure hit-test
           gui_launch_settings' click branch now calls before touching
           `sel`; no mouse, GUI, or boot state needed to exercise it. */
        /* 1.8: extended for the sidebar+detail-pane redesign. settings_row_at
           now takes which section is showing (a row belonging to a
           different, not-currently-visible section can never be hit) and
           settings_sidebar_at is the same shape for the new sidebar's own
           3 section rows. Also covers the real "does a setting survive a
           reboot" contract settings_save/settings_load promise -- Settings'
           own footer text says so, this proves it round-trips through the
           same save/load path a real reboot uses, not just that the in-
           memory toggle flips. */
        int ok = 1;
        if (settings_row_at(300, 92,  960, 0) != 0) { puts("settingsclick: General row 0 (Wind) center missed\n"); ok = 0; }
        /* 1.8.2 polish pass: Wind's value is now a real switch control,
           right-aligned near the row's own right edge instead of an
           "On"/"Off" text label near the middle -- prove that region of
           the row (where the switch itself actually is, detail_right(940)
           - SETTINGS_SWITCH_W(40) = 900, plus a few px margin) still
           resolves to row 0 like the rest of the row always has, same
           "click anywhere on the row toggles it" contract every row here
           keeps, not just the switch's own bounding box. */
        if (settings_row_at(910, 92,  960, 0) != 0) { puts("settingsclick: General row 0's switch region missed\n"); ok = 0; }
        if (settings_row_at(300, 164, 960, 0) != 2) { puts("settingsclick: General row (Wallpaper) center missed\n"); ok = 0; }
        if (settings_row_at(300, 200, 960, 0) != 7) { puts("settingsclick: General row (Location) center missed\n"); ok = 0; }
        if (settings_row_at(300, 70,  960, 0) != -1) { puts("settingsclick: above the first General row should miss\n"); ok = 0; }
        if (settings_row_at(300, 119, 960, 0) != -1) { puts("settingsclick: real gap between rows should miss\n"); ok = 0; }
        if (settings_row_at(180, 92,  960, 0) != -1) { puts("settingsclick: left of the detail pane (in the sidebar's x range) should miss\n"); ok = 0; }
        if (settings_row_at(950, 92,  960, 0) != -1) { puts("settingsclick: right of the detail pane should miss\n"); ok = 0; }
        if (settings_row_at(300, 92,  960, 1) != 3) { puts("settingsclick: Assistant row (LLM model) center missed\n"); ok = 0; }
        if (settings_row_at(300, 128, 960, 1) != 4) { puts("settingsclick: Assistant row (LLM host:port) center missed\n"); ok = 0; }
        if (settings_row_at(300, 92,  960, 2) != 5) { puts("settingsclick: Account row (Account) center missed\n"); ok = 0; }
        /* Same physical y (92, each section's own first row) resolves to a
           different absolute row index depending which section is showing
           -- proves the hit-test is scoped to what's actually on screen,
           not just "any known row y" independent of section. */
        if (settings_row_at(300, 92, 960, 1) == 0) { puts("settingsclick: Assistant section must not hit General's Wind row\n"); ok = 0; }
        if (settings_sidebar_at(90, 60)  != 0) { puts("settingsclick: sidebar row 0 (General) missed\n"); ok = 0; }
        if (settings_sidebar_at(90, 94)  != 1) { puts("settingsclick: sidebar row 1 (Assistant) missed\n"); ok = 0; }
        if (settings_sidebar_at(90, 128) != 2) { puts("settingsclick: sidebar row 2 (Account) missed\n"); ok = 0; }
        if (settings_sidebar_at(90, 40)  != -1) { puts("settingsclick: above the sidebar's first row should miss\n"); ok = 0; }
        if (settings_sidebar_at(2,  60)  != -1) { puts("settingsclick: left of the sidebar (x<8) should miss\n"); ok = 0; }
        if (settings_sidebar_at(170,60)  != -1) { puts("settingsclick: right of the sidebar should miss\n"); ok = 0; }

        int wind_before = wind_enabled;
        wind_enabled = !wind_enabled;
        settings_save();
        int wind_saved = wind_enabled;
        wind_enabled = !wind_enabled; /* corrupt the in-memory value so a real disk round-trip is what proves it, not just the assignment above */
        settings_load();
        if (wind_enabled != wind_saved) { puts("settingsclick: Wind toggle did not survive settings_save/settings_load\n"); ok = 0; }
        wind_enabled = wind_before; settings_save(); /* restore, no lasting side effect on the rest of this boot */

        puts(ok ? "settingsclick: click hit-tests the row it actually landed on, sidebar sections hit-test correctly, and a toggle survives save/load: ok\n" : "settingsclick: FAILED\n");
        serial_puts(ok ? "settingsclick PASS\n" : "settingsclick FAIL\n"); /* mirrors texttest/chattest/jpegtest's own convention so a tools/checks shell script can read the verdict headless */
    }
    else return 0;
    return 1;
}
