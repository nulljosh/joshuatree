#include "trash.h"
#include "vfs.h"

struct item {
    char name[TRASH_NAME_LEN];
    unsigned char data[TRASH_MAX_SIZE];
    unsigned int len;
    int used;
};

static struct item items[TRASH_MAX_ITEMS];

void trash_init(void) {
    for (int i = 0; i < TRASH_MAX_ITEMS; i++) items[i].used = 0;
}

int trash_count(void) {
    int n = 0;
    for (int i = 0; i < TRASH_MAX_ITEMS; i++) if (items[i].used) n++;
    return n;
}

/* Index over *live* items only, so callers can treat the trash as a list
   of 0..count-1 without knowing about the gaps a restore leaves behind. */
static int slot_of(int nth) {
    int n = 0;
    for (int i = 0; i < TRASH_MAX_ITEMS; i++) {
        if (!items[i].used) continue;
        if (n == nth) return i;
        n++;
    }
    return -1;
}

const char *trash_name(int i) {
    int s = slot_of(i);
    return s < 0 ? "" : items[s].name;
}

unsigned int trash_size(int i) {
    int s = slot_of(i);
    return s < 0 ? 0 : items[s].len;
}

int trash_put(const char *name, const void *data, unsigned int len) {
    if (len > TRASH_MAX_SIZE) return 0;
    int slot = -1;
    for (int i = 0; i < TRASH_MAX_ITEMS; i++) if (!items[i].used) { slot = i; break; }
    if (slot < 0) return 0;

    int k = 0;
    while (name[k] && k < TRASH_NAME_LEN - 1) { items[slot].name[k] = name[k]; k++; }
    items[slot].name[k] = 0;
    for (unsigned int j = 0; j < len; j++) items[slot].data[j] = ((const unsigned char *)data)[j];
    items[slot].len = len;
    items[slot].used = 1;
    return 1;
}

int trash_restore(int i) {
    int s = slot_of(i);
    if (s < 0) return 0;
    /* Only drop it from the trash once the filesystem has actually taken
       it: a failed write here used to be the one way a "restore" could
       lose the file permanently. */
    if (!vfs_write_file(items[s].name, items[s].data, items[s].len)) return 0;
    items[s].used = 0;
    return 1;
}

void trash_empty(void) {
    for (int i = 0; i < TRASH_MAX_ITEMS; i++) items[i].used = 0;
}
