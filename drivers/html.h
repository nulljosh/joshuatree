#ifndef HTML_H
#define HTML_H
/* Tag-soup, not a parser: strips everything between < and >, decodes the
   five entities anything actually uses, copies the rest through as-is.
   No DOM, no CSS, no nesting awareness, Lynx-level v1, matches the
   roadmap's own scope for this item. Returns the number of text bytes
   written (excluding the trailing NUL). */
unsigned int html_to_text(const char *html, char *out, unsigned int maxlen);
#endif
