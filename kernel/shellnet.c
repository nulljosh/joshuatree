/* The shell's network commands: lspci, ifconfig, netscan, web (the text browser), serve and serveapp (the embedded
 apps over HTTP), build (an LLM-made page, served), and the tcpmatchtest and rxclamptest regressions for net.c and the
 RTL8139 driver. Moved out of kernel.c's run() in 4.19.1; run() hands every line here after the file commands. */
#include "libc.h"
#include "console.h"
#include "serial.h"
#include "pci.h"
#include "kheap.h"
#include "window.h"
#include "font.h"
#include "net.h"
#include "http.h"
#include "html.h"
#include "json.h"
#include "rtl8139.h"
#include "app_weather.h"
#include "app_curbfind.h"
#include "app_keyrate.h"
#include "app_bookrank.h"
#include "app_quotestreak.h"
#include "app_tonchi.h"
#include "app_toroid.h"
#include "app_hikko.h"
#include "app_fieldbook.h"
#include "selftest.h"

/* LLMs habitually wrap generated code in a ```html ... ``` fence even when
   told not to. Strips a leading ``` line and a trailing ``` if present,
   in place, returns the new length. Not a markdown parser, just this one
   specific, extremely common habit. */
static unsigned int strip_code_fence(char *s, unsigned int len){
    unsigned int start = 0;
    if (len >= 3 && s[0] == '`' && s[1] == '`' && s[2] == '`') {
        unsigned int i = 3;
        while (i < len && s[i] != '\n') i++; /* skip the rest of the ```html line */
        if (i < len) i++;
        start = i;
    }
    unsigned int end = len;
    while (end > start && (s[end - 1] == '\n' || s[end - 1] == ' ')) end--;
    if (end - start >= 3 && s[end - 3] == '`' && s[end - 2] == '`' && s[end - 1] == '`') end -= 3;
    while (end > start && (s[end - 1] == '\n' || s[end - 1] == ' ')) end--;

    unsigned int new_len = end - start;
    for (unsigned int i = 0; i < new_len; i++) s[i] = s[start + i];
    return new_len;
}

static void web_str_copy(char *dst, const char *src, unsigned int cap) {
    unsigned int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Resolves a link's href against the current page's host. No TLS in this
   stack, so an https:// link is a real, honest dead end, reported as one
   rather than silently attempted and failed. Bare relative paths (no
   leading /) and non-http schemes (mailto:, #anchors) are a known gap,
   not a scope this pass claims to cover. */
static int resolve_href(const char *href, const char *cur_host, char *host_out, unsigned int host_cap,
                         char *path_out, unsigned int path_cap, const char **reason_out) {
    if (!href[0]) { *reason_out = "empty link"; return 0; }
    if (web_starts_with(href, "https://")) { *reason_out = "https not supported, no TLS in this kernel yet"; return 0; }
    if (web_starts_with(href, "http://")) {
        const char *h = href + 7;
        unsigned int i = 0;
        while (h[i] && h[i] != '/' && i < host_cap - 1) { host_out[i] = h[i]; i++; }
        host_out[i] = 0;
        if (h[i] == '/') web_str_copy(path_out, h + i, path_cap);
        else { path_out[0] = '/'; path_out[1] = 0; }
        return 1;
    }
    if (href[0] == '/') {
        web_str_copy(host_out, cur_host, host_cap);
        web_str_copy(path_out, href, path_cap);
        return 1;
    }
    *reason_out = "relative/unsupported link (mailto:, anchors, bare relative paths)";
    return 0;
}

/* v8's link navigation: fetch, render, list real links found on the page,
   number keys follow one, 'b' goes back, anything else closes. A small
   fixed-depth history stack, not a general browser session, that's plenty
   for what this proves. */
#define WEB_HISTORY_DEPTH 6
static void browse_web(const char *first_host, const char *first_path) {
    char cur_host[64], cur_path[192];
    web_str_copy(cur_host, first_host, sizeof(cur_host));
    web_str_copy(cur_path, first_path, sizeof(cur_path));

    char hist_host[WEB_HISTORY_DEPTH][64];
    char hist_path[WEB_HISTORY_DEPTH][192];
    int hist_n = 0;

    for (;;) {
        static char body[1400];
        int n = http_get(cur_host, cur_path, 80, body, sizeof(body) - 1);
        if (n < 0) { puts("FAIL (dns/tcp)\n"); return; }
        if (n == 0) { puts("FAIL (no body, response too large or truncated)\n"); return; }
        body[n] = 0;

        static char text[1400];
        unsigned int tn = html_to_text(body, text, sizeof(text));
        putn(tn); puts(" bytes of text:\n\n");
        puts(text);
        putc('\n');

        static struct html_link links[HTML_MAX_LINKS];
        unsigned int nlinks = html_extract_links(body, links, HTML_MAX_LINKS);

        if (!window_open(800, 600, 32)) return;
        window_clear(0x00FAF8F6);
        render_wrapped_text(text, 20, 20, 760, 420, 0x001C1C1E);

        int ly = 460;
        for (unsigned int i = 0; i < nlinks && ly < 560; i++) {
            char label[8]; label[0] = '['; label[1] = (char)('1' + i); label[2] = ']'; label[3] = ' '; label[4] = 0;
            font_draw_string(label, 20, ly, 0x007A2048, -1);
            font_draw_string(links[i].text[0] ? links[i].text : links[i].href, 20 + 32, ly, 0x007A2048, -1);
            ly += 18;
        }
        font_draw_string(hist_n > 0 ? "[b]ack   any other key closes" : "any other key closes", 20, 570, 0x0075726E, -1);

        int k = get_key();
        window_close();
        clear();
        puts("back in text mode\n");

        if (k == 'b' && hist_n > 0) {
            hist_n--;
            web_str_copy(cur_host, hist_host[hist_n], sizeof(cur_host));
            web_str_copy(cur_path, hist_path[hist_n], sizeof(cur_path));
            continue;
        }
        if (k >= '1' && k <= '9') {
            unsigned int idx = (unsigned int)(k - '1');
            if (idx < nlinks) {
                char new_host[64], new_path[192];
                const char *reason = 0;
                if (resolve_href(links[idx].href, cur_host, new_host, sizeof(new_host), new_path, sizeof(new_path), &reason)) {
                    if (hist_n < WEB_HISTORY_DEPTH) {
                        web_str_copy(hist_host[hist_n], cur_host, sizeof(hist_host[0]));
                        web_str_copy(hist_path[hist_n], cur_path, sizeof(hist_path[0]));
                        hist_n++;
                    }
                    web_str_copy(cur_host, new_host, sizeof(cur_host));
                    web_str_copy(cur_path, new_path, sizeof(cur_path));
                    continue;
                }
                puts("can't follow that link: "); puts(reason); putc('\n');
            }
        }
        return;
    }
}

/* Combines HTTP headers with an embedded app's raw bytes (gen_app.sh's
   output) into one buffer and serves it. Shared by every serveapp target
   so adding another embedded app is one dispatch line, not a copy-pasted
   block. */
static void serve_app(const char *label, const unsigned char *data, unsigned int data_len){
    static const char header[] = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
    unsigned int total_len = (sizeof(header) - 1) + data_len;
    char *buf = kmalloc(total_len);
    if (!buf) { puts("out of heap\n"); return; }

    for (unsigned int i = 0; i < sizeof(header) - 1; i++) buf[i] = header[i];
    for (unsigned int i = 0; i < data_len; i++) buf[sizeof(header) - 1 + i] = (char)data[i];

    puts("serving "); puts(label); puts(" (a real app from the codebase, ");
    putn(data_len); puts(" bytes), waiting on :8080...\n");
    puts(tcp_serve_once(8080, buf, total_len) ? "served:ok\n" : "timeout, nobody connected\n");
    kfree(buf);
}

int shellnet_run(const char *line, char *arg) {
    (void)arg;
    if (0) {}
    else if (!strcmp(line, "lspci")) {
        struct pci_device dev;
        if (pci_find_device(0x03, 0x00, &dev)) { puts("VGA device found, BAR0="); puthex(dev.bar0); putc('\n'); }
        else puts("no VGA device found\n");
        if (pci_find_device(0x02, 0x00, &dev)) {
            puts("NIC found, "); puts(dev.bar0_is_io ? "I/O BAR=" : "MEM BAR=");
            puthex(dev.bar0); putc('\n');
        } else puts("no NIC found\n");
    }
    else if (!strcmp(line, "ifconfig")) {
        if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            unsigned char mac[6]; net_get_mac(mac);
            puts("net0: 10.0.2.15\n  mac ");
            for (int i = 0; i < 6; i++) {
                char hx[3]; const char *hexd = "0123456789abcdef";
                hx[0] = hexd[mac[i] >> 4]; hx[1] = hexd[mac[i] & 0xF]; hx[2] = 0;
                puts(hx); if (i < 5) puts(":");
            }
            puts("\n");
        }
    }
    else if (!strcmp(line, "netscan")) {
        /* v30: a real, honest connect scan, Kali-flavored in spirit only,
           not a clone: no SYN-stealth/OS-fingerprint modes, one scan type,
           a fixed common-port list, exactly what tcp_probe_port actually
           supports. Same "not a general tool, a real narrow proof" scope
           as everything else this session, see ROADMAP.md's v30 entry. */
        if (!*arg) { puts("usage: netscan <host, e.g. 10.0.2.2>\n"); }
        else if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            unsigned int ip;
            int have_ip = 0;
            /* accept a bare dotted-quad directly, skip DNS for the common
               "scan a LAN host" case; anything else goes through the same
               DNS path `web`/`nettest` already use. */
            { unsigned int a=0,b=0,c=0,d=0; int i=0; const char *p=arg; int quad=1;
              while (*p) { if (*p=='.') i++; else if (*p<'0'||*p>'9') { quad=0; break; } p++; }
              if (quad && i==3) { unsigned int parts[4]={0,0,0,0}; int pi=0; p=arg;
                  while (*p) { if (*p=='.') pi++; else parts[pi]=parts[pi]*10+(*p-'0'); p++; }
                  a=parts[0];b=parts[1];c=parts[2];d=parts[3];
                  ip = (a<<24)|(b<<16)|(c<<8)|d; have_ip = 1; }
            }
            if (!have_ip) have_ip = dns_resolve(arg, 0x0A000203, &ip);
            if (!have_ip) { puts("could not resolve host\n"); }
            else {
                static const unsigned short ports[] = {21,22,23,25,80,443,3306,8080};
                puts("scanning...\n");
                for (unsigned int i = 0; i < sizeof(ports)/sizeof(ports[0]); i++) {
                    char buf[16]; int n = 0; unsigned int v = ports[i];
                    char tmp[6]; int ti = 0; if (v==0) tmp[ti++]='0'; while (v) { tmp[ti++]='0'+v%10; v/=10; }
                    while (ti) buf[n++] = tmp[--ti];
                    buf[n++] = '/'; buf[n++]='t'; buf[n++]='c'; buf[n++]='p'; buf[n++]=' '; buf[n]=0;
                    puts(buf);
                    int r = tcp_probe_port(ip, ports[i]);
                    puts(r == 1 ? "open\n" : r == 0 ? "closed\n" : "filtered\n");
                }
            }
        }
    }
    else if (!strcmp(line, "web")) {
        char *first_host = arg;
        char *first_path = first_host;
        while (*first_path && *first_path != ' ') first_path++;
        if (*first_path) *first_path++ = 0; else first_path = "/";
        if (!*first_host) { puts("usage: web <host> [path]\n"); }
        else if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            browse_web(first_host, first_path);
        }
    }
    else if (!strcmp(line, "serve")) {
        if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            /* Long enough on purpose: >536 bytes forces tcp_serve_once
               through its multi-segment path, not just the one-chunk case. */
            static const char page[] =
                "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                "<html><body><h1>Joshua Tree</h1>"
                "<p>Served from a kernel with no OS underneath it: no Linux, no XNU, "
                "no Windows NT, nothing between this HTML and the bare metal except "
                "the code in this repository. Booted with a multiboot1 header, brought "
                "up its own GDT, IDT, paging, and a cooperative scheduler, mounted a "
                "FAT16 filesystem it wrote the driver for, found this network card by "
                "walking PCI configuration space by hand, and built Ethernet, ARP, "
                "IPv4, UDP, DNS and TCP from raw bytes on the wire, no libc anywhere "
                "in the chain. The response you are reading crossed that entire stack "
                "in more than one TCP segment, which is exactly why this paragraph is "
                "this long.</p></body></html>";
            puts("waiting for a connection on :8080...\n");
            puts(tcp_serve_once(8080, page, sizeof(page) - 1) ? "served:ok\n" : "timeout, nobody connected\n");
        }
    }
    else if (!strcmp(line, "serveapp")) {
        if (!*arg) { puts("usage: serveapp weather|curbfind|keyrate|bookrank|quotestreak|tonchi|toroid|hikko|fieldbook\n"); }
        else if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            if (!strcmp(arg, "weather"))          serve_app("weather", app_weather_html, app_weather_len);
            else if (!strcmp(arg, "curbfind"))    serve_app("curbfind", app_curbfind_html, app_curbfind_len);
            else if (!strcmp(arg, "keyrate"))     serve_app("keyrate", app_keyrate_html, app_keyrate_len);
            else if (!strcmp(arg, "bookrank"))    serve_app("bookrank", app_bookrank_html, app_bookrank_len);
            else if (!strcmp(arg, "quotestreak")) serve_app("quotestreak", app_quotestreak_html, app_quotestreak_len);
            else if (!strcmp(arg, "tonchi") || !strcmp(arg, "lexly")) serve_app("tonchi", app_tonchi_html, app_tonchi_len);
            else if (!strcmp(arg, "toroid"))      serve_app("toroid", app_toroid_html, app_toroid_len);
            else if (!strcmp(arg, "hikko"))    serve_app("hikko", app_hikko_html, app_hikko_len);
            else if (!strcmp(arg, "fieldbook"))   serve_app("fieldbook", app_fieldbook_html, app_fieldbook_len);
            else puts("unknown app, see usage\n");
        }
    }
    else if (!strcmp(line, "build")) {
        /* v10's "build stuff" loop: take a request, ask the LLM to generate
           a page for it, serve the result live. The kernel-native version
           of what gato does on macOS, minus the file-editing part, there's
           no persistent app catalog to edit yet, just this one slot. */
        if (!*arg) { puts("usage: build <what to make>\n"); }
        else if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            char escaped[256];
            json_escape(arg, escaped, sizeof(escaped));

            static char req_body[1024];
            unsigned int n = 0;
            const char *parts[3];
            parts[0] = "{\"model\":\"llama3.1:8b\",\"stream\":false,\"prompt\":\"Output ONLY raw HTML for one self-contained page, inline CSS and JS, no external resources, no markdown code fences, no explanation, just the HTML. Make: ";
            parts[1] = escaped;
            parts[2] = "\"}";
            for (int p = 0; p < 3; p++) {
                const char *s = parts[p];
                while (*s && n < sizeof(req_body)) req_body[n++] = *s++;
            }

            puts("asking llama3.1 to build it...\n");
            static char resp[8192];
            int rn = http_post("10.0.2.2", "/api/generate", 11434, req_body, n, resp, sizeof(resp) - 1);
            if (rn <= 0) { puts("FAIL (couldn't reach the host's Ollama server)\n"); }
            else {
                resp[rn] = 0;
                static char html[6144];
                unsigned int hn = json_extract_string(resp, "response", html, sizeof(html));
                if (hn == 0) { puts("(no response field in the reply)\n"); }
                else {
                    hn = strip_code_fence(html, hn);
                    serve_app("the generated page", (const unsigned char *)html, hn);
                }
            }
        }
    }
    else if (!strcmp(line, "tcpmatchtest")) {
        /* Regression test for a real bug: tcp_match (net.c) used to trust
           ip->total_length outright with no check against the actual number
           of bytes rtl8139_receive put in the frame, so a malicious/corrupt
           remote peer could claim far more payload than it actually sent
           and every caller (tcp_get/tcp_probe_port/tcp_serve_once) would
           copy that many bytes starting past the real frame, an
           out-of-bounds read and uninitialized-stack-memory disclosure.
           No hardware needed: tcp_match_selftest builds real frames in a
           local buffer and calls the exact same tcp_match(). */
        puts(tcp_match_selftest() ? "tcpmatch: honest frame accepted, lying frame rejected: ok\n" : "tcpmatch: FAILED\n");
    }
    else if (!strcmp(line, "rxclamptest")) {
        /* Regression test for a real bug: rtl8139_receive (rtl8139.c) used
           to return the NIC's raw, unclamped claimed length instead of how
           many bytes it actually copied into the caller's buffer. Every
           net.c caller trusts that return value as ground truth for how
           much of rx[1514] is valid, which is exactly what tcp_match's own
           bound check depends on -- an oversized or underflowed NIC length
           defeated that check one layer down, the same OOB-read class as
           the already-fixed tcp_match bug. No hardware needed:
           rtl8139_clamp_selftest calls the exact same clamp logic
           rtl8139_receive uses. */
        puts(rtl8139_clamp_selftest() ? "rxclamp: oversized/runt NIC lengths clamp correctly: ok\n" : "rxclamp: FAILED\n");
    }
    else return 0;
    return 1;
}
