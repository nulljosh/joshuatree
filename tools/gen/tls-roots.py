#!/usr/bin/env python3
"""Copies the well-known root CAs real sites chain to (37 since 3.0.0, so Cloudflare, Google, Amazon, Microsoft, Let's Encrypt and DigiCert sites verify) out of this machine's default certificate store into arch/arm64/certs/ as PEM.

Run once by hand when the set should change; the PEM files are committed, so a build never needs a network or
the host's store. tools/gen/tls-ta.py turns them into the kernel's embedded trust anchors at build time.
"""
import os, ssl, sys

WANT = ["AAA Certificate Services",
        "Amazon Root CA 1",
        "Amazon Root CA 2",
        "Amazon Root CA 3",
        "Amazon Root CA 4",
        "Baltimore CyberTrust Root",
        "Certum Trusted Network CA",
        "COMODO Certification Authority",
        "COMODO ECC Certification Authority",
        "DigiCert Global Root CA",
        "DigiCert Global Root G2",
        "DigiCert Global Root G3",
        "DigiCert High Assurance EV Root CA",
        "DigiCert TLS ECC P384 Root G5",
        "DigiCert TLS RSA4096 Root G5",
        "Entrust Root Certification Authority - G2",
        "GlobalSign Root CA",
        "GlobalSign Root E46",
        "GlobalSign Root R46",
        "GTS Root R1",
        "GTS Root R2",
        "GTS Root R3",
        "GTS Root R4",
        "ISRG Root X1",
        "ISRG Root X2",
        "Microsoft ECC Root Certificate Authority 2017",
        "Microsoft RSA Root Certificate Authority 2017",
        "Sectigo Public Server Authentication Root E46",
        "Sectigo Public Server Authentication Root R46",
        "SSL.com Root Certification Authority ECC",
        "SSL.com Root Certification Authority RSA",
        "SSL.com TLS ECC Root CA 2022",
        "SSL.com TLS RSA Root CA 2022",
        "Starfield Root Certificate Authority - G2",
        "Starfield Services Root Certificate Authority - G2",
        "USERTrust ECC Certification Authority",
        "USERTrust RSA Certification Authority"]
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "arch", "arm64", "certs")
os.makedirs(out, exist_ok=True)
ctx = ssl.create_default_context()
ctx.load_default_certs()
seen = set()
for der, info in zip(ctx.get_ca_certs(binary_form=True), ctx.get_ca_certs()):
    cn = dict(x[0] for x in info["subject"]).get("commonName")
    if cn in WANT and cn not in seen:
        seen.add(cn)
        name = cn.lower().replace(" ", "-") + ".pem"
        with open(os.path.join(out, name), "w") as f:
            f.write(ssl.DER_cert_to_PEM_cert(der))
        print("wrote", name, "expires", info["notAfter"])
missing = [w for w in WANT if w not in seen]
if missing:
    print("not in this machine's store:", ", ".join(missing))
    sys.exit(1)
