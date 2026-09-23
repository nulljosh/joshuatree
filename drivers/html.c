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
            /* Named entities map to their closest ASCII, same rule as the
               UTF-8 skip below. Plan's page printed a raw "&middot;". An
               entity not in the table is dropped whole, never shown raw. */
            static const struct { const char *name; char ch; } ENT[] = {
                {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'},
                {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '},
                {"&middot;", '-'}, {"&bull;", '-'}, {"&ndash;", '-'}, {"&mdash;", '-'},
                {"&lsquo;", '\''}, {"&rsquo;", '\''}, {"&ldquo;", '"'}, {"&rdquo;", '"'},
                {"&hellip;", '.'},
            };
            int hit = 0;
            for (unsigned int i = 0; i < sizeof ENT / sizeof ENT[0]; i++) {
                if (starts_with(p, ENT[i].name)) {
                    out[pos++] = ENT[i].ch;
                    for (const char *n = ENT[i].name; *n; n++) p++;
                    hit = 1; break;
                }
            }
            if (hit) continue;
            const char *q = p + 1;
            while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '#') q++;
            if (*q == ';' && q - p <= 10) { p = q + 1; continue; }
        }
        /* Real bug, not a theory: found via a screendump that looked like
           memory corruption (a perfectly regular vertical-stripe pattern
           across the entire screen) for exactly the apps whose real copy
           uses more typographic punctuation (em dashes, curly quotes).
           Those are multi-byte UTF-8 sequences this extractor has no
           decoder for; copied through raw, each byte >= 0x80 lands on
           CP437's line-drawing/box block (─│┌┐└┘█▓ etc.), and repeated
           across a page's worth of body copy that reads as a stripe
           pattern, not garbled text. Real UTF-8 decoding (turning one
           multi-byte sequence into its closest ASCII equivalent) is a
           bigger, separate feature; skipping these bytes entirely instead
           of passing them straight to a font that was never going to
           render them correctly is the honest, scoped fix for now. */
        if ((unsigned char)*p >= 0x80) { p++; continue; }
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
