"""Writes src/github_ca.h: the public root certificates the online firmware
update (src/fw_update.cpp) accepts for github.com and its release downloads.
Run on a machine with the Mozilla CA store in /etc/ssl/certs:
    python3 tools/gen_github_ca.py
"""
import os

ROOTS = [
    "USERTrust_ECC_Certification_Authority",   # github.com
    "USERTrust_RSA_Certification_Authority",
    "Sectigo_Public_Server_Authentication_Root_E46",
    "Sectigo_Public_Server_Authentication_Root_R46",
    "DigiCert_Global_Root_G2",                 # *.githubusercontent.com
    "DigiCert_Global_Root_G3",
    "DigiCert_Global_Root_CA",
    "ISRG_Root_X1",                            # spares
    "ISRG_Root_X2",
]

HEADER = """// Root certificates (public) of the hosts the online firmware update talks to:
// github.com (USERTrust / Sectigo) and the release downloads it redirects to
// (*.githubusercontent.com: DigiCert, or Sectigo), with spares in case GitHub
// changes CA. A download from a host that none of these signed is refused.
// From the Mozilla CA store (/etc/ssl/certs); regenerate with
// tools/gen_github_ca.py.
#pragma once

static const char kGithubRootCAs[] ="""


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    lines = [HEADER]
    for name in ROOTS:
        path = "/etc/ssl/certs/%s.pem" % name
        if not os.path.exists(path):
            print("skipped (not in the store): " + name)
            continue
        lines.append("    // " + name)
        for line in open(path).read().strip().splitlines():
            lines.append('    "%s\\n"' % line)
    lines[-1] += ";"
    with open(os.path.join(root, "src", "github_ca.h"), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
