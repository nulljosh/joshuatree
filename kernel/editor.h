/* kernel/editor.h: what is left of the old in-kernel Notes. The editor and
   browser moved to ring 3 (user/notes.c, a compositor window). This file keeps
   the pointer position the compositor shares with ring-3 windows, the
   editor_fonts.h glyph tables the GUI text path reads, and the one-time
   NOTES.TXT-into-the-default-folder migration notes_ring3_open runs before
   the app starts. */
#include "editor_fonts.h"

int editor_mouse_x = 400, editor_mouse_y = 300;

#define NOTES_DIR "NOTES"
#define NOTES_DEFAULT_FOLDER "NOTES"

static int notes_folders_supported = -1; /* -1 unknown, 0 ramfs (flat), 1 real disk */
static int notes_migrated = 0;

/* Always returns to VFS root. Safe from any depth this app ever reaches
   (root, NOTES/, or NOTES/<folder>/): fat_chdir("..") at root is a no-op
   that still returns success, so two calls always land at root. */
static void notes_goto_root(void) { vfs_chdir(".."); vfs_chdir(".."); }

/* Enters (creating if needed) the NOTES/ container directory. Returns 0
   on ramfs or on real disk failure (full root directory, etc). */
static int notes_enter_container(void) {
    notes_goto_root();
    if (vfs_chdir(NOTES_DIR)) return 1;
    if (vfs_mkdir(NOTES_DIR) && vfs_chdir(NOTES_DIR)) return 1;
    return 0;
}

static int notes_enter_folder(const char *name) {
    if (!notes_enter_container()) return 0;
    if (vfs_chdir(name)) return 1;
    if (vfs_mkdir(name) && vfs_chdir(name)) return 1;
    return 0;
}

static int notes_check_support(void) {
    if (notes_folders_supported >= 0) return notes_folders_supported;
    notes_folders_supported = notes_enter_folder(NOTES_DEFAULT_FOLDER) ? 1 : 0;
    notes_goto_root();
    return notes_folders_supported;
}

static int notes_list_tmp_count = 0;
static int notes_max_idx = 0; /* highest N<digits>.TXT number notes_count_cb saw */
static void notes_count_cb(const char *name, unsigned int size, int is_dir) {
    (void)size;
    if (is_dir || name[0] == '.') return;
    notes_list_tmp_count++;
    if (name[0] == 'N') { int v = 0, i = 1; while (name[i] >= '0' && name[i] <= '9' && i < 9) v = v * 10 + (name[i++] - '0'); if (v > notes_max_idx) notes_max_idx = v; }
}

/* One-time: NOTES.TXT's content must never be lost. On a real disk it
   moves into the default folder's first note file; NOTES.TXT itself is
   only deleted once that write is confirmed. On ramfs (no folders) it's
   left exactly where it is -- it already IS the one note the flat
   fallback below shows. Skips entirely (and safely: nothing to do) once
   the default folder already has a note in it, so this never re-fires
   or clobbers real notes a user has since made. */
static void notes_migrate_legacy(void) {
    if (notes_migrated) return;
    notes_migrated = 1;
    notes_goto_root();
    static char legacy[4096];
    int n = vfs_read_file("NOTES.TXT", legacy, sizeof(legacy) - 1);
    if (n <= 0) { notes_goto_root(); return; }
    if (!notes_check_support()) { notes_goto_root(); return; }
    if (!notes_enter_folder(NOTES_DEFAULT_FOLDER)) { notes_goto_root(); return; }
    notes_list_tmp_count = 0;
    vfs_list(notes_count_cb);
    if (notes_list_tmp_count == 0) {
        if (vfs_write_file("N0000001.TXT", legacy, (unsigned int)n)) {
            notes_goto_root();
            vfs_delete("NOTES.TXT");
            notes_goto_root();
            return;
        }
    }
    notes_goto_root();
}

void notes_ring3_launch(void);
void notes_ring3_open(void) { notes_migrate_legacy(); notes_ring3_launch(); } /* ring 3 (user/notes.c) */
