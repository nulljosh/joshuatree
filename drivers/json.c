#include "json.h"

static int starts_with(const char *p, const char *needle) {
    while (*needle) { if (*p != *needle) return 0; p++; needle++; }
    return 1;
}

unsigned int json_extract_string(const char *json, const char *key, char *out, unsigned int maxlen) {
    /* security pass: every call site today passes a real sizeof(), but
       maxlen==0 would underflow the `maxlen - 1` bound checks below
       (unsigned) into a huge value and turn this into an unbounded write
       into `out` -- untrusted network JSON (weather/geo/chat replies)
       lands here, so this is worth guarding even with no current caller
       that triggers it. */
    if (maxlen == 0) return 0;
    /* build the "key": pattern to search for (with the opening quote) */
    char pattern[64];
    unsigned int pn = 0;
    pattern[pn++] = '"';
    for (const char *k = key; *k && pn < sizeof(pattern) - 3; k++) pattern[pn++] = *k;
    pattern[pn++] = '"'; pattern[pn++] = ':';
    pattern[pn] = 0;

    const char *p = json;
    const char *found = 0;
    for (; *p; p++) {
        if (starts_with(p, pattern)) { found = p + pn; break; }
    }
    if (!found) return 0;

    p = found;
    while (*p == ' ') p++;
    if (*p != '"') return 0; /* value isn't a string (number/bool/null/object), out of scope */
    p++;

    unsigned int pos = 0;
    while (*p && *p != '"' && pos < maxlen - 1) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': out[pos++] = '\n'; break;
                case 't': out[pos++] = '\t'; break;
                case 'r': out[pos++] = '\r'; break;
                case '"': out[pos++] = '"'; break;
                case '\\': out[pos++] = '\\'; break;
                default: out[pos++] = *p; break; /* unicode escapes etc: pass through raw, good enough here */
            }
            if (*p) p++;
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = 0;
    return pos;
}

unsigned int json_extract_number_text(const char *json, const char *key, char *out, unsigned int maxlen) {
    if (maxlen == 0) return 0; /* same unsigned-underflow guard as json_extract_string above */
    char pattern[64];
    unsigned int pn = 0;
    pattern[pn++] = '"';
    for (const char *k = key; *k && pn < sizeof(pattern) - 3; k++) pattern[pn++] = *k;
    pattern[pn++] = '"'; pattern[pn++] = ':';
    pattern[pn] = 0;

    const char *p = json;
    const char *found = 0;
    for (; *p; p++) {
        if (starts_with(p, pattern)) { found = p + pn; break; }
    }
    if (!found) return 0;

    p = found;
    while (*p == ' ') p++;
    unsigned int pos = 0;
    int digits = 0;
    while (pos < maxlen - 1 && (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9'))) {
        if (*p >= '0' && *p <= '9') digits = 1;
        out[pos++] = *p++;
    }
    out[pos] = 0;
    if (!digits) { out[0] = 0; return 0; } /* "lat":"x" or "lat":null: not a number, out of scope */
    return pos;
}

unsigned int json_escape(const char *s, char *out, unsigned int maxlen) {
    if (maxlen < 2) { if (maxlen == 1) out[0] = 0; return 0; } /* same unsigned-underflow guard, `maxlen - 2` below */
    unsigned int pos = 0;
    for (; *s && pos < maxlen - 2; s++) {
        if (*s == '"' || *s == '\\') { if (pos < maxlen - 2) out[pos++] = '\\'; out[pos++] = *s; }
        else if (*s == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
        else if ((unsigned char)*s < 0x20) { /* other control chars: drop, not worth a \uXXXX encoder here */ }
        else out[pos++] = *s;
    }
    out[pos] = 0;
    return pos;
}
