#ifndef TLS_H
#define TLS_H
/* HTTPS for the Pi (tls.c): BearSSL over drivers/net.c's TCP stream. See tls.c for the rules. */
#define TLS_ERR_DNS     -1   /* the name did not resolve */
#define TLS_ERR_CONNECT -2   /* no TCP connection */
#define TLS_ERR_TIMEOUT -3   /* connected, nothing came back in time */
#define TLS_ERR_ENTROPY -4   /* hardware random generator unavailable or unhealthy */
int tls_connect(unsigned ip, unsigned short port, const char *host);   /* the three steps of https_fetch, for a caller that reports progress */
int tls_handshake(void);
int tls_exchange(const void *request, unsigned request_len, char *out, unsigned max);
int https_fetch(const char *host, unsigned short port, const void *request, unsigned request_len, char *out, unsigned max);
int https_fetch_timeout(const char *host, unsigned short port, const void *request, unsigned request_len, char *out, unsigned max, unsigned reply_ticks);
int https_get(const char *host, unsigned short port, const char *path, char *body, unsigned max);
int https_last_status(void);
int tls_last_error(void);   /* after a -1: a TLS_ERR_* above, or BearSSL's own code (62 = certificate not trusted) */
#endif
