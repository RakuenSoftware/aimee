#!/bin/bash
# Replace the running aimee-module-config binary and bring the stack back up.
#
# `pct push` onto a running executable fails with "Text file busy" -- and that
# failure is easy to miss, because the container keeps working perfectly with the
# OLD binary. The only reason it was caught here is that the push was followed by
# a content check rather than trusted.
#
# The staged replacement goes to /tmp first, the module is stopped, the binary is
# moved into place, and the stack is restarted. The content is verified at the
# end: "I replaced it" has to become "it is the new one".
# Run AS ROOT in the container. Expects /tmp/aimee-module-config to be staged.
set -u
export LC_ALL=C
SRC=/tmp/aimee-module-config
DST=/usr/local/libexec/aimee-modules/aimee-module-config

[ -s "$SRC" ] || { echo "FAIL: $SRC not staged" >&2; exit 1; }
before="$(md5sum "$DST" 2>/dev/null | cut -d' ' -f1)"

# Stop everything that holds the binary open. The daemons are restarted below.
pkill -f /usr/local/libexec/aimee-modules/aimee-module-config 2>/dev/null
pkill -f /usr/local/bin/aimee-server 2>/dev/null
pkill -f /usr/local/bin/aimee-kb 2>/dev/null
sleep 3

cp -f "$SRC" "$DST" || { echo "FAIL: could not replace $DST" >&2; exit 1; }
chmod +x "$DST"
after="$(md5sum "$DST" | cut -d' ' -f1)"
want="$(md5sum "$SRC" | cut -d' ' -f1)"
if [ "$after" != "$want" ]; then
  echo "FAIL: $DST does not match the staged binary after the copy" >&2
  exit 1
fi
echo "config module replaced: ${before:-none} -> $after"

bash /root/install-config-module.sh grants >/dev/null 2>&1
bash /root/start-kb.sh    >/dev/null 2>&1
bash /root/smm.sh         >/dev/null 2>&1
bash /root/install-postgres-module.sh >/dev/null 2>&1
bash /root/start-server.sh >/dev/null 2>&1
bash /root/imms.sh        >/dev/null 2>&1
sleep 6

export AIMEE_HOME=/root AIMEE_API_ENDPOINT=unix:/root/aimee-http.sock
/usr/local/bin/aimee status 2>&1 | head -8
