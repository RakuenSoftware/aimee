#!/bin/sh
# How far does the store module get, exactly?
#
# It reports "no store backend: attach denied by grant policy" and exits. That
# is one sentence covering two different states, and the difference decides
# whether anything here is broken:
#
#   the OUTBOUND identity has no grant  -- a registry entry that cannot land
#       until the postgres module serves its SQL stage in the same tree, because
#       the validator refuses a client requesting a kind nobody serves
#
#   something about the module itself is wrong
#
# So grant the outbound identity by hand and see what it says next. If the
# module then attaches and fails reaching the postgres SQL stage, the blocker is
# exactly the unlanded stage and nothing else. If it fails some other way, that
# is a real defect and this finds it.
#
# The grant is taken from the INSTALLED BUNDLE rather than written here. It
# used to be a heredoc naming ref 68, which is how it kept saying 68 after the
# ref moved to 69: a transcribed grant goes stale in silence, and what that
# produces is a module denied for a reason that looks nothing like the grant.
set -u

HOME_DIR=/var/lib/aimee-full
GRANTS="$HOME_DIR/modules.d/server"
BUS_SOCK="$HOME_DIR/server-module-bus.sock"

[ -d "$GRANTS" ] || { echo "store-probe: run e2e-252-full.sh first"; exit 2; }

BUNDLED=/opt/aimee/module-grants/server/aimee-postgres.grant
[ -r "$BUNDLED" ] || { echo "store-probe: no $BUNDLED in the install"; exit 2; }
cp "$BUNDLED" "$GRANTS/aimee-postgres.grant"
chmod 0600 "$GRANTS/aimee-postgres.grant"
echo "== granted the outbound identity: $(sed -n 's/^principal_ref=/ref /p' \
   "$GRANTS/aimee-postgres.grant"), requesting 11266 =="

echo
echo "== restarting the store module against the live bus =="
pkill -f "aimee-module-aimee" 2>/dev/null
sleep 1
AIMEE_STORE_URL=postgresql:///aimee_e2e_store?host=/var/run/postgresql \
   /usr/local/libexec/aimee-modules/aimee-module-aimee "$BUS_SOCK" \
   >"$HOME_DIR/store-probe.log" 2>&1 &
MOD=$!
sleep 8

if kill -0 "$MOD" 2>/dev/null; then
   echo "the store module is RUNNING"
   echo "tables in the store:"
   su postgres -c "psql -tAc \"SELECT count(*) FROM information_schema.tables WHERE table_schema='public'\" -d aimee_e2e_store"
   kill "$MOD" 2>/dev/null
else
   echo "the store module exited. What it said:"
   cat "$HOME_DIR/store-probe.log" | sed 's/^/    /'
fi
exit 0
