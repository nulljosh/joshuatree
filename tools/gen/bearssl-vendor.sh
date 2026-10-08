#!/bin/bash
# Re-vendor the BearSSL TLS 1.2 client subset from an upstream checkout.
# Usage: tools/gen/bearssl-vendor.sh /path/to/bearssl-upstream
# Flat copy into third_party/bearssl/src; the file list is the whole policy.
set -euo pipefail
UP="${1:?upstream BearSSL checkout}"
D="$(dirname "$0")/../../third_party/bearssl"
cp "$UP"/inc/*.h "$D/inc/"
cp "$UP"/src/inner.h "$UP"/src/config.h "$D/src/"
cp "$UP"/LICENSE.txt "$D/LICENSE"
for f in \
  ssl/ssl_engine.c ssl/ssl_io.c ssl/ssl_hs_client.c ssl/ssl_client.c \
  ssl/ssl_client_full.c ssl/ssl_lru.c ssl/ssl_hashes.c ssl/ssl_keyexport.c \
  ssl/ssl_rec_cbc.c ssl/ssl_rec_gcm.c \
  ssl/ssl_engine_default_aescbc.c ssl/ssl_engine_default_aesgcm.c \
  ssl/ssl_engine_default_ec.c ssl/ssl_engine_default_rsavrfy.c \
  ssl/ssl_engine_default_ecdsa.c ssl/ssl_client_default_rsapub.c \
  ssl/prf.c ssl/prf_sha256.c ssl/prf_sha384.c ssl/prf_md5sha1.c \
  x509/x509_minimal.c x509/x509_decoder.c x509/skey_decoder.c \
  int/i31_add.c int/i31_bitlen.c int/i31_decmod.c int/i31_decode.c \
  int/i31_decred.c int/i31_encode.c int/i31_fmont.c int/i31_iszero.c \
  int/i31_moddiv.c int/i31_modpow.c int/i31_modpow2.c int/i31_montmul.c \
  int/i31_mulacc.c int/i31_muladd.c int/i31_ninv31.c int/i31_reduce.c \
  int/i31_rshift.c int/i31_sub.c int/i31_tmont.c int/i32_div32.c \
  ec/ec_p256_m31.c ec/ec_prime_i31.c ec/ec_all_m31.c ec/ec_secp256r1.c \
  ec/ec_secp384r1.c ec/ec_secp521r1.c ec/ec_c25519_m31.c ec/ec_curve25519.c \
  ec/ecdsa_i31_vrfy_raw.c ec/ecdsa_i31_vrfy_asn1.c ec/ecdsa_i31_bits.c \
  ec/ecdsa_atr.c \
  rsa/rsa_i31_pkcs1_vrfy.c rsa/rsa_i31_pss_vrfy.c rsa/rsa_i31_pub.c \
  rsa/rsa_pkcs1_sig_unpad.c rsa/rsa_pss_sig_unpad.c \
  hash/sha2small.c hash/sha2big.c hash/sha1.c hash/md5.c hash/md5sha1.c \
  hash/multihash.c hash/dig_oid.c hash/dig_size.c hash/mgf1.c \
  hash/ghash_ctmul32.c \
  mac/hmac.c mac/hmac_ct.c rand/hmac_drbg.c \
  symcipher/aes_ct.c symcipher/aes_ct_dec.c symcipher/aes_ct_enc.c \
  symcipher/aes_ct_cbcdec.c symcipher/aes_ct_cbcenc.c symcipher/aes_ct_ctr.c \
  symcipher/aes_common.c aead/gcm.c \
  codec/dec16be.c codec/dec32be.c codec/dec64be.c codec/enc16be.c \
  codec/enc32be.c codec/enc64be.c codec/dec32le.c codec/enc32le.c codec/ccopy.c
do cp "$UP/src/$f" "$D/src/"; done
echo "vendored $(ls "$D"/src/*.c | wc -l | tr -d ' ') .c files"
