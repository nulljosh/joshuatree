# BearSSL (vendored subset)

Upstream: https://bearssl.org, commit 7bea48e, MIT licensed (see LICENSE).
Flat copy of the pieces a minimal TLS 1.2 client for the Pi needs: the SSL
client core and record layer (AES-CBC and AES-GCM suites only), x509
minimal, the i31 big integers, P-256 (`ec_p256_m31`), RSA and ECDSA verify,
SHA-1/SHA-2, HMAC, HMAC_DRBG, constant-time AES, GCM and the big-endian
codec helpers. The i386 kernel's entropy pool still uses only `sha2small.c`,
`hmac.c`, `hmac_drbg.c`, `dec32be.c`, `enc32be.c` (see the Makefile).

`inc/` is the full public header set, unchanged. `shim/string.h` maps the
one libc header `inner.h` wants onto `lib/libc.h`. Nothing here is modified.
`tools/gen/bearssl-vendor.sh <upstream>` is the file list and re-copies it;
`tools/checks/bearssl-tls-check.py` compiles every file for aarch64.

Not wired into the Pi network code yet. Left out on purpose: ChaCha20/
Poly1305, CCM, DES, client certificates, the 64-bit (`ct64`/`m64`) variants,
and `sysrng.c` (the Pi will seed from its tick timer).
