#ifndef HTTP_H
#define HTTP_H
/* Thin wrapper over net.c's tcp_get/dns_resolve: takes a host and path,
   hands back just the response body (status line and headers stripped).
   One request, no redirects, no chunked transfer-encoding, HTTP/1.0 with
   Connection: close so the server ends the stream for us. */
int http_get(const char *host, const char *path, unsigned short port,
             void *body_out, unsigned int body_maxlen);

/* http_get with a caller-chosen reply deadline in ticks (0 = net.c's
   default, which is sized for a slow local LLM, far too patient for a
   weather one-liner). */
int http_get_timeout(const char *host, const char *path, unsigned short port,
                     void *body_out, unsigned int body_maxlen,
                     unsigned int reply_timeout_ticks);

/* Status code off the last reply's status line ("HTTP/1.1 403 ..." -> 403),
   0 when there was no parseable status line. The body-only return value
   cannot tell a 200 from a proxy's 403 page; callers that care check this. */
int http_last_status(void);

/* Same idea, HTTP/1.0 POST with a Content-Length body (application/json).
   host also accepts a plain dotted-quad IP literal, skipping DNS, which is
   what reaching a service on the host machine itself needs. */
int http_post(const char *host, const char *path, unsigned short port,
              const char *body, unsigned int body_len,
              void *response_out, unsigned int response_maxlen);
#endif
