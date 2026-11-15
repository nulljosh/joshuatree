/* Printing, step one (13.11.0): the Terminal's `print FILE HOST[:PORT]` turns a text file into a PDF and sends it to a
   network printer as one IPP Print-Job (RFC 8011), the way every AirPrint and IPP Everywhere printer takes a job. No
   driver: the printer does the rest. Discovery (mDNS) and Print in the apps are the next steps.

   pdf_make writes a plain PDF 1.4: US Letter, Helvetica 11 pt, 60 lines a page, lines cut at 90 characters, anything
   outside printable ASCII shown as '?', and ( ) \ escaped so a file cannot break out of its string. ipp_job wraps it in
   the request: charset, language, printer-uri, requesting-user-name, job-name and document-format, then the PDF.
   ipp_reply reads the answer, which is network data: it is walked by length with bounds on every field, and only the
   status code and the job-id are kept. tools/checks/arm64-print-check.py runs the builders on the host and the whole
   command in QEMU against a stub printer. */
#ifndef PRINT_HOST_TEST
#include "../../drivers/http.h"
extern void (*cmd_out)(const char *s);   /* cmd.c */
void *kmalloc(unsigned int n);
void kfree(void *p);
#endif

#define PDF_COLS 90
#define PDF_ROWS 60

static unsigned put(char *o, unsigned n, unsigned cap, const char *s) { while (*s && n < cap) o[n++] = *s++; return n; }
static unsigned putd(char *o, unsigned n, unsigned cap, unsigned v) {
    char t[12]; int k = 0;
    do t[k++] = (char)('0' + v % 10); while ((v /= 10) && k < 11);
    while (k && n < cap) o[n++] = t[--k];
    return n;
}
static unsigned put10(char *o, unsigned n, unsigned cap, unsigned v) {   /* an xref offset: ten digits */
    char t[10]; for (int k = 9; k >= 0; k--) { t[k] = (char)('0' + v % 10); v /= 10; }
    for (int k = 0; k < 10 && n < cap; k++) o[n++] = t[k];
    return n;
}
/* The text as a PDF in out; returns its length, or 0 if cap is too small or it runs past PDF_PAGES. Objects: 1 the
   catalog, 2 the page list (written last, once the pages are known), 3 the font, then each page and its content. */
#define PDF_PAGES 512
unsigned pdf_make(const char *text, unsigned len, char *out, unsigned cap) {
    static unsigned at[3 + 2 * PDF_PAGES];
    unsigned n = 0, i = 0, pages = 0;
    n = put(out, n, cap, "%PDF-1.4\n");
    at[0] = n; n = put(out, n, cap, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    at[2] = n; n = put(out, n, cap, "3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");
    do {
        if (pages == PDF_PAGES) return 0;
        unsigned pg = 4 + 2 * pages;
        at[pg - 1] = n;
        n = putd(out, n, cap, pg); n = put(out, n, cap, " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents ");
        n = putd(out, n, cap, pg + 1); n = put(out, n, cap, " 0 R >>\nendobj\n");
        at[pg] = n;
        n = putd(out, n, cap, pg + 1); n = put(out, n, cap, " 0 obj\n<< /Length ");
        unsigned lenat = n; n = put(out, n, cap, "0000000000 >>\nstream\n");
        unsigned s0 = n;
        n = put(out, n, cap, "BT /F1 11 Tf 13 TL 54 740 Td\n");
        for (unsigned r = 0; r < PDF_ROWS && i < len; r++) {   /* a line, cut at PDF_COLS; the rest goes on the next */
            n = put(out, n, cap, "(");
            for (unsigned c = 0; c < PDF_COLS && i < len && text[i] != '\n'; i++) {
                char ch = text[i];
                if (ch == '\r') continue;
                if (ch == '(' || ch == ')' || ch == '\\') { if (n < cap) out[n++] = '\\'; }
                else if (ch < 32 || ch > 126) ch = '?';
                if (n < cap) out[n++] = ch;
                c++;
            }
            if (i < len && text[i] == '\n') i++;
            n = put(out, n, cap, ") Tj T*\n");
        }
        n = put(out, n, cap, "ET\n");
        put10(out, lenat, cap, n - s0);
        n = put(out, n, cap, "endstream\nendobj\n");
        pages++;
    } while (i < len);
    at[1] = n; n = put(out, n, cap, "2 0 obj\n<< /Type /Pages /Count "); n = putd(out, n, cap, pages); n = put(out, n, cap, " /Kids [");
    for (unsigned p = 0; p < pages; p++) { n = putd(out, n, cap, 4 + 2 * p); n = put(out, n, cap, " 0 R "); }
    n = put(out, n, cap, "] >>\nendobj\n");
    unsigned objs = 3 + 2 * pages, xref = n;
    n = put(out, n, cap, "xref\n0 "); n = putd(out, n, cap, objs + 1); n = put(out, n, cap, "\n0000000000 65535 f \n");
    for (unsigned k = 0; k < objs; k++) { n = put10(out, n, cap, at[k]); n = put(out, n, cap, " 00000 n \n"); }
    n = put(out, n, cap, "trailer\n<< /Size "); n = putd(out, n, cap, objs + 1); n = put(out, n, cap, " /Root 1 0 R >>\nstartxref\n");
    n = putd(out, n, cap, xref); n = put(out, n, cap, "\n%%EOF\n");
    return n < cap ? n : 0;
}
static unsigned ipp_attr(unsigned char *o, unsigned n, unsigned cap, unsigned char tag, const char *name, const char *value) {
    unsigned nl = 0, vl = 0;
    while (name[nl]) nl++;
    while (value[vl]) vl++;
    if (n + 5 + nl + vl > cap) return cap + 1;
    o[n++] = tag; o[n++] = (unsigned char)(nl >> 8); o[n++] = (unsigned char)nl;
    for (unsigned k = 0; k < nl; k++) o[n++] = (unsigned char)name[k];
    o[n++] = (unsigned char)(vl >> 8); o[n++] = (unsigned char)vl;
    for (unsigned k = 0; k < vl; k++) o[n++] = (unsigned char)value[k];
    return n;
}
/* A Print-Job request for the PDF already at doc (doclen bytes), its header written in front of it at out; returns the
   whole length, or 0 if it does not fit. */
unsigned ipp_job(const char *uri, const char *user, const char *job, const char *doc, unsigned doclen, unsigned char *out, unsigned cap) {
    unsigned n = 0;
    if (cap < 9) return 0;
    out[n++] = 2; out[n++] = 0;      /* IPP 2.0 */
    out[n++] = 0; out[n++] = 2;      /* Print-Job */
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;   /* request-id 1 */
    out[n++] = 0x01;                 /* operation-attributes */
    n = ipp_attr(out, n, cap, 0x47, "attributes-charset", "utf-8");
    n = ipp_attr(out, n, cap, 0x48, "attributes-natural-language", "en");
    n = ipp_attr(out, n, cap, 0x45, "printer-uri", uri);
    n = ipp_attr(out, n, cap, 0x42, "requesting-user-name", user[0] ? user : "joshuatree");
    n = ipp_attr(out, n, cap, 0x42, "job-name", job);
    n = ipp_attr(out, n, cap, 0x49, "document-format", "application/pdf");
    if (n + 1 + doclen > cap) return 0;
    out[n++] = 0x03;                 /* end-of-attributes */
    for (unsigned k = 0; k < doclen; k++) out[n++] = (unsigned char)doc[k];
    return n;
}
/* The printer's answer: its status code (0x0000 to 0x00FF is success), and the job-id if it sent one. -1 when the
   reply is not IPP at all. */
int ipp_reply(const unsigned char *r, unsigned len, unsigned *job_id) {
    *job_id = 0;
    if (len < 9 || r[0] < 1 || r[0] > 2) return -1;
    int status = (r[2] << 8) | r[3];
    unsigned i = 8, last = 0;
    char name[16]; unsigned nl = 0;
    while (i < len) {
        unsigned char tag = r[i++];
        if (tag == 0x03) break;
        if (tag < 0x10) continue;               /* a group delimiter */
        if (i + 2 > len) break;
        unsigned l = ((unsigned)r[i] << 8) | r[i + 1]; i += 2;
        if (i + l + 2 > len) break;
        if (l) { nl = 0; for (unsigned k = 0; k < l && k < sizeof name - 1; k++) name[nl++] = (char)r[i + k]; name[nl] = 0; last = l; }
        i += l;
        unsigned vl = ((unsigned)r[i] << 8) | r[i + 1]; i += 2;
        if (i + vl > len) break;
        if (tag == 0x21 && vl == 4 && last == 6 && nl == 6 && name[0] == 'j' && name[1] == 'o' && name[2] == 'b' && name[3] == '-' && name[4] == 'i' && name[5] == 'd')
            *job_id = ((unsigned)r[i] << 24) | ((unsigned)r[i + 1] << 16) | ((unsigned)r[i + 2] << 8) | r[i + 3];
        i += vl;
    }
    return status;
}
#ifndef PRINT_HOST_TEST
static void out_hex4(int v) { char t[5]; for (int k = 3; k >= 0; k--) { int d = v & 15; t[k] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v >>= 4; } t[4] = 0; cmd_out(t); }
static void out_dec(unsigned v) { char t[12]; int k = 11; t[k] = 0; do t[--k] = (char)('0' + v % 10); while ((v /= 10) && k); cmd_out(t + k); }
/* cmd.c's `print`: the file's text is already read; host is a name or dotted quad, port 631 unless given. */
void print_text(const char *host, unsigned short port, const char *user, const char *job, const char *text, unsigned len) {
    unsigned cap = len * 2 + 4096 + (len / (PDF_COLS * PDF_ROWS) + 1) * 400;
    char *pdf = kmalloc(cap);
    unsigned char *req = pdf ? kmalloc(cap + 1024) : 0;
    unsigned char reply[1024];
    if (!pdf || !req) { cmd_out("print: not enough memory for that file\n"); if (pdf) kfree(pdf); return; }
    unsigned pl = pdf_make(text, len, pdf, cap);
    char uri[96]; unsigned u = 0;
    u = put(uri, u, sizeof uri - 1, "ipp://"); u = put(uri, u, sizeof uri - 1, host); uri[u++] = ':';
    u = putd(uri, u, sizeof uri - 1, port); u = put(uri, u, sizeof uri - 1, "/ipp/print"); uri[u] = 0;
    unsigned rl = pl ? ipp_job(uri, user, job, pdf, pl, req, cap + 1024) : 0;
    kfree(pdf);
    if (!rl) { cmd_out("print: that file is too big to print\n"); kfree(req); return; }
    http_post_set_type("application/ipp");
    int got = http_post_timeout(host, "/ipp/print", port, (const char *)req, rl, reply, sizeof reply, 1500);
    kfree(req);
    if (got < 0) { cmd_out("print: no printer answered at "); cmd_out(host); cmd_out("\n"); return; }
    unsigned id; int st = ipp_reply(reply, (unsigned)got, &id);
    if (st < 0) { cmd_out("print: "); cmd_out(host); cmd_out(" answered, but not as a printer\n"); return; }
    if (st > 0xFF) { cmd_out("print: the printer said no (0x"); out_hex4(st); cmd_out(")\n"); return; }
    cmd_out("print: sent to "); cmd_out(host);
    if (id) { cmd_out(", job "); out_dec(id); }
    cmd_out("\n");
}
#endif
