#include "html.h"

static int starts_with(const char *p, const char *needle) {
    while (*needle) { if (*p != *needle) return 0; p++; needle++; }
    return 1;
}

unsigned int html_to_text(const char *html, char *out, unsigned int maxlen) {
    unsigned int pos = 0;
    const char *p = html;
    while (*p && pos < maxlen - 1) {
        if (*p == '<') {
            /* <script>/<style> element CONTENT isn't text, a plain <>-strip
               would print raw CSS/JS as if it were the page's words (found
               exactly that way: example.com's stylesheet showed up in the
               extracted text). Skip to the matching close tag first, then
               fall through to the normal single-tag skip for that tag. */
            if (starts_with(p, "<script")) while (*p && !starts_with(p, "</script>")) p++;
            else if (starts_with(p, "<style")) while (*p && !starts_with(p, "</style>")) p++;

            while (*p && *p != '>') p++;
            if (*p) p++;
            continue;
        }
        if (*p == '&') {
            if      (starts_with(p, "&lt;"))   { out[pos++] = '<';  p += 4; continue; }
            else if (starts_with(p, "&gt;"))   { out[pos++] = '>';  p += 4; continue; }
            else if (starts_with(p, "&amp;"))  { out[pos++] = '&';  p += 5; continue; }
            else if (starts_with(p, "&quot;")) { out[pos++] = '"';  p += 6; continue; }
            else if (starts_with(p, "&#39;"))  { out[pos++] = '\''; p += 5; continue; }
        }
        out[pos++] = *p++;
    }
    out[pos] = 0;
    return pos;
}
