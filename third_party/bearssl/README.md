# BearSSL (vendored subset)

Upstream: https://bearssl.org, version 0.6, MIT licensed (see LICENSE).
Only the pieces the kernel's entropy pool needs: SHA-256 (`sha2small.c`),
HMAC (`hmac.c`), HMAC_DRBG (`hmac_drbg.c`) and the two big-endian codec
helpers they call. `inc/` is the full public header set, unchanged, because
`bearssl.h` includes all of it. `shim/string.h` maps the one libc header
`inner.h` wants onto `lib/libc.h`. Nothing here is modified; pull the same
files from a newer upstream tarball to update.
