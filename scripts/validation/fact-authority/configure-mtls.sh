#!/bin/bash
# Point aimee-server at the client CA and record the API bearer the mTLS test
# needs. Run AS ROOT in the container, after make-mtls-certs.sh.
#
# mtls stays `optional`, not `required`: the test needs one leg that connects
# over the SAME TLS port WITHOUT a client certificate, so that the only
# difference between "no account" and "account" is the certificate itself.
# Under `required` that leg could not connect at all and would "pass" by
# failing the handshake.
set -u
CONF=/root/aimee.yaml
CA=/root/tls/client-ca.crt

# aimee OWN client CA (pki.c:67 -> $AIMEE_HOME/tls/client-ca.crt). It must be
# this one, not a CA invented here: the server checks the chain against it AND
# re-checks the serial against the durable roster, and only certs aimee issued
# are in that roster.
[ -f "$CA" ] || { echo "aimee client CA missing at $CA -- issue one cert first" >&2; exit 1; }

python3 - "$CONF" "$CA" <<'PY'
import sys
path, ca = sys.argv[1], sys.argv[2]
s = open(path).read()
import re
if "mtls_client_ca" in s:
    s = re.sub(r"    mtls_client_ca: .*", "    mtls_client_ca: %s" % ca, s)
    open(path, "w").write(s)
    print("mtls_client_ca repointed")
else:
    if "    mtls: optional" not in s:
        print("FAIL: no 'mtls: optional' line under aimee.api to anchor to")
        raise SystemExit(1)
    s = s.replace("    mtls: optional",
                  "    mtls: optional\n    mtls_client_ca: %s" % ca)
    open(path, "w").write(s)
    print("mtls_client_ca added")
PY

grep -n -A5 "^  api:" "$CONF"
