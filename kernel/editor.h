#include "editor_fonts.h"

static char editor_buffer[4096];
static int editor_length, editor_position, editor_loaded, editor_dirty;
static int editor_family, editor_size = 1, editor_weight, editor_scroll;
static int editor_mouse_x = 400, editor_mouse_y = 300;
static const char *editor_status = "NOTES.TXT   |   Ctrl+S saves   Esc closes";

/* v1.2.0: real text selection. -1 means no selection; otherwise this is
   the position Shift+arrow/Ctrl+A started extending from, and the active
   range is always [min(anchor,editor_position), max(...)) -- there is no
   separate "which end is the caret" flag because editor_position IS the
   caret, the same way every other editor's selection follows its caret. */
static int editor_sel_anchor = -1;

/* True only when the anchor and the caret actually differ: an anchor left
   sitting exactly on the caret (Shift pressed, then released with no net
   movement) is not a real, drawable/copyable selection. */
static int editor_selection_range(int *lo, int *hi) {
    if (editor_sel_anchor < 0 || editor_sel_anchor == editor_position) return 0;
    if (editor_sel_anchor < editor_position) { *lo = editor_sel_anchor; *hi = editor_position; }
    else { *lo = editor_position; *hi = editor_sel_anchor; }
    return 1;
}

/* Shared decimal-append, same shape clip_serial_dump already uses for its
   own length field: no libc, no sprintf. */
static void editor_append_uint(char *out, int *k, unsigned int v) {
    char d[10]; int dn = 0;
    do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (dn) out[(*k)++] = d[--dn];
}

/* "edsel=<start>,<end>": fired only when a real (non-empty) selection's
   bounds change, never on every keystroke, so a check can grep for the
   exact range Shift+arrow/Ctrl+A just produced. */
static void editor_serial_sel(void) {
    int lo, hi;
    if (!editor_selection_range(&lo, &hi)) return;
    char out[40]; int k = 0;
    const char *tag = "edsel=";
    for (const char *p = tag; *p; p++) out[k++] = *p;
    editor_append_uint(out, &k, (unsigned int)lo);
    out[k++] = ',';
    editor_append_uint(out, &k, (unsigned int)hi);
    out[k++] = '\n'; out[k] = 0;
    serial_puts(out);
}

/* "edcopy=<n>" / "edcut=<n>": fired only for a selection-driven Ctrl+C/X
   (the pre-1.2.0 whole-line Ctrl+C/X path keeps CLIPCOPY's own marker,
   see clipboard_set below), so a check can prove the count matches
   exactly the range that was highlighted, not just that some copy ran. */
static void editor_serial_count(const char *tag, unsigned int n) {
    char out[24]; int k = 0;
    for (const char *p = tag; *p; p++) out[k++] = *p;
    editor_append_uint(out, &k, n);
    out[k++] = '\n'; out[k] = 0;
    serial_puts(out);
}

/* Removes the active selection from the buffer, leaves the caret at its
   start, and clears the anchor -- the one real choke point every
   selection-replacing action (typing, Backspace, Delete, Ctrl+X, a paste
   over a selection) below goes through, so none of them can forget to
   clear the anchor or drop a byte off the shift. No-op with no active
   selection. */
static void editor_delete_selection(void) {
    int lo, hi;
    if (!editor_selection_range(&lo, &hi)) return;
    int n = hi - lo;
    for (int index = lo; index <= editor_length - n; index++) editor_buffer[index] = editor_buffer[index + n];
    editor_length -= n;
    editor_position = lo;
    editor_sel_anchor = -1;
    editor_dirty = 1;
}

static const struct editor_glyph *editor_glyph_for(unsigned char character) {
    if (character < 32 || character > 126) character = '?';
    return &editor_glyphs[((editor_family * 2 + editor_weight) * 4 + editor_size) * 95 + character - 32];
}

static void editor_draw_glyph(unsigned char character, int origin_x, int origin_y) {
    const struct editor_glyph *glyph = editor_glyph_for(character);
    for (int row = 0; row < glyph->height; row++) {
        for (int column = 0; column < glyph->width; column++) {
            int alpha = text_ink_dark[editor_pixels[glyph->offset + row * glyph->width + column]]; /* dark ink on the light page: same curve as gui_aa_char (see text_ink) */
            int red = (28 * alpha + 250 * (255 - alpha)) / 255;
            int green = (28 * alpha + 248 * (255 - alpha)) / 255;
            int blue = (30 * alpha + 246 * (255 - alpha)) / 255;
            window_pixel(origin_x + glyph->left + column, origin_y + glyph->top + row,
                         (red << 16) | (green << 8) | blue);
        }
    }
}

/* v67: the text area is sized from the real window, not a hardcoded
   800x600. Dock-launched Notes lives in gui_launch_from_dock's 804x345
   viewport, where the old fixed 450px area and a status line at y=570
   were both clipped clean off, and the caret's scroll logic believed
   lines were visible that nothing ever drew. */
/* Toolbar and text top follow gui_app_dy(): a dock window's frame already
   draws the title bar, so both move up by the strip a full-screen Notes
   keeps for its own. */
#define EDITOR_BAR_Y (42 + gui_app_dy())
#define EDITOR_TEXT_TOP (92 + gui_app_dy())
#define EDITOR_STATUS_H 30
static int editor_visible_lines(int line_height) {
    int n = ((int)window_height() - EDITOR_TEXT_TOP - EDITOR_STATUS_H) / line_height;
    return n < 1 ? 1 : n;
}

static void editor_layout(int draw, int *caret_x, int *caret_line) {
    int text_x = 56, line = 0;
    int line_height = 26 + editor_size * 5;
    int visible_lines = editor_visible_lines(line_height);
    int limit = (int)window_width() - 56, word_start = 1;
    for (int index = 0; index <= editor_length; index++) {
        unsigned char character = editor_buffer[index];
        int advance = character == '\t' ? editor_glyph_for(' ')->advance * 4 : editor_glyph_for(character)->advance;
        /* Wrap at word boundaries: at the first letter of a word, measure
           the whole word and move it down if it will not fit on this line.
           Used to break mid-word ("fox j" / "umps"). A word wider than a
           line still breaks by character below. */
        if (word_start && text_x > 56 && character != ' ' && character != '\n' && character != '\t') {
            int w = 0;
            for (int j = index; j < editor_length; j++) {
                unsigned char cj = editor_buffer[j];
                if (cj == ' ' || cj == '\n' || cj == '\t') break;
                w += editor_glyph_for(cj)->advance;
                if (text_x + w > limit) break;
            }
            if (text_x + w > limit && w <= limit - 56) { text_x = 56; line++; }
        }
        word_start = (character == ' ' || character == '\t');
        if (text_x + advance > limit && character != '\n') { text_x = 56; line++; }
        if (index == editor_position) { *caret_x = text_x; *caret_line = line; }
        if (index == editor_length) break;
        if (character == '\n') { text_x = 56; line++; continue; }
        if (draw && character != '\t' && line >= editor_scroll && line < editor_scroll + visible_lines) {
            /* v1.2.0: the selection highlight, a light-blue band drawn
               BEHIND the glyph so the ink still reads on top of it, only
               over the exact columns actually selected on this line --
               never a whole-line bar, never drawn for an unselected run. */
            int sel_lo, sel_hi;
            if (editor_selection_range(&sel_lo, &sel_hi) && index >= sel_lo && index < sel_hi)
                window_rect(text_x, EDITOR_TEXT_TOP + 2 + (line - editor_scroll) * line_height, advance, 20 + editor_size * 4, 0x00B4D5FE);
            editor_draw_glyph(character, text_x, EDITOR_TEXT_TOP + (line - editor_scroll) * line_height);
        }
        text_x += advance;
    }
}

static const char *EDITOR_FAMILIES[] = {"Sans", "Serif", "Mono"};
static const char *EDITOR_SIZES[] = {"16 px", "20 px", "24 px", "28 px"};

/* v0.76.10: direct report + real screen recording, "redraws the entire
   screen on every keystroke" -- confirmed real. editor_draw() used to
   window_clear() the WHOLE window and repaint the titlebar and toolbar
   labels on every single character typed, even though none of that chrome
   changes between keystrokes (family/size/weight/dirty-state only change
   on a toolbar click or the dirty-flag's one 0->1 flip). This kernel has
   no double buffer yet (a real, already-tracked architectural gap, see
   the "Multi-window, honestly scoped" roadmap entry's own backing-store
   box, still unchecked) -- window_* calls draw straight into the shared
   framebuffer, so a bigger redraw is a longer visible flash, especially
   once v86's own JS/WASM per-pixel drawing overhead is added on top of a
   real browser's own frame timing. Splitting chrome (titlebar + toolbar,
   drawn only when its own state actually changed) from the text region
   (redrawn every keystroke, since content genuinely changes) cuts the
   redrawn area to roughly the text region alone for the overwhelmingly
   common case of "just typed a character," which is most of what a real
   visitor's keystrokes are. This does not add real double buffering --
   that's the compositor rewrite the roadmap already scopes as its own
   multi-session project -- it only shrinks how much of the screen a
   single keystroke has to repaint under the current no-buffer model. */
static int editor_chrome_family = -1, editor_chrome_size = -1, editor_chrome_weight = -1, editor_chrome_dirty = -1;

static void editor_draw_chrome(void) {
    /* Real, discriminating regression coverage per CLAUDE.md 4b:
       editorflash-check.sh counts these lines to prove the chrome band
       redraws once on open and again only on an actual toolbar-relevant
       change, never once per plain keystroke. */
    serial_puts("editorchrome\n");
    /* Clears the whole chrome band first: gui_draw_app_titlebar and the
       toolbar's own window_rect below don't necessarily cover every pixel
       between them (there's real gap space at their seams), and this is
       the one thing the old single window_clear() used to guarantee that
       splitting the redraw could otherwise silently lose. */
    window_rect(0, 0, (int)window_width(), EDITOR_TEXT_TOP, 0x00FAF8F6);
    gui_draw_app_titlebar(editor_dirty ? "Notes *" : "Notes");
    window_rect(20, EDITOR_BAR_Y, 760, 34, 0x00EAE4DC);
    font_draw_string("F1 Font:", 32, EDITOR_BAR_Y + 9, 0x0075726E, -1);
    font_draw_string(EDITOR_FAMILIES[editor_family], 108, EDITOR_BAR_Y + 9, 0x001C1C1E, -1);
    font_draw_string("F2 Size:", 236, EDITOR_BAR_Y + 9, 0x0075726E, -1);
    font_draw_string(EDITOR_SIZES[editor_size], 312, EDITOR_BAR_Y + 9, 0x001C1C1E, -1);
    font_draw_string("F3 Weight:", 450, EDITOR_BAR_Y + 9, 0x0075726E, -1);
    font_draw_string(editor_weight ? "Bold" : "Regular", 540, EDITOR_BAR_Y + 9, 0x001C1C1E, -1);
    font_draw_string("Save", 708, EDITOR_BAR_Y + 9, 0x0085144B, -1);
}

static void editor_draw(void) {
    if (editor_family != editor_chrome_family || editor_size != editor_chrome_size
     || editor_weight != editor_chrome_weight || editor_dirty != editor_chrome_dirty) {
        editor_draw_chrome();
        editor_chrome_family = editor_family; editor_chrome_size = editor_size;
        editor_chrome_weight = editor_weight; editor_chrome_dirty = editor_dirty;
    }
    int caret_x = 56, caret_line = 0;
    int line_height = 26 + editor_size * 5;
    int visible_lines = editor_visible_lines(line_height);
    window_rect(0, EDITOR_TEXT_TOP, (int)window_width(), (int)window_height() - EDITOR_TEXT_TOP, 0x00FAF8F6);
    editor_layout(0, &caret_x, &caret_line);
    if (caret_line < editor_scroll) editor_scroll = caret_line;
    if (caret_line >= editor_scroll + visible_lines) editor_scroll = caret_line - visible_lines + 1;
    editor_layout(1, &caret_x, &caret_line);
    window_rect(caret_x, EDITOR_TEXT_TOP + 2 + (caret_line - editor_scroll) * line_height, 2, 20 + editor_size * 4, 0x0085144B);
    font_draw_string(editor_status, 20, (int)window_height() - EDITOR_STATUS_H, 0x0075726E, -1);
    /* Windowed (dock-launched): gui_app_mouse_tick owns the pointer sprite,
       in screen coordinates outside this viewport. Drawing a second one
       here at a viewport-local position was a real double cursor. */
    if (!gui_app_windowed) gui_draw_cursor(editor_mouse_x, editor_mouse_y);
}

static int editor_save(void) {
    editor_buffer[editor_length] = 0;
    if (!vfs_replace_file("NOTES.TXT", editor_buffer, editor_length)) {
        editor_status = "Save failed. Text kept in RAM. Esc closes; Ctrl+S retries.";
        return 0;
    }
    editor_dirty = 0;
    editor_status = "Saved to NOTES.TXT   |   Ctrl+S saves   Esc closes";
    return 1;
}

/* v1.0.6: no selection model exists yet (see docs/roadmap.md), so Ctrl+C/X
   act on "the current line" -- the run of editor_buffer between the
   newlines either side of editor_position, same contract the single-line
   fields (gui_prompt_line_input, Terminal) use for their one line. */
static int editor_line_start(int pos) {
    while (pos > 0 && editor_buffer[pos - 1] != '\n') pos--;
    return pos;
}
static int editor_line_end(int pos) {
    while (pos < editor_length && editor_buffer[pos] != '\n') pos++;
    return pos;
}

static void editor_vertical(int direction) {
    int start = editor_position;
    while (start > 0 && editor_buffer[start - 1] != '\n') start--;
    int column = editor_position - start;
    if (direction < 0) {
        if (start == 0) return;
        int end = start - 1;
        start = end;
        while (start > 0 && editor_buffer[start - 1] != '\n') start--;
        editor_position = start + column < end ? start + column : end;
    } else {
        int next = editor_position;
        while (next < editor_length && editor_buffer[next] != '\n') next++;
        if (next == editor_length) return;
        start = ++next;
        while (next < editor_length && editor_buffer[next] != '\n') next++;
        editor_position = start + column < next ? start + column : next;
    }
}

static void gui_launch_editor(void) {
    if (!editor_loaded) {
        editor_length = vfs_read_file("NOTES.TXT", editor_buffer, sizeof(editor_buffer));
        if (editor_length >= (int)sizeof(editor_buffer)) {
            window_clear(0x00FAF8F6);
            gui_draw_app_titlebar("Notes");
            font_draw_string("File exceeds 4095 bytes. Editing disabled to protect it.", 20, 60 + gui_app_dy(), 0x001C1C1E, -1);
            gui_wait_close();
            return;
        }
        if (editor_length < 0) editor_length = 0;
        editor_position = editor_length;
        editor_loaded = 1;
    }
    int shift_left = 0, shift_right = 0, control = 0, caps = 0, extended = 0;
    int delta_x, delta_y, buttons;
    /* v67 (0.62.2), the real "stuck on Notes" bug. This loop used to read
       mouse_get_delta alone and never mouse_get_absolute, so under v62's
       vmmouse backdoor (live on QEMU's default pc machine and in v86, i.e.
       the native app AND the landing page) the pointer never moved from
       the dock tile that opened the app and the close hitbox could never
       be reached: no click anywhere closed Notes, and with the dock still
       visible around the modal window every later dock click looked dead
       too, which is how one app's bug got reported as "all apps".
       Windowed, the pointer now comes from the same gui_app_mouse_tick
       every other app's wait loop uses (screen coordinates, absolute-
       aware); clicks come from mouse_click_edge, which also survives a
       tap's press+release landing in one poll. Anything outside the text
       area (the window chrome, the X, the desktop) closes, the same
       "click closes" contract the rest of the GUI keeps for a visitor
       with no keyboard; inside it the toolbar keeps working. */
    mouse_click_edge_sync();
    if (gui_app_windowed) gui_app_cursor_hide();
    /* Forces editor_draw()'s very next call to redraw the chrome band
       unconditionally: family/size/weight/dirty are static and persist
       across a close+reopen, so without this a reopen could skip
       repainting the titlebar/toolbar under the (wrong, for a fresh
       window) assumption that nothing chrome-related changed, leaving
       whatever the framebuffer happened to hold there from a different
       app in between. */
    editor_chrome_family = -1;
    editor_sel_anchor = -1; /* v1.2.0: a fresh window session starts with no selection, even if a prior close/reopen this boot left one set */
    editor_draw();
    for (;;) {
        int changed = 0, close = 0, save = 0;
        if (gui_app_windowed) {
            gui_app_mouse_tick();
            editor_mouse_x = app_cursor_x - app_view_x;
            editor_mouse_y = app_cursor_y - app_view_y;
        } else if (mouse_get_delta(&delta_x, &delta_y, &buttons)) {
            editor_mouse_x += delta_x; editor_mouse_y += delta_y;
            mouse_get_absolute(&editor_mouse_x, &editor_mouse_y, (int)window_width(), (int)window_height());
            if (editor_mouse_x < 0) editor_mouse_x = 0;
            if (editor_mouse_x > (int)window_width() - 1) editor_mouse_x = (int)window_width() - 1;
            if (editor_mouse_y < 0) editor_mouse_y = 0;
            if (editor_mouse_y > (int)window_height() - 1) editor_mouse_y = (int)window_height() - 1;
            changed = 1;
        }
        if (mouse_click_edge()) {
            gui_close_was_click = 1;
            int outside = editor_mouse_x < 0 || editor_mouse_y < 0
                       || editor_mouse_x >= (int)window_width() || editor_mouse_y >= (int)window_height();
            if (outside || (!gui_app_windowed && editor_mouse_y < 32 && editor_mouse_x < 38)) close = 1;
            else if (editor_mouse_y >= EDITOR_BAR_Y && editor_mouse_y < EDITOR_BAR_Y + 34) {
                if (editor_mouse_x < 220) editor_family = (editor_family + 1) % 3;
                else if (editor_mouse_x < 430) editor_size = (editor_size + 1) % 4;
                else if (editor_mouse_x < 680) editor_weight ^= 1;
                else save = 1;
                changed = 1;
            }
        }
        int scan = kbd_pop();
        if (scan == 0xE0) { extended = 1; continue; }
        if (scan >= 0) {
            gui_close_was_click = 0;
            int released = scan & 0x80, code = scan & 0x7F;
            if (code == 0x2A) shift_left = !released;
            else if (code == 0x36) shift_right = !released;
            else if (code == 0x1D) control = !released;
            else if (!released) {
                changed = 1;
                if (code == 0x3A) caps ^= 1;
                else if (code == 1) {
                    /* v1.2.0: Escape clears an active selection first, the
                       same "navigate deselects" contract a plain arrow
                       gets below; only a SECOND Escape (nothing left to
                       clear) closes Notes, so a selection never eats the
                       one key every read-only viewer already relies on to
                       leave the app. */
                    if (editor_sel_anchor >= 0) editor_sel_anchor = -1; else close = 1;
                }
                else if (code == 0x3B) editor_family = (editor_family + 1) % 3;
                else if (code == 0x3C) editor_size = (editor_size + 1) % 4;
                else if (code == 0x3D) editor_weight ^= 1;
                else if (control && code == 0x1F) save = 1;
                else if (control && code == 0x1E) {
                    /* Ctrl+A: select the whole buffer, caret at the end,
                       the same convention Ctrl+A's own select-all keeps
                       everywhere else it exists. */
                    editor_sel_anchor = 0;
                    editor_position = editor_length;
                    editor_serial_sel();
                }
                else if (control && (code == 0x2E || code == 0x2D)) {
                    /* v1.2.0: Ctrl+C/X now prefer a real selection when one
                       is active. With none, this is exactly the pre-1.2.0
                       "current line" fallback (docs/roadmap.md's own
                       "no selection model exists yet" note, now stale) --
                       same clipboard_set/CLIPCOPY choke point either way,
                       so Ctrl+V and every other existing consumer of the
                       one global clipboard keep working unchanged. */
                    int sel_lo, sel_hi, has_sel = editor_selection_range(&sel_lo, &sel_hi);
                    int ls = has_sel ? sel_lo : editor_line_start(editor_position);
                    int le = has_sel ? sel_hi : editor_line_end(editor_position);
                    clipboard_set(&editor_buffer[ls], (unsigned int)(le - ls));
                    if (has_sel) editor_serial_count(code == 0x2D ? "edcut=" : "edcopy=", (unsigned int)(le - ls));
                    if (code == 0x2D) {
                        if (has_sel) editor_delete_selection();
                        else {
                            for (int index = ls; index <= editor_length - (le - ls); index++)
                                editor_buffer[index] = editor_buffer[index + (le - ls)];
                            editor_length -= (le - ls); editor_position = ls; editor_dirty = 1;
                        }
                    }
                }
                else if (control && code == 0x2F) {
                    /* Ctrl+V: paste at the cursor, truncated cleanly at the
                       4095-byte buffer limit -- never overflows
                       editor_buffer, same bound plain typing enforces
                       below. v1.2.0: an active selection is replaced by
                       the paste, same as typing a character over it. */
                    if (editor_sel_anchor >= 0) editor_delete_selection();
                    unsigned int room = (unsigned int)sizeof(editor_buffer) - 1 - (unsigned int)editor_length;
                    unsigned int take = clipboard_len < room ? clipboard_len : room;
                    if (take) {
                        for (int index = editor_length + (int)take - 1; index >= editor_position + (int)take; index--)
                            editor_buffer[index] = editor_buffer[index - (int)take];
                        for (unsigned int i = 0; i < take; i++) editor_buffer[editor_position + (int)i] = clipboard_buf[i];
                        clip_serial_dump("CLIPPASTE:", &editor_buffer[editor_position], take);
                        if (take < clipboard_len) { serial_puts("CLIPTRUNC\n"); editor_status = "Pasted; rest didn't fit the 4095-byte limit."; }
                        editor_position += (int)take; editor_length += (int)take; editor_dirty = 1;
                    }
                }
                else if (extended) {
                    /* v1.2.0: Shift+Left/Right/Up/Down/Home/End extends a
                       selection from wherever the caret already was; the
                       same keys with no Shift held clear whatever
                       selection existed instead of moving it, the
                       standard "navigating deselects" contract. Delete
                       (0x53) is handled on its own below it, since with an
                       active selection it removes the selection instead
                       of moving the caret forward a character. */
                    int shift = shift_left || shift_right;
                    int is_nav = (code == 0x4B || code == 0x4D || code == 0x48 || code == 0x50 || code == 0x47 || code == 0x4F);
                    if (is_nav) {
                        if (shift) { if (editor_sel_anchor < 0) editor_sel_anchor = editor_position; }
                        else editor_sel_anchor = -1;
                    }
                    if (code == 0x4B && editor_position > 0) editor_position--;
                    if (code == 0x4D && editor_position < editor_length) editor_position++;
                    if (code == 0x48) editor_vertical(-1);
                    if (code == 0x50) editor_vertical(1);
                    if (code == 0x47) {
                        if (control) editor_position = 0;
                        else while (editor_position > 0 && editor_buffer[editor_position - 1] != '\n') editor_position--;
                    }
                    if (code == 0x4F) {
                        if (control) editor_position = editor_length;
                        else while (editor_position < editor_length && editor_buffer[editor_position] != '\n') editor_position++;
                    }
                    if (code == 0x53) {
                        if (editor_sel_anchor >= 0) editor_delete_selection();
                        else if (editor_position < editor_length) {
                            for (int index = editor_position; index < editor_length; index++) editor_buffer[index] = editor_buffer[index + 1];
                            editor_length--; editor_dirty = 1;
                        }
                    }
                    if (is_nav && shift) editor_serial_sel();
                } else if (!control) {
                    char character = SC[code];
                    int shift = shift_left || shift_right;
                    if (character >= 'a' && character <= 'z') {
                        if (shift ^ caps) character -= 32;
                    } else if (shift) {
                        const char *normal = "1234567890-=[];'`,./\\";
                        const char *shifted = "!@#$%^&*()_+{}:\"~<>?|";
                        for (int index = 0; normal[index]; index++)
                            if (character == normal[index]) { character = shifted[index]; break; }
                    }
                    if (character == '\b') {
                        /* v1.2.0: Backspace with an active selection removes
                           the selection instead of the one char behind the
                           caret. */
                        if (editor_sel_anchor >= 0) editor_delete_selection();
                        else if (editor_position > 0) {
                            for (int index = editor_position - 1; index < editor_length; index++) editor_buffer[index] = editor_buffer[index + 1];
                            editor_position--; editor_length--; editor_dirty = 1;
                        }
                    } else if (character == '\n' || character == '\t' || (character >= 32 && character <= 126)) {
                        /* v1.2.0: typing over an active selection replaces
                           it, the same "typing eats the selection" contract
                           every other real text editor keeps. */
                        if (editor_sel_anchor >= 0) editor_delete_selection();
                        if (editor_length < (int)sizeof(editor_buffer) - 1) {
                            for (int index = editor_length; index > editor_position; index--) editor_buffer[index] = editor_buffer[index - 1];
                            editor_buffer[editor_position++] = character;
                            editor_length++; editor_dirty = 1;
                        } else editor_status = "4095-byte limit reached. Save before starting another note.";
                    }
                }
            }
            extended = 0;
        }
        editor_buffer[editor_length] = 0;
        if (save) { editor_save(); changed = 1; }
        if (close) {
            /* v67: a failed save no longer holds the window open for a
               second Esc/click. The landing page's v86 boot has no FAT
               disk at all, so every save there fails, and the idle tour
               (which types real text into Notes, then taps once to
               close) sat on a "Save failed" screen forever, one more
               way to be "stuck on Notes" even with the pointer fixed.
               The text isn't lost: editor_buffer is static and stays
               loaded, so reopening Notes shows it, with the status line
               saying why it's RAM-only. */
            if (editor_dirty && !editor_save())
                editor_status = "Save failed (no disk?). Text kept in RAM until reboot.   Ctrl+S retries";
            return;
        }
        if (changed) {
            if (gui_app_windowed) gui_app_cursor_hide();
            editor_draw();
        }
        /* Present on EVERY pass, redraw or not. This was written as
           `else window_present(); __asm__ ("hlt");`, where the else binds
           to the present alone, so the one case that actually had something
           new to show (changed, so editor_draw just ran) was the one case
           that never reached the screen. A typography or font change drew
           into the back buffer and sat there until some later idle pass
           happened to present it. Caught by editor_qa.py in CI, which
           sampled a frame the kernel had drawn and not yet shown. */
        window_present();
        __asm__ volatile ("hlt");
    }
}
