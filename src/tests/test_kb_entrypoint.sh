#!/bin/sh
# The kb entrypoint's embedder gate.
#
# There is no fallback embedder: a kb told to SERVE with none configured must refuse to
# start rather than come up answering searches it cannot answer. The gate has to be
# narrow, though — the same image is run as a one-shot for jobs that never serve a
# query, and the managed deploy's `managed-server-identity install` is exactly that.
# Requiring an embedder of those failed server-identity enrolment on every clean
# install: the kb came up healthy and the server could not talk to it.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
entrypoint="$root/deploy/container/aimee-kb-entrypoint.sh"
[ -f "$entrypoint" ] || { echo "missing $entrypoint" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail=0
ok()   { echo "  ok    $1"; }
bad()  { echo "  FAIL  $1" >&2; fail=1; }

# Definitions only — no cluster, no vault, no image.
AIMEE_KB_ENTRYPOINT_SOURCE_ONLY=1
export AIMEE_KB_ENTRYPOINT_SOURCE_ONLY
# shellcheck disable=SC1090
. "$entrypoint"

echo "embedded store module receives the local socket DSN"
unset AIMEE_STORE_URL AIMEE_STORE_MIGRATION_URL 2>/dev/null || true
embedded_runtime_dsn='postgresql:///aimee_shared?host=/var/lib/aimee/run&user=aimee_store_runtime'
embedded_migration_dsn='postgresql:///aimee_shared?host=/var/lib/aimee/run&user=aimee_store_migrator'
configure_embedded_store_module "$embedded_runtime_dsn" "$embedded_migration_dsn"
if [ "${AIMEE_STORE_URL:-}" = "$embedded_runtime_dsn" ]; then
    ok "embedded runtime DSN exported as AIMEE_STORE_URL"
else
    bad "embedded runtime DSN did not reach AIMEE_STORE_URL"
fi
if [ "${AIMEE_STORE_MIGRATION_URL:-}" = "$embedded_migration_dsn" ]; then
    ok "embedded migration DSN exported as AIMEE_STORE_MIGRATION_URL"
else
    bad "embedded migration DSN did not reach AIMEE_STORE_MIGRATION_URL"
fi
unset AIMEE_STORE_URL AIMEE_STORE_MIGRATION_URL

echo "kb_is_serving: flags mean serve, a bare word means one-shot"
kb_is_serving            && ok "no args -> serving"            || bad "no args should serve"
kb_is_serving --http-port=8741 && ok "--http-port -> serving"  || bad "flag should serve"

# Informational flags are one-shots: aimee-kb answers them at argv[1] and exits. Treating
# every -* as serving refused `docker run <image> --help` on a fresh install, which is
# both the first thing someone types and the moment they least likely have an embedder.
for f in --help -h --version -v --print-embedding-model --bootstrap-vault-env \
         --bootstrap-vault-stdin --list-credential-env-names; do
    if kb_is_serving "$f"; then bad "$f must NOT count as serving"; else ok "$f -> one-shot"; fi
done
if kb_is_serving managed-server-identity install --uid=1000; then
    bad "a bare subcommand must NOT count as serving"
else
    ok "managed-server-identity -> one-shot"
fi
if kb_is_serving team; then bad "'team' must not count as serving"; else ok "team -> one-shot"; fi

# Exercise the persisted 0.4.1 -> 0.4.2 authorization transition without Docker.
grants_tmp=$(mktemp -d)
mkdir -p "$grants_tmp/image" "$grants_tmp/home"
cat > "$grants_tmp/image/memory.grant" <<'GRANT'
version=1
principal_class=1
principal_ref=7
uid=self
executable=/usr/local/libexec/aimee-modules/aimee-module-memory
publish=
subscribe=
request=
serve=5889,5890,5891,5892,5893,5894,5895
GRANT
sed 's/,5895$//' "$grants_tmp/image/memory.grant" > "$grants_tmp/home/memory.grant"
seed_kb_module_grants "$grants_tmp/image" "$grants_tmp/home"
if cmp -s "$grants_tmp/image/memory.grant" "$grants_tmp/home/memory.grant"; then
    ok "0.4.1 KB memory grant gains the memory-data stage"
else bad "upgrade left memory-data unauthorized"; fi
seed_kb_module_grants "$grants_tmp/image" "$grants_tmp/home"
if cmp -s "$grants_tmp/image/memory.grant" "$grants_tmp/home/memory.grant"; then
    ok "grant migration is idempotent"
else bad "repeat grant migration changed policy"; fi
# A narrower policy with a seed record must never be overwritten.
sed 's/serve=.*/serve=5889/' "$grants_tmp/image/memory.grant" > "$grants_tmp/home/memory.grant"
seed_kb_module_grants "$grants_tmp/image" "$grants_tmp/home"
if [ "$(grep '^serve=' "$grants_tmp/home/memory.grant")" = 'serve=5889' ]; then
    ok "operator-modified grant remains narrow"
else bad "operator grant was overwritten"; fi
# Nor may a pre-record policy edit masquerade as a historical image default.
rm "$grants_tmp/home/.seeded/memory.grant"
sed -e 's/,5895$//' -e 's/principal_ref=7/principal_ref=999/' \
    "$grants_tmp/image/memory.grant" > "$grants_tmp/home/memory.grant"
seed_kb_module_grants "$grants_tmp/image" "$grants_tmp/home"
if grep -q '^principal_ref=999$' "$grants_tmp/home/memory.grant"; then
    ok "historical recognition preserves edited identity"
else bad "edited historical policy was overwritten"; fi
rm -rf "$grants_tmp"

echo
echo "start_embedder: refuses only when it is actually serving"
# read_cfg_embedding_model shells out to the aimee-kb binary; stub it to "nothing set".
read_cfg_embedding_model() { printf ''; }

unset EMBEDDER_URL EMBEDDER_MODEL 2>/dev/null || true

# Serving with nothing configured must exit non-zero and say why.
out=$( (start_embedder --http-port=8741) 2>&1 ) && rc=0 || rc=$?
if [ "$rc" -ne 0 ]; then ok "serving with no embedder exits $rc"; else bad "serving with no embedder did not exit"; fi
case "$out" in
*"no embedder selected"*) ok "message names the problem" ;;
*) bad "message missing: $out" ;;
esac
case "$out" in
*"embedder_model"*) ok "message names the bundled fix" ;;
*) bad "message does not name embedder_model" ;;
esac
case "$out" in
*EMBEDDER_URL*) ok "message names the external fix" ;;
*) bad "message does not name EMBEDDER_URL" ;;
esac

# Informational flags must reach the binary, not the refusal.
for f in --help --version --print-embedding-model; do
    out=$( (start_embedder "$f") 2>&1 ) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then ok "$f with no embedder proceeds"; else bad "$f refused (rc=$rc)"; fi
    case "$out" in
    *"no embedder selected"*) bad "$f printed the serving refusal" ;;
    *) ok "$f prints no refusal" ;;
    esac
done

# The one-shot path must be untouched by the same missing configuration.
out=$( (start_embedder managed-server-identity install --server-home=/x) 2>&1 ) && rc=0 || rc=$?
if [ "$rc" -eq 0 ]; then ok "one-shot with no embedder proceeds"; else bad "one-shot refused (rc=$rc): $out"; fi
case "$out" in
*"no embedder selected"*) bad "one-shot printed the serving refusal" ;;
*) ok "one-shot prints no refusal" ;;
esac

# An external endpoint satisfies the gate without a bundled model.
EMBEDDER_URL=http://embedder.example:8760
export EMBEDDER_URL
out=$( (start_embedder --http-port=8741) 2>&1 ) && rc=0 || rc=$?
if [ "$rc" -eq 0 ]; then ok "external embedder satisfies the gate"; else bad "external embedder refused: $out"; fi
unset EMBEDDER_URL

# A selected model on an image with no bundled weights must refuse, not serve silently.
EMBEDDER_MODEL=bekko-a25m
EMBEDDER_VENV=$tmp/absent-venv
export EMBEDDER_MODEL EMBEDDER_VENV
out=$( (start_embedder --http-port=8741) 2>&1 ) && rc=0 || rc=$?
if [ "$rc" -ne 0 ]; then ok "model selected but no bundled embedder exits $rc"; else bad "embedderless image did not refuse"; fi
case "$out" in
*"no bundled embedder"*) ok "message names the image mismatch" ;;
*) bad "message missing: $out" ;;
esac

echo
if [ "$fail" -eq 0 ]; then echo "test_kb_entrypoint: all passed"; else echo "test_kb_entrypoint: FAILED" >&2; fi
exit "$fail"
