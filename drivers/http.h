#ifndef HTTP_H
#define HTTP_H
/* Thin wrapper over net.c's tcp_get/dns_resolve: takes a host and path,
   hands back just the response body (status line and headers stripped).
   One request, no redirects, no chunked transfer-encoding, HTTP/1.0 with
   Connection: close so the server ends the stream for us. */
int http_get(const char *host, const char *path, unsigned short port,
             void *body_out, unsigned int body_maxlen);
#endif
