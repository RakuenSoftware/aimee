#!/bin/bash
# Set aimee.api.mtls, because two of these probes need opposite postures and the
# reason is not obvious.
#
# server_http_effective_conn_caps():
#
#     if (!is_tcp || mtls_mode <= 0)
#        return server_http_conn_caps(is_tcp, bearer, remote_writes);
#     if (mtls_authenticated)
#        return remote_writes >= FULL ? CAPS_ALL : CAPS_AUTHENTICATED;
#     return CAPS_READ_ONLY & ~CAP_CHAT;   <-- optional mode, no client cert
#
# So with mTLS in OPTIONAL mode a caller that presents no client certificate is
# read-only NO MATTER WHAT ITS IDENTITY TOKEN SAYS -- deliberately: "optional-mode
# bearer fallback is deliberately weaker than a client cert". Only when mTLS is
# off does the per-user tier from the token reach the route gate.
#
#   test-mtls-authority.sh        needs `optional` -- it is about the certificate
#   test-account-tcp-authority.sh needs `off`      -- it is about the identity
#                                                     token ALONE, and with a
#                                                     certificate present the
#                                                     certificate would supply
#                                                     the account instead, which
#                                                     is the other test
#
# Usage: set-mtls-mode.sh off|optional|required
# Run AS ROOT in the container.
set -u
export LC_ALL=C
MODE="${1:-}"
CONF=/root/aimee.yaml
case "$MODE" in
  off|optional|required) ;;
  *) echo "usage: set-mtls-mode.sh off|optional|required" >&2; exit 1 ;;
esac

python3 - "$CONF" "$MODE" <<'PY'
import re, sys
path, mode = sys.argv[1], sys.argv[2]
s = open(path).read()
if not re.search(r"^    mtls: .*$", s, re.M):
    print("FAIL: no 'mtls:' line under aimee.api"); raise SystemExit(1)
s = re.sub(r"^    mtls: .*$", "    mtls: %s" % mode, s, flags=re.M)
open(path, "w").write(s)
print("mtls: %s" % mode)
PY
grep -n -A5 "^  api:" "$CONF"
