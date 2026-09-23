#ifndef HTML_H
#define HTML_H
/* Tag-soup, not a parser: strips everything between < and >, decodes the
   common named entities to ASCII (drops unknown ones), copies the rest.
   No DOM, no CSS, no nesting awareness, Lynx-level v1, matches the
   roadmap's own scope for this item. Returns the number of text bytes
   written (excluding the trailing NUL). */
unsigned int html_to_text(const char *html, char *out, unsigned int maxlen);

#define HTML_MAX_LINKS 16
#define HTML_HREF_LEN  128
#define HTML_LINK_TEXT_LEN 48
struct html_link {
    char href[HTML_HREF_LEN];
    char text[HTML_LINK_TEXT_LEN];
};

/* Finds `<a href="...">visible text</a>` occurrences (lowercase tag only,
   matches real-world markup, not a general case-insensitive HTML scanner).
   Fills links_out up to max_links, returns how many were found. */
unsigned int html_extract_links(const char *html, struct html_link *links_out, unsigned int max_links);
#endif
