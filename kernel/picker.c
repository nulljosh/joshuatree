/* The small tool picker. See picker.h. Spec: turing/training/smallpicker/ref.py (feats and pick). */
#include "picker.h"
#include "picker_weights.h"
#include "picker_golden.h"

static unsigned int fnv(unsigned char prefix, const unsigned char *s, int n) {
    unsigned int h = 2166136261u;
    h = (h ^ prefix) * 16777619u;
    for (int i = 0; i < n; i++) h = (h ^ s[i]) * 16777619u;
    return h;
}

/* Lowercase; keep a-z 0-9 ~ / . _ and space; every other byte becomes a space; pad one space each end. */
static int norm(const char *text, unsigned char *t, int max) {
    int n = 0;
    t[n++] = ' ';
    for (const unsigned char *p = (const unsigned char *)text; *p && n < max - 2; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '~' || c == '/' || c == '.' || c == '_' || c == ' ';
        t[n++] = ok ? c : ' ';
    }
    t[n++] = ' ';
    return n;
}

static void add(unsigned int h, unsigned char *seen, int *s) {
    h %= PICK_B;
    if (seen[h >> 3] & (1 << (h & 7))) return;       /* each feature counts once, like the reference's set */
    seen[h >> 3] |= 1 << (h & 7);
    for (unsigned int i = pick_off[h]; i < pick_off[h + 1]; i++) s[pick_cls[i]] += pick_w[i];
}

const char *picker_pick(const char *text) {
    unsigned char t[256], seen[PICK_B / 8] = {0};
    int s[PICK_K];
    for (int k = 0; k < PICK_K; k++) s[k] = pick_bias[k];
    int n = norm(text, t, sizeof t);
    for (int i = 0; i < n; ) {                        /* words: runs of non-space */
        if (t[i] == ' ') { i++; continue; }
        int j = i; while (j < n && t[j] != ' ') j++;
        add(fnv('w', t + i, j - i), seen, s);
        i = j;
    }
    for (int i = 0; i + 3 <= n; i++) add(fnv('c', t + i, 3), seen, s);
    int best = 0;
    for (int k = 1; k < PICK_K; k++) if (s[k] > s[best]) best = k;   /* first maximum wins, like the reference */
    return pick_tools[best];
}

int picker_selfcheck(int *total) {
    int ok = 0;
    for (int i = 0; i < PICK_GOLDEN_N; i++) ok += picker_pick(pick_golden[i].text) == pick_tools[pick_golden[i].want];
    *total = PICK_GOLDEN_N;
    return ok;
}
