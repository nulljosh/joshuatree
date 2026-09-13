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

/* Double-quoted href="..." only, real-world markup overwhelmingly uses it;
   single-quoted attributes are a known gap, not silently mishandled. */
unsigned int html_extract_links(const char *html, struct html_link *links_out, unsigned int max_links) {
    unsigned int count = 0;
    const char *p = html;
    while (*p && count < max_links) {
        if (!starts_with(p, "<a ") && !starts_with(p, "<a>")) { p++; continue; }

        const char *tag_end = p;
        while (*tag_end && *tag_end != '>') tag_end++;
        const char *href_pos = 0;
        for (const char *q = p; q < tag_end; q++) {
            if (starts_with(q, "href=\"")) { href_pos = q + 6; break; }
        }

        struct html_link *link = &links_out[count];
        link->href[0] = 0;
        link->text[0] = 0;
        if (href_pos) {
            unsigned int hi = 0;
            while (*href_pos && *href_pos != '"' && hi < HTML_HREF_LEN - 1) link->href[hi++] = *href_pos++;
            link->href[hi] = 0;
        }
        p = *tag_end ? tag_end + 1 : tag_end;

        unsigned int ti = 0;
        while (*p && !starts_with(p, "</a>")) {
            if (*p == '<') { while (*p && *p != '>') p++; if (*p) p++; continue; }
            if (ti < HTML_LINK_TEXT_LEN - 1) link->text[ti++] = *p;
            p++;
        }
        link->text[ti] = 0;
        if (starts_with(p, "</a>")) p += 4;

        if (href_pos) count++; /* only a real link if it actually had an href */
    }
    return count;
}
