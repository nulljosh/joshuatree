/* v0.86.0: Search. A real Spotlight-shaped app, scoped honestly against
   what this kernel's VFS actually supports: vfs_list (drivers/vfs.h) is a
   real driver-level directory listing (fat.c walks real FAT directory
   entries, ramfs.c walks its real flat file table), the same call Files
   (kernel.c's gui_draw_files_content) already uses. There is no recursive
   directory-walk API at any layer yet, kernel or ring-3, so this searches
   the current directory of whichever backend `fsuse` has active, exactly
   the same honest scope Files already has, not a fabricated whole-disk
   index. Opening a directory result chdirs into it and reloads a fresh
   real listing (vfs_chdir, vfs_list), the same two real primitives the
   shell's own `cd`/`ls` commands use; opening a file shows its real bytes
   the way `cat` does (vfs_read_file), not a summary or a guess.

   Same file/UI shape as reminders.h/contacts.h: a fixed-size static array,
   no persistence of its own (nothing to persist, it mirrors the real
   filesystem), gui_prompt.h's chrome/content split so filtering as you
   type never redraws the whole window (the exact per-keystroke
   window_clear bug class this repo keeps fixing). Substring filter,
   case-insensitive, no fuzzy scoring: a real "does this filename contain
   what was typed" check is a legitimate v1, the direct request's own call. */
#define SEARCH_MAX_FILES 16 /* matches gui_fat_collect's own real display cap for a flat listing */
#define SEARCH_NAME_MAX 13  /* FAT 8.3 name, 12 chars + NUL; ramfs names are truncated to fit the same real limit */
#define SEARCH_QUERY_MAX 32

typedef struct {
    char name[SEARCH_NAME_MAX];
    int is_dir;
} search_file_t;

static search_file_t search_files[SEARCH_MAX_FILES];
static int search_file_count;

static void search_collect(const char *name, unsigned int size, int is_dir) {
    (void)size;
    if (search_file_count >= SEARCH_MAX_FILES) return;
    int i = 0;
    while (name[i] && i < SEARCH_NAME_MAX - 1) { search_files[search_file_count].name[i] = name[i]; i++; }
    search_files[search_file_count].name[i] = 0;
    search_files[search_file_count].is_dir = is_dir;
    search_file_count++;
}

static void search_reload(void) {
    search_file_count = 0;
    vfs_list(search_collect);
}

static int search_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Real substring match, case-insensitive, hand-rolled (no strstr in this
   freestanding libc). An empty query matches everything, the same
   "type nothing, see the full list" Spotlight itself starts from. */
static int search_contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && search_lower((unsigned char)hay[i + j]) == search_lower((unsigned char)needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static char search_query[SEARCH_QUERY_MAX];
static unsigned int search_query_len;
static int search_matches[SEARCH_MAX_FILES];
static int search_match_count;
static int search_sel;

static void search_refilter(void) {
    search_query[search_query_len] = 0;
    search_match_count = 0;
    for (int i = 0; i < search_file_count; i++) {
        if (search_contains_ci(search_files[i].name, search_query))
            search_matches[search_match_count++] = i;
    }
    if (search_sel >= search_match_count) search_sel = search_match_count > 0 ? search_match_count - 1 : 0;
    if (search_sel < 0) search_sel = 0;
}

/* cat-style read-only content view for a real file match: same real
   vfs_read_file + render_wrapped_text pair kernel.c's own shell `cat`
   command and gui_launch_html already use, not a summary or a guess at
   the content. gui_wait_close's contract (esc or click returns) is
   reused as-is; the caller redraws the search screen fresh on return. */
static void search_open_file(const char *name) {
    int T = gui_app_dy();
    window_clear(GUI_BG);
    gui_draw_app_titlebar(name);
    static char buf[4096];
    int n = vfs_read_file(name, buf, sizeof(buf) - 1);
    if (n < 0) {
        font_draw_string("Could not read this file.", 20, T + 50, 0x00A33B3B, -1);
    } else {
        buf[n] = 0;
        render_wrapped_text(buf, 20, T + 44, (int)window_width() - 40, (int)window_height() - 90 - T, 0x001C1C1E);
    }
    gui_wait_close();
}

/* Redraws only the query box and the result list, the gui_prompt.h chrome/
   content split: the titlebar and the "type to filter" instructions above
   it are chrome, drawn once by gui_launch_search below, never here. */
static void search_draw_content(void) {
    int T = gui_app_dy();
    int w = (int)window_width();
    window_rect(20, T + 76, w - 40, 20, 0x00FFFFFF);
    search_query[search_query_len] = 0;
    font_draw_string(search_query, 24, T + 78, 0x001C1C1E, -1);
    int list_h = (int)window_height() - 120 - T;
    if (list_h < 0) list_h = 0;
    window_rect(16, T + 104, w - 32, list_h, GUI_BG);
    if (!search_file_count) {
        font_draw_string("(no files, or no filesystem mounted)", 20, T + 110, 0x00807468, -1);
        return;
    }
    if (!search_match_count) {
        font_draw_string("No matches.", 20, T + 110, 0x00807468, -1);
        return;
    }
    for (int r = 0; r < search_match_count; r++) {
        int y = T + 110 + r * 22;
        if (y + 20 > T + 104 + list_h) break; /* real, honest cap: no scroll in this v1, same as Reminders/Contacts */
        search_file_t *f = &search_files[search_matches[r]];
        if (r == search_sel) window_rect(16, y - 4, w - 32, 20, 0x00EDE6DC);
        char label[SEARCH_NAME_MAX + 1];
        int i = 0;
        while (f->name[i] && i < SEARCH_NAME_MAX - 1) { label[i] = f->name[i]; i++; }
        if (f->is_dir && i < SEARCH_NAME_MAX) label[i++] = '/';
        label[i] = 0;
        font_draw_string(label, 28, y, f->is_dir ? 0x00375A4A : 0x001C1C1E, -1);
    }
}

static void gui_launch_search(void) {
    int T = gui_app_dy();
    search_reload();
    search_query_len = 0;
    search_sel = 0;
    search_refilter();
    mouse_click_edge_sync();

    /* Chrome drawn once: titlebar + instructions never change while typing. */
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Search");
    font_draw_string("type to filter   up/down to pick   enter opens   esc closes", 20, T + 52, 0x0075726E, -1);

    for (;;) {
        search_draw_content();
        serial_puts("searchcontent\n"); /* discriminating marker for tools/checks/search-check.py */
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;
        if (k == KEY_CLICK) {
            /* Same real hit-test the Apps folder grid uses: app_cursor_x/y
               are full-screen logical coordinates (gui_app_mouse_tick),
               this app's own content draws through a viewport at
               app_view_x/app_view_y, so a click has to be converted before
               it can be compared against the list rows drawn above. A hit
               opens that result exactly like Enter does; a miss closes the
               app, the same "click anywhere else dismisses" contract every
               other list app here already has. */
            int click_vx = app_cursor_x - app_view_x, click_vy = app_cursor_y - app_view_y;
            int hit = -1;
            for (int r = 0; r < search_match_count; r++) {
                int y = T + 110 + r * 22;
                if (y + 20 > T + 104 + ((int)window_height() - 120 - T)) break;
                if (click_vx >= 16 && click_vx < (int)window_width() - 16 && click_vy >= y - 4 && click_vy < y + 16) { hit = r; break; }
            }
            if (hit < 0) return;
            search_sel = hit;
        } else if (k == KEY_UP) { if (search_sel > 0) search_sel--; continue; }
        else if (k == KEY_DOWN) { if (search_sel < search_match_count - 1) search_sel++; continue; }
        else if (k == '\b') { if (search_query_len > 0) { search_query_len--; search_refilter(); } continue; }
        else if (k >= 32 && k < 127 && search_query_len < SEARCH_QUERY_MAX - 1) { search_query[search_query_len++] = (char)k; search_refilter(); continue; }
        else if (k != KEY_ENTER && k != KEY_CLICK) continue;

        /* Enter or a click hit: open the selected match for real. */
        if (!search_match_count) continue;
        search_file_t *f = &search_files[search_matches[search_sel]];
        if (f->is_dir) {
            /* Real directory navigation: vfs_chdir is the same primitive
               the shell's own `cd` uses. A failed chdir (e.g. ramfs, which
               honestly refuses it, see drivers/ramfs.c) just leaves the
               listing where it was. */
            vfs_chdir(f->name);
            search_reload();
            search_query_len = 0;
            search_refilter();
        } else {
            search_open_file(f->name);
            window_clear(GUI_BG);
            gui_draw_app_titlebar("Search");
            font_draw_string("type to filter   up/down to pick   enter opens   esc closes", 20, T + 52, 0x0075726E, -1);
        }
    }
}
