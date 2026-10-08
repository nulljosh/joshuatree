#!/usr/bin/env python3
"""Copies a few well-known root CAs out of this machine's default certificate store into arch/arm64/certs/ as PEM.

Run once by hand when the set should change; the PEM files are committed, so a build never needs a network or
the host's store. tools/gen/tls-ta.py turns them into the kernel's embedded trust anchors at build time.
"""
import os, ssl, sys

WANT = ["ISRG Root X1", "DigiCert Global Root G2", "DigiCert Global Root CA", "GTS Root R1",
        "GlobalSign Root CA", "Amazon Root CA 1", "USERTrust RSA Certification Authority"]
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
