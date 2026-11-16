/* Printing, step one (13.11.0): the Terminal's `print FILE HOST[:PORT]` turns a text file into a PDF and sends it to a
   network printer as one IPP Print-Job (RFC 8011), the way every AirPrint and IPP Everywhere printer takes a job. No
   driver: the printer does the rest. Discovery (mDNS) and Print in the apps are the next steps.

   pdf_make writes a plain PDF 1.4: US Letter, Helvetica 11 pt, 52 lines a page, lines cut at 90 characters, anything
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
unsigned long heap_mark(void);   /* main.c: kfree is a no-op on the bump heap, so a job rolls the heap back when it ends */
void heap_release(unsigned long m);
#endif

#define PDF_COLS 90
#define PDF_ROWS 52   /* 740 pt down by 13: the last line at 77 pt, inside any printer's margin */

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
/* 13.12.0, finding a printer: one mDNS question for _ipp._tcp.local (RFC 6762/6763), asked from our own port so the
   printer answers us directly, and the first answer read for its name (PTR), port and host (SRV), address (A) and the
   job path (TXT rp=). The answer is network data: every name is read with bounds, compression pointers may only point
   backwards and at most 16 deep, and a record that runs past the end ends the walk. */
unsigned mdns_query_svc(unsigned char *q, const char *svc) {   /* 13.14.0: any _svc._tcp.local, "_ipp" or "_uscan" */
    static const unsigned char head[] = { 0x13, 0x37, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
    static const unsigned char tail[] = { 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0, 0, 12, 0x80, 1 };   /* PTR, IN, unicast reply */
    unsigned n = 0, l = 0;
    for (unsigned i = 0; i < sizeof head; i++) q[n++] = head[i];
    while (svc[l] && l < 15) l++;
    q[n++] = (unsigned char)l;
    for (unsigned i = 0; i < l; i++) q[n++] = (unsigned char)svc[i];
    for (unsigned i = 0; i < sizeof tail; i++) q[n++] = tail[i];
    return n;
}
unsigned mdns_query(unsigned char *q) { return mdns_query_svc(q, "_ipp"); }
static unsigned dns_name(const unsigned char *r, unsigned len, unsigned i, char *out, unsigned cap, unsigned *after) {   /* 0 when bad */
    unsigned o = 0, jumps = 0, end = 0;
    while (i < len) {
        unsigned l = r[i];
        if ((l & 0xC0) == 0xC0) {
            if (i + 1 >= len || ++jumps > 16) return 0;
            unsigned to = ((l & 0x3F) << 8) | r[i + 1];
            if (to >= i) return 0;                    /* only backwards: no loops */
            if (!end) end = i + 2;
            i = to; continue;
        }
        if (l & 0xC0) return 0;
        if (!l) { if (!end) end = i + 1; if (out && cap) out[o < cap ? o : cap - 1] = 0; *after = end; return 1; }
        if (i + 1 + l > len) return 0;
        if (out && o && o < cap - 1) out[o++] = '.';
        for (unsigned k = 0; k < l; k++) if (out && o < cap - 1) { char c = (char)r[i + 1 + k]; out[o++] = c >= 32 && c < 127 ? c : '?'; }
        i += 1 + l;
    }
    return 0;
}
/* The first printer in an mDNS answer: its name (the PTR's instance, up to the first dot), address, port and path. */
/* key is the TXT key that holds the path ("rp" for IPP, "rs" for eSCL) and dflt the path when there is none. */
int mdns_parse_svc(const unsigned char *r, unsigned len, char *name, unsigned ncap, unsigned *ip, unsigned *port, char *path, unsigned pcap, const char *key, const char *dflt) {
    *ip = 0; *port = 0; name[0] = 0; path[0] = 0;
    if (len < 12 || !(r[2] & 0x80)) return 0;          /* not an answer */
    unsigned qd = (r[4] << 8) | r[5], rr = ((r[6] << 8) | r[7]) + ((r[8] << 8) | r[9]) + ((r[10] << 8) | r[11]), i = 12, after;
    char tmp[128];
    for (unsigned k = 0; k < qd && k < 8; k++) { if (!dns_name(r, len, i, 0, 0, &after) || after + 4 > len) return 0; i = after + 4; }
    for (unsigned k = 0; k < rr && k < 32; k++) {
        if (!dns_name(r, len, i, 0, 0, &after) || after + 10 > len) break;
        unsigned type = (r[after] << 8) | r[after + 1], dl = (r[after + 8] << 8) | r[after + 9], d = after + 10;
        if (d + dl > len) break;
        if (type == 12 && !name[0] && dns_name(r, d + dl, d, tmp, sizeof tmp, &after)) {
            unsigned o = 0; while (tmp[o] && tmp[o] != '.' && o < ncap - 1) { name[o] = tmp[o]; o++; } name[o] = 0;
        } else if (type == 33 && dl >= 7 && !*port) *port = (r[d + 4] << 8) | r[d + 5];
        else if (type == 1 && dl == 4 && !*ip) *ip = ((unsigned)r[d] << 24) | ((unsigned)r[d + 1] << 16) | ((unsigned)r[d + 2] << 8) | r[d + 3];
        else if (type == 16 && !path[0]) {
            for (unsigned t = d; t < d + dl; ) {
                unsigned sl = r[t];
                if (t + 1 + sl > d + dl) break;
                if (sl > 3 && r[t + 1] == key[0] && r[t + 2] == key[1] && r[t + 3] == '=') {
                    unsigned o = 0; for (unsigned c = 4; c <= sl && o < pcap - 1; c++) { char ch = (char)r[t + c]; if (ch > 32 && ch < 127) path[o++] = ch; } path[o] = 0;
                }
                t += 1 + sl;
            }
        }
        i = d + dl;
    }
    if (!path[0]) { unsigned o = 0; while (dflt[o] && o < pcap - 1) { path[o] = dflt[o]; o++; } path[o] = 0; }
    return *ip && *port && name[0];
}
int mdns_parse(const unsigned char *r, unsigned len, char *name, unsigned ncap, unsigned *ip, unsigned *port, char *path, unsigned pcap) {
    return mdns_parse_svc(r, len, name, ncap, ip, port, path, pcap, "rp", "ipp/print");
}
#ifndef PRINT_HOST_TEST
static void out_hex4(int v) { char t[5]; for (int k = 3; k >= 0; k--) { int d = v & 15; t[k] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v >>= 4; } t[4] = 0; cmd_out(t); }
static void out_dec(unsigned v) { char t[12]; int k = 11; t[k] = 0; do t[--k] = (char)('0' + v % 10); while ((v /= 10) && k); cmd_out(t + k); }
/* cmd.c's `print`: the file's text is already read; host is a name or dotted quad, port 631 unless given. */
static int print_text_in(const char *host, unsigned short port, const char *path, const char *user, const char *job, const char *text, unsigned len);
int print_text(const char *host, unsigned short port, const char *path, const char *user, const char *job, const char *text, unsigned len) {   /* 1 when the printer took it */
    unsigned long m = heap_mark();   /* 13.14.0: the PDF, the request and http.c's reply buffer, all gone when the job is */
    int ok = print_text_in(host, port, path, user, job, text, len);
    heap_release(m);
    return ok;
}
static int print_text_in(const char *host, unsigned short port, const char *path, const char *user, const char *job, const char *text, unsigned len) {
    unsigned lines = 1; for (unsigned k = 0; k < len; k++) lines += text[k] == '\n';
    unsigned cap = len * 2 + lines * 12 + 4096 + (lines / PDF_ROWS + len / (PDF_COLS * PDF_ROWS) + 2) * 400;   /* each line costs about 10 bytes of PDF */
    char *pdf = kmalloc(cap);
    unsigned char *req = pdf ? kmalloc(cap + 1024) : 0;
    unsigned char reply[1024];
    if (!pdf || !req) { cmd_out("print: not enough memory for that file\n"); if (pdf) kfree(pdf); return 0; }
    unsigned pl = pdf_make(text, len, pdf, cap);
    char uri[96]; unsigned u = 0;
    u = put(uri, u, sizeof uri - 1, "ipp://"); u = put(uri, u, sizeof uri - 1, host); uri[u++] = ':';
    u = putd(uri, u, sizeof uri - 1, port); uri[u++] = '/'; u = put(uri, u, sizeof uri - 1, path); uri[u] = 0;
    char hpath[64]; unsigned h = 0; hpath[h++] = '/'; h = put(hpath, h, sizeof hpath - 1, path); hpath[h] = 0;
    unsigned rl = pl ? ipp_job(uri, user, job, pdf, pl, req, cap + 1024) : 0;
    kfree(pdf);
    if (!rl) { cmd_out("print: that file is too big to print\n"); kfree(req); return 0; }
    http_post_set_type("application/ipp");
    int got = http_post_timeout(host, hpath, port, (const char *)req, rl, reply, sizeof reply, 1500);
    kfree(req);
    if (got < 0) { cmd_out("print: no printer answered at "); cmd_out(host); cmd_out("\n"); return 0; }
    unsigned id; int st = ipp_reply(reply, (unsigned)got, &id);
    if (st < 0) { cmd_out("print: "); cmd_out(host); cmd_out(" answered, but not as a printer\n"); return 0; }
    if (st > 0xFF) { cmd_out("print: the printer said no (0x"); out_hex4(st); cmd_out(")\n"); return 0; }
    cmd_out("print: sent to "); cmd_out(host);
    if (id) { cmd_out(", job "); out_dec(id); }
    cmd_out("\n");
    return 1;
}
#endif

#ifndef PRINT_HOST_TEST
int udp_group_ask(unsigned int group, unsigned short port, unsigned short src_port, const void *probe, unsigned int len,
                  void *reply, unsigned int max, unsigned int *from_ip, unsigned int wait_ticks);
static int svc_find(const char *svc, const char *key, const char *dflt, char *name, unsigned ncap, char *host, unsigned hcap, unsigned *port, char *path, unsigned pcap) {
    unsigned char q[64], r[1500]; unsigned from, ip;
    int got = udp_group_ask(0xE00000FBu, 5353, 5354, q, mdns_query_svc(q, svc), r, sizeof r, &from, 300);
    if (got <= 0 || !mdns_parse_svc(r, (unsigned)got, name, ncap, &ip, port, path, pcap, key, dflt)) return 0;
    unsigned o = 0;
    for (int b = 3; b >= 0; b--) { o = putd(host, o, hcap - 1, (ip >> (8 * b)) & 255); if (b && o < hcap - 1) host[o++] = '.'; }
    host[o] = 0;
    return 1;
}
/* `printers`, and `print FILE` with no printer named: ask the network once, three seconds. host gets a dotted quad. */
int print_find(char *name, unsigned ncap, char *host, unsigned hcap, unsigned *port, char *path, unsigned pcap) {
    return svc_find("_ipp", "rp", "ipp/print", name, ncap, host, hcap, port, path, pcap);
}
/* 13.14.0: `scan` with no scanner named: the first _uscan._tcp answer, its eSCL root from TXT rs= (eSCL when none). */
int scan_find(char *name, unsigned ncap, char *host, unsigned hcap, unsigned *port, char *path, unsigned pcap) {
    return svc_find("_uscan", "rs", "eSCL", name, ncap, host, hcap, port, path, pcap);
}
#endif

/* 13.14.0, scanning: eSCL, the HTTP and XML protocol AirPrint and Mopria scanners speak. POST ScanSettings to
   ROOT/ScanJobs, the scanner answers 201 with the job in Location, GET LOCATION/NextDocument returns the page as a
   JPEG (503 while the lamp warms up, so it is asked again). The picture lands in PHOTOS/ as the next SCANnnnn.JPG,
   where Photos shows it. The reply is network data: only its status, its Location path and a JPEG's first bytes are
   trusted, and a body that fills the buffer is called too big, never saved cut short. */
unsigned escl_settings(char *o, unsigned cap, unsigned dpi) {
    unsigned n = 0;
    n = put(o, n, cap, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<scan:ScanSettings xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\">\n"
        "<pwg:Version>2.6</pwg:Version>\n<scan:Intent>Photo</scan:Intent>\n<pwg:InputSource>Platen</pwg:InputSource>\n"
        "<scan:ColorMode>RGB24</scan:ColorMode>\n<scan:XResolution>");
    n = putd(o, n, cap, dpi); n = put(o, n, cap, "</scan:XResolution>\n<scan:YResolution>");
    n = putd(o, n, cap, dpi); n = put(o, n, cap, "</scan:YResolution>\n<pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat>\n"
        "<scan:DocumentFormatExt>image/jpeg</scan:DocumentFormatExt>\n</scan:ScanSettings>\n");
    return n < cap ? n : 0;
}
int escl_is_jpeg(const unsigned char *d, unsigned n) { return n > 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF; }
/* LOCATION/NextDocument from the Location path; 0 when it is empty, not absolute, or too long. */
unsigned escl_next(const char *loc, char *out, unsigned cap) {
    if (loc[0] != '/') return 0;
    unsigned n = put(out, 0, cap, loc);
    if (n && out[n - 1] == '/') n--;
    n = put(out, n, cap, "/NextDocument");
    if (n >= cap) return 0;
    out[n] = 0;
    return n;
}
#ifndef PRINT_HOST_TEST
int photo_save_next(const void *d, unsigned n, char *name, unsigned ncap);   /* ufile.c */
unsigned long cnt_hz(void);
unsigned long cnt_now(void);
#define SCAN_MAX (2u << 20)
static int scan_in(const char *host, unsigned short port, const char *root);
int scan_to_photos(const char *host, unsigned short port, const char *root) {   /* 1 when a picture was saved */
    unsigned long m = heap_mark();   /* the 2 MB picture and http.c's 2 MB reply buffer: back to the heap after every scan */
    int ok = scan_in(host, port, root);
    heap_release(m);
    return ok;
}
static int scan_in(const char *host, unsigned short port, const char *root) {
    char xml[1024], path[96], next[128], small[256], name[16];
    unsigned xl = escl_settings(xml, sizeof xml, 150), p = 0;
    path[p++] = '/'; p = put(path, p, sizeof path - 1, root); p = put(path, p, sizeof path - 1, "/ScanJobs"); path[p] = 0;
    http_post_set_type("text/xml");
    int got = http_post_timeout(host, path, port, xml, xl, small, sizeof small, 1500);
    if (got < 0) { cmd_out("scan: no scanner answered at "); cmd_out(host); cmd_out("\n"); return 0; }
    int st = http_last_status();
    if (st != 201) { cmd_out("scan: the scanner said no (HTTP "); out_dec((unsigned)st); cmd_out(")\n"); return 0; }
    if (!escl_next(http_last_location(), next, sizeof next)) { cmd_out("scan: the scanner did not say where the scan is\n"); return 0; }
    unsigned char *img = kmalloc(SCAN_MAX);
    if (!img) { cmd_out("scan: not enough memory for a scan\n"); return 0; }
    cmd_out("scan: scanning\n");
    for (int tries = 0; tries < 30; tries++) {   /* about 30 s for a lamp to warm up */
        got = http_get_timeout(host, next, port, img, SCAN_MAX, 3000);
        st = http_last_status();
        if (got < 0 || st != 503) break;
        unsigned long until = cnt_now() + cnt_hz();
        while (cnt_now() < until) { }
    }
    int ok = 0;
    if (got < 0) { cmd_out("scan: the scanner stopped answering\n"); }
    else if (st != 200) { cmd_out("scan: no page came back (HTTP "); out_dec((unsigned)st); cmd_out(")\n"); }
    else if ((unsigned)got >= SCAN_MAX) cmd_out("scan: that scan is over 2 MB\n");
    else if (!escl_is_jpeg(img, (unsigned)got)) cmd_out("scan: the scanner sent something that is not a JPEG\n");
    else if (!photo_save_next(img, (unsigned)got, name, sizeof name)) cmd_out("scan: could not save it to PHOTOS on the card\n");
    else { cmd_out("scan: saved PHOTOS/"); cmd_out(name); cmd_out(", "); out_dec(((unsigned)got + 1023) / 1024); cmd_out(" KB\n"); ok = 1; }
    kfree(img);
    return ok;
}
#endif
