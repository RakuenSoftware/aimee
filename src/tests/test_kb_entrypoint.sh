#!/bin/sh
# The legacy launcher delegates to the one maintained role/credential boundary.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cat > "$tmp/canonical" <<'SH'
#!/bin/sh
[ "$AIMEE_INSTANCE_ROLE" = kb ]
[ "$#" = 2 ] && [ "$1" = '--one-shot' ] && [ "$2" = 'literal $argument' ]
printf 'delegated\n'
SH
chmod +x "$tmp/canonical"
sed "s|/usr/local/bin/aimee-server-entrypoint|$tmp/canonical|" \
    "$root/deploy/container/aimee-kb-entrypoint.sh" > "$tmp/entrypoint"
result=$(env -u AIMEE_INSTANCE_ROLE sh "$tmp/entrypoint" --one-shot 'literal $argument')
[ "$result" = delegated ]
result=$(AIMEE_INSTANCE_ROLE=kb sh "$tmp/entrypoint" --one-shot 'literal $argument')
[ "$result" = delegated ]
if AIMEE_INSTANCE_ROLE=server sh "$tmp/entrypoint" --one-shot 'literal $argument' > "$tmp/out" 2>/dev/null; then
    echo 'KB launcher accepted a conflicting role' >&2
    exit 1
fi
[ ! -s "$tmp/out" ]
if grep -Eq 'pg_ctl|initdb|AIMEE_DB2_URL' "$root/deploy/container/aimee-kb-entrypoint.sh"; then
    echo 'retired database startup returned' >&2
    exit 1
fi
printf 'KB compatibility launcher: all tests passed\n'
