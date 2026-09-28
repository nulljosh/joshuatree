/* PBKDF2-HMAC-SHA256 over BearSSL's br_hmac, see auth_kdf.c. The only
   password-to-key derivation kernel/auth.h stores new records with. */
#ifndef AUTH_KDF_H
#define AUTH_KDF_H

void auth_pbkdf2_sha256(const void *password, unsigned int password_len,
                        const void *salt, unsigned int salt_len,
                        unsigned int iterations,
                        unsigned char *out, unsigned int out_len);

#endif
