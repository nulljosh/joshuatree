#ifndef APPSIG_H
#define APPSIG_H
/* App signatures (6.54.0). NAME.SIG beside NAME.APP in JT/APPS is an ECDSA P-256 signature (DER, as openssl writes it) over
   SHA-256 of "JTAPP1\0", the app's lower-case name, "\0", then the image, by the key the kernel was built with (APP_KEY,
   app_key.h by default: Joshua's, made by tools/signapp.sh --new-key). The prefix keeps the key's signatures for apps only;
   the name means a signed SUN.APP copied to another name no longer verifies. */
#define APPSIG_MIN 8
#define APPSIG_MAX 72
int appsig_ok(const char *name, unsigned n, const void *img, unsigned long len, const unsigned char *sig, unsigned siglen);
#endif
