/* HTTPS on the Pi: BearSSL (third_party/bearssl) over the stream TCP calls in drivers/net.c. TLS 1.2 only, the
   AES-GCM and AES-CBC suites with ECDHE or RSA key exchange, and every server certificate is checked against the
   trust anchors baked in at build time (tls_ta.h, from the PEM files in arch/arm64/certs by tools/gen/tls-ta.py) and against
   the host name asked for. Nothing is fetched at run time but the page itself. Memory is static: one client context,
   one X.509 context and a 33 KB record buffer. The handshake runs on the kernel stack (linker.ld: 64 KiB). */
#include <stddef.h>
#include "bearssl.h"
#include "../../drivers/net.h"
#include "../../drivers/http.h"
#include "tls.h"
#include "rng.h"
#include "tls_ta.h"

unsigned int ticks(void);              /* ip.c */
unsigned long net_clock_utc(void);     /* ip.c: 0 until the network set it */
void *kmalloc(unsigned int n);         /* main.c */
unsigned long heap_mark(void);
void heap_release(unsigned long m);

unsigned long net_clock_floor(void);  /* clock.c: the build time, never unauthenticated network time */
#define TLS_READ_TICKS 800       /* 8 s for one piece of the reply to show up; the browser promises an answer in about 10 */

static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;
static int tls_err, last_status;
static unsigned read_ticks = TLS_READ_TICKS;

int tls_last_error(void) { return tls_err; }
int https_last_status(void) { return last_status; }

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    int r = tcp_read(buf, (unsigned)len, read_ticks);
    return r > 0 ? r : -1;
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    return tcp_write(buf, (unsigned)len) ? (int)len : -1;
}

static void wipe_seed(unsigned char *s, unsigned n) {
    for (unsigned i = 0; i < n; i++) ((volatile unsigned char *)s)[i] = 0;
}
static int platform_seeder(const br_prng_class **ctx) {
    unsigned char seed[32];
    int ok = tls_entropy(seed, sizeof seed);
    if (ok) (*ctx)->update(ctx, seed, sizeof seed);
    wipe_seed(seed, sizeof seed);
    return ok;
}
br_prng_seeder br_prng_seeder_system(const char **name) { if (name) *name = "platform"; return platform_seeder; }

static int setup(const char *host) {
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA, BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
        BR_TLS_RSA_WITH_AES_128_GCM_SHA256, BR_TLS_RSA_WITH_AES_128_CBC_SHA256, BR_TLS_RSA_WITH_AES_128_CBC_SHA };
    static const br_hash_class *const hashes[] = { &br_md5_vtable, &br_sha1_vtable, &br_sha224_vtable, &br_sha256_vtable, &br_sha384_vtable, &br_sha512_vtable };
    br_ssl_client_zero(&sc);
    br_ssl_engine_set_versions(&sc.eng, BR_TLS12, BR_TLS12);
    br_x509_minimal_init(&xc, &br_sha256_vtable, TAS, TAS_NUM);
    unsigned long now = net_clock_utc(), floor = net_clock_floor();
    if (now < floor) now = floor;
    br_x509_minimal_set_time(&xc, (uint32_t)(now / 86400 + 719528), (uint32_t)(now % 86400));
    br_ssl_engine_set_suites(&sc.eng, suites, sizeof suites / sizeof suites[0]);
    br_ssl_client_set_rsapub(&sc, br_rsa_i31_public);
    br_ssl_engine_set_rsavrfy(&sc.eng, br_rsa_i31_pkcs1_vrfy);
    br_ssl_engine_set_ec(&sc.eng, &br_ec_all_m31);
    br_ssl_engine_set_ecdsa(&sc.eng, br_ecdsa_i31_vrfy_asn1);
    br_x509_minimal_set_rsa(&xc, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(&xc, &br_ec_all_m31, br_ecdsa_i31_vrfy_asn1);
    for (int id = br_md5_ID; id <= br_sha512_ID; id++) {
        br_ssl_engine_set_hash(&sc.eng, id, hashes[id - 1]);
        br_x509_minimal_set_hash(&xc, id, hashes[id - 1]);
    }
    br_ssl_engine_set_x509(&sc.eng, &xc.vtable);
    br_ssl_engine_set_prf10(&sc.eng, &br_tls10_prf);
    br_ssl_engine_set_prf_sha256(&sc.eng, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(&sc.eng, &br_tls12_sha384_prf);
    br_ssl_engine_set_aes_cbc(&sc.eng, &br_aes_ct_cbcenc_vtable, &br_aes_ct_cbcdec_vtable);
    br_ssl_engine_set_cbc(&sc.eng, &br_sslrec_in_cbc_vtable, &br_sslrec_out_cbc_vtable);
    br_ssl_engine_set_aes_ctr(&sc.eng, &br_aes_ct_ctr_vtable);
    br_ssl_engine_set_ghash(&sc.eng, &br_ghash_ctmul32);
    br_ssl_engine_set_gcm(&sc.eng, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
    unsigned char seed[32];
    if (!tls_entropy(seed, sizeof seed)) { wipe_seed(seed, sizeof seed); return 0; }
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof seed);
    wipe_seed(seed, sizeof seed);
    if (!br_ssl_client_reset(&sc, host, 0)) return 0;
    br_sslio_init(&ioc, &sc.eng, sock_read, 0, sock_write, 0);
    return 1;
}

/* A fetch in three steps, so a caller can say where it got to (the Pi browser prints a line after each):
   tls_connect opens TCP to ip:port and readies the engine for host; tls_handshake runs the handshake; tls_exchange sends
   the request and reads the whole reply (status line, headers, body) into out, then closes. Each returns -1 with
   tls_last_error set: a BearSSL code (BR_ERR_X509_NOT_TRUSTED is 62, BR_ERR_X509_BAD_SERVER_NAME 56), or TLS_ERR_CONNECT,
   TLS_ERR_TIMEOUT for the network under it. */
static int failed(void) {   /* -1, with the engine's error or the network's in tls_err; closes the connection */
    int e = br_ssl_engine_last_error(&sc.eng);
    tls_err = e != BR_ERR_OK && e != BR_ERR_IO ? e : net_last_error() == NET_ERR_REPLY_TIMEOUT ? TLS_ERR_TIMEOUT : TLS_ERR_CONNECT;
    tcp_close();
    return -1;
}
int tls_connect(unsigned ip, unsigned short port, const char *host) {
    tls_err = 0; read_ticks = TLS_READ_TICKS;
    if (!tcp_open(ip, port)) { tls_err = TLS_ERR_CONNECT; return -1; }
    if (!setup(host)) { tls_err = TLS_ERR_ENTROPY; tcp_close(); return -1; }
    return 0;
}
int tls_handshake(void) {
    return br_sslio_flush(&ioc) == 0 ? 0 : failed();   /* nothing to send yet, so flush runs the handshake and no more */
}
int tls_exchange(const void *request, unsigned request_len, char *out, unsigned max) {
    if (br_sslio_write_all(&ioc, request, request_len) != 0 || br_sslio_flush(&ioc) != 0) return failed();
    int total = 0;
    while ((unsigned)total < max) {
        int r = br_sslio_read(&ioc, out + total, max - (unsigned)total);
        if (r <= 0) break;
        total += r;
    }
    int e = br_ssl_engine_last_error(&sc.eng);
    if (e != BR_ERR_OK && e != BR_ERR_IO) return failed();   /* BR_ERR_IO is the plain FIN an HTTP/1.0 server ends with */
    if (total <= 0 && net_last_error() == NET_ERR_REPLY_TIMEOUT) return failed();
    tcp_close();
    return total;
}
/* The three steps in one, after a DNS lookup of host (TLS_ERR_DNS when that fails). */
int https_fetch_timeout(const char *host, unsigned short port, const void *request, unsigned request_len, char *out, unsigned max, unsigned reply_ticks) {
    unsigned ip;
    tls_err = 0;
    if (!http_resolve_host(host, &ip)) { tls_err = TLS_ERR_DNS; return -1; }
    if (tls_connect(ip, port, host) < 0 || tls_handshake() < 0) return -1;
    read_ticks = reply_ticks ? reply_ticks : TLS_READ_TICKS;
    int got = tls_exchange(request, request_len, out, max);
    read_ticks = TLS_READ_TICKS;
    return got;
}
int https_fetch(const char *host, unsigned short port, const void *request, unsigned request_len, char *out, unsigned max) {
    return https_fetch_timeout(host, port, request, request_len, out, max, TLS_READ_TICKS);
}

/* The body of GET https://host/path, like drivers/http.c's http_get. Returns its length, -1 on any failure. */
int https_get(const char *host, unsigned short port, const char *path, char *body, unsigned max) {
    static const char *const parts[] = { "GET ", 0, " HTTP/1.0\r\nHost: ", 0, "\r\nUser-Agent: JoshuaTree/1.0\r\nConnection: close\r\n\r\n" };
    char req[1024]; unsigned n = 0;
    last_status = 0;
    for (int p = 0; p < 5; p++)
        for (const char *s = p == 1 ? path : p == 3 ? host : parts[p]; *s && n < sizeof req - 1; s++) req[n++] = *s;
    unsigned long mark = heap_mark();
    char *raw = kmalloc(max + 2048);
    if (!raw) return -1;
    int got = https_fetch(host, port, req, n, raw, max + 2047), r = -1;
    if (got > 0) {
        raw[got] = 0;
        last_status = http_status_of(raw, (unsigned)got);
        int b = http_body_start(raw, (unsigned)got);
        if (b >= 0) {
            r = got - b; if ((unsigned)r > max) r = (int)max;
            for (int i = 0; i < r; i++) body[i] = raw[b + i];
        } else r = 0;
    }
    heap_release(mark);
    return r;
}
