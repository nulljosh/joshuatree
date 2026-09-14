#include "editor_fonts.h"

static char editor_buffer[4096];
static int editor_length, editor_position, editor_loaded, editor_dirty;
static int editor_family, editor_size = 1, editor_weight, editor_scroll;
static int editor_mouse_x = 400, editor_mouse_y = 300;
static const char *editor_status = "NOTES.TXT   |   Ctrl+S saves   Esc closes";

static const struct editor_glyph *editor_glyph_for(unsigned char character) {
    if (character < 32 || character > 126) character = '?';
    return &editor_glyphs[((editor_family * 2 + editor_weight) * 4 + editor_size) * 95 + character - 32];
}

static void editor_draw_glyph(unsigned char character, int origin_x, int origin_y) {
    const struct editor_glyph *glyph = editor_glyph_for(character);
    for (int row = 0; row < glyph->height; row++) {
        for (int column = 0; column < glyph->width; column++) {
            int alpha = editor_pixels[glyph->offset + row * glyph->width + column];
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
#define EDITOR_TEXT_TOP 92
#define EDITOR_STATUS_H 30
static int editor_visible_lines(int line_height) {
    int n = ((int)window_height() - EDITOR_TEXT_TOP - EDITOR_STATUS_H) / line_height;
    return n < 1 ? 1 : n;
}

static void editor_layout(int draw, int *caret_x, int *caret_line) {
    int text_x = 56, line = 0;
    int line_height = 26 + editor_size * 5;
    int visible_lines = editor_visible_lines(line_height);
    for (int index = 0; index <= editor_length; index++) {
        unsigned char character = editor_buffer[index];
        int advance = character == '\t' ? editor_glyph_for(' ')->advance * 4 : editor_glyph_for(character)->advance;
        if (text_x + advance > (int)window_width() - 56 && character != '\n') { text_x = 56; line++; }
        if (index == editor_position) { *caret_x = text_x; *caret_line = line; }
        if (index == editor_length) break;
        if (character == '\n') { text_x = 56; line++; continue; }
        if (draw && character != '\t' && line >= editor_scroll && line < editor_scroll + visible_lines)
            editor_draw_glyph(character, text_x, EDITOR_TEXT_TOP + (line - editor_scroll) * line_height);
        text_x += advance;
    }
}

static void editor_draw(void) {
    static const char *families[] = {"Sans", "Serif", "Mono"};
    static const char *sizes[] = {"16 px", "20 px", "24 px", "28 px"};
    window_clear(0x00FAF8F6);
    gui_draw_app_titlebar(editor_dirty ? "Notes *" : "Notes");
    window_rect(20, 42, 760, 34, 0x00EAE4DC);
    font_draw_string("F1 Font:", 32, 51, 0x0075726E, -1);
    font_draw_string(families[editor_family], 108, 51, 0x001C1C1E, -1);
    font_draw_string("F2 Size:", 236, 51, 0x0075726E, -1);
    font_draw_string(sizes[editor_size], 312, 51, 0x001C1C1E, -1);
    font_draw_string("F3 Weight:", 450, 51, 0x0075726E, -1);
    font_draw_string(editor_weight ? "Bold" : "Regular", 540, 51, 0x001C1C1E, -1);
    font_draw_string("Save", 708, 51, 0x0085144B, -1);
    int caret_x = 56, caret_line = 0;
    int line_height = 26 + editor_size * 5;
    int visible_lines = editor_visible_lines(line_height);
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
            font_draw_string("File exceeds 4095 bytes. Editing disabled to protect it.", 20, 60, 0x001C1C1E, -1);
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
            int outside = editor_mouse_x < 0 || editor_mouse_y < 0
                       || editor_mouse_x >= (int)window_width() || editor_mouse_y >= (int)window_height();
            if (outside || (editor_mouse_y < 32 && editor_mouse_x < 38)) close = 1;
            else if (editor_mouse_y >= 42 && editor_mouse_y < 76) {
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
            int released = scan & 0x80, code = scan & 0x7F;
            if (code == 0x2A) shift_left = !released;
            else if (code == 0x36) shift_right = !released;
            else if (code == 0x1D) control = !released;
            else if (!released) {
                changed = 1;
                if (code == 0x3A) caps ^= 1;
                else if (code == 1) close = 1;
                else if (code == 0x3B) editor_family = (editor_family + 1) % 3;
                else if (code == 0x3C) editor_size = (editor_size + 1) % 4;
                else if (code == 0x3D) editor_weight ^= 1;
                else if (control && code == 0x1F) save = 1;
                else if (extended) {
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
                    if (code == 0x53 && editor_position < editor_length) {
                        for (int index = editor_position; index < editor_length; index++) editor_buffer[index] = editor_buffer[index + 1];
                        editor_length--; editor_dirty = 1;
                    }
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
                    if (character == '\b' && editor_position > 0) {
                        for (int index = editor_position - 1; index < editor_length; index++) editor_buffer[index] = editor_buffer[index + 1];
                        editor_position--; editor_length--; editor_dirty = 1;
                    } else if (character == '\n' || character == '\t' || (character >= 32 && character <= 126)) {
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
        else __asm__ volatile ("hlt");
    }
}
