#!/bin/bash
# Build integrity tests: catch common Makefile and source breakage early.
# Run from the src/ directory.
set -uo pipefail

MODE="${1:-default}"

FAIL=0
pass() { echo "  PASS: $1"; }
fail() { echo "  FAIL: $1"; FAIL=1; }

if python3 ../scripts/check-vault-only-container-env.py >/dev/null; then
    pass "server and KB container definitions keep credentials out of long-lived environments"
else
    fail "server or KB container definitions persist credentials outside Vault"
fi

# Docker E2E must exercise the same operator contract as production: build the
# image, seal first-boot credentials through the disposable helper, and only
# then create the long-lived service without rebuilding it.
container_smoke_bootstrap_ok=1
for smoke_spec in \
    "../scripts/aimee-kb-docker-smoke.sh:kb" \
    "../scripts/aimee-server-docker-smoke.sh:all" \
    "../scripts/aimee-server-standalone-docker-smoke.sh:server"; do
    smoke_script=${smoke_spec%:*}
    bootstrap_target=${smoke_spec##*:}
    smoke_build_line=$(grep -nF '"${DC[@]}" build' "$smoke_script" | cut -d: -f1)
    smoke_bootstrap_line=$(grep -nF \
        "scripts/aimee-compose-vault-bootstrap.sh -f \"\$bootstrap_compose\" $bootstrap_target" \
        "$smoke_script" | cut -d: -f1)
    smoke_up_line=$(grep -nF '"${DC[@]}" up -d --no-build' "$smoke_script" | cut -d: -f1)
    if [ -z "$smoke_build_line" ] || [ -z "$smoke_bootstrap_line" ] || [ -z "$smoke_up_line" ] ||
       [ "$smoke_build_line" -ge "$smoke_bootstrap_line" ] ||
       [ "$smoke_bootstrap_line" -ge "$smoke_up_line" ] ||
       grep -qF '"${DC[@]}" up -d --build' "$smoke_script"; then
        container_smoke_bootstrap_ok=0
    fi
done
if [ "$container_smoke_bootstrap_ok" -eq 1 ]; then
    pass "Docker smokes Vault-bootstrap before creating long-lived containers"
else
    fail "Docker smoke bypasses the disposable Vault bootstrap contract"
fi

# Debian installs runuser under /usr/sbin. The disposable helper overrides the
# image entrypoint, so its path must match the runtime image exactly.
if grep -qF -- '--entrypoint /usr/sbin/runuser aimee-server' \
        ../scripts/aimee-compose-vault-bootstrap.sh &&
   ! grep -qF -- '--entrypoint /usr/bin/runuser' \
        ../scripts/aimee-compose-vault-bootstrap.sh; then
    pass "server Vault bootstrap uses the runtime image's runuser path"
else
    fail "server Vault bootstrap points at a missing runuser binary"
fi

# The server image entrypoint must honor Docker's explicit command override, but
# even an override must first consume credential env into Vault and scrub it.
# Stub only the short-lived bootstrap transport so this can run outside the
# image; the explicit child proves it received no credential value.
entrypoint_test_dir=$(mktemp -d /tmp/aimee-entrypoint.XXXXXX)
cat >"$entrypoint_test_dir/aimee-server" <<'SH'
#!/bin/sh
[ -n "${ENTRYPOINT_TEST_API_KEY:-}" ] || exit 3
[ "$*" = "--bootstrap-vault-env --drop-user aimee" ] || exit 4
[ -z "${ENTRYPOINT_TEST_BOOTSTRAP_FAIL:-}" ] || exit 9
exit 0
SH
chmod +x "$entrypoint_test_dir/aimee-server"
cat >"$entrypoint_test_dir/runuser" <<'SH'
#!/bin/sh
[ -n "${ENTRYPOINT_TEST_API_KEY:-}" ] || exit 3
case "$*" in
    *--list-credential-env-names*) printf '%s\n' ENTRYPOINT_TEST_API_KEY ;;
esac
exit 0
SH
chmod +x "$entrypoint_test_dir/runuser"
entrypoint_output=$(env -i PATH="$entrypoint_test_dir:/usr/bin:/bin" \
    AIMEE_HOME="$entrypoint_test_dir/home" ENTRYPOINT_TEST_API_KEY=first-boot-only \
    sh ../deploy/container/server-entrypoint.sh sh -c \
    'printf "%s\n" "${ENTRYPOINT_TEST_API_KEY-unset}"' 2>/dev/null)
if [ "$entrypoint_output" = "unset" ]; then
    pass "server entrypoint Vault-ingests and scrubs before an explicit command override"
else
    fail "server entrypoint bypassed Vault ingestion or leaked a credential to an override"
fi
entrypoint_fail_output=$(env -i PATH="$entrypoint_test_dir:/usr/bin:/bin" \
    AIMEE_HOME="$entrypoint_test_dir/home" ENTRYPOINT_TEST_API_KEY=first-boot-only \
    ENTRYPOINT_TEST_BOOTSTRAP_FAIL=1 \
    sh ../deploy/container/server-entrypoint.sh sh -c 'printf "%s\n" child-started' 2>/dev/null)
entrypoint_fail_rc=$?
rm -rf "$entrypoint_test_dir"
if [ "$entrypoint_fail_rc" -ne 0 ] && [ -z "$entrypoint_fail_output" ]; then
    pass "server entrypoint aborts before children when Vault bootstrap fails"
else
    fail "server entrypoint continued after Vault bootstrap failure"
fi

# The KB entrypoint can remain PID 1 while supervising its embedded PostgreSQL,
# so it must replace its process image after scrubbing inherited credentials.
# Use the external-DB lane to avoid starting PostgreSQL while proving that both
# the injected DB URL and an unrelated credential are absent in the final KB.
kb_entrypoint_test_dir=$(mktemp -d /tmp/aimee-kb-entrypoint.XXXXXX)
cat >"$kb_entrypoint_test_dir/aimee-kb" <<'SH'
#!/bin/sh
case "${1:-}" in
    --bootstrap-vault-env)
        [ -z "${ENTRYPOINT_BOOTSTRAP_LOG:-}" ] || printf x >>"$ENTRYPOINT_BOOTSTRAP_LOG"
        exit 0
        ;;
    --vault-db2-external) exit 0 ;;
    # The entrypoint asks the binary which embedder is selected instead of parsing
    # aimee.yaml. Exit 1 = nothing selected, so this stub starts no embedder; without
    # the case the stub would fall through and print "clean", which the entrypoint
    # would take as a MODEL NAME.
    --print-embedding-model) exit 1 ;;
    --list-credential-env-names)
        [ -n "${AIMEE_DB2_URL:-}" ] && printf '%s\n' AIMEE_DB2_URL
        [ -n "${ENTRYPOINT_TEST_API_KEY:-}" ] && printf '%s\n' ENTRYPOINT_TEST_API_KEY
        exit 0
        ;;
esac
if [ -n "${AIMEE_DB2_URL:-}" ]; then
    printf '%s\n' dirty-db-url
elif [ -n "${ENTRYPOINT_TEST_API_KEY:-}" ]; then
    printf '%s\n' dirty-api-key
else
    printf '%s\n' clean
fi
SH
chmod +x "$kb_entrypoint_test_dir/aimee-kb"
# stderr is captured separately, not folded in: the entrypoint legitimately logs
# operator diagnostics there (which embedder it is using), and folding them into
# stdout would turn this into an assertion that the entrypoint is silent. What must
# hold is that no credential VALUE reaches either stream, and that the final process
# image is credential-free.
#
# EMBEDDER_URL is set because a serving kb with no embedder refuses to start, and
# these two checks are about credential scrubbing, not embedder selection. The gate
# itself is covered by tests/test_kb_entrypoint.sh.
kb_entrypoint_stderr="$kb_entrypoint_test_dir/stderr.log"
kb_entrypoint_output=$(env -i PATH="$kb_entrypoint_test_dir:/usr/bin:/bin" \
    AIMEE_HOME="$kb_entrypoint_test_dir/home" \
    AIMEE_DB2_URL=postgresql://external.invalid/aimee \
    ENTRYPOINT_TEST_API_KEY=first-boot-only \
    EMBEDDER_URL=http://embedder.invalid \
    sh ../deploy/container/aimee-kb-entrypoint.sh 2>"$kb_entrypoint_stderr")
if [ "$kb_entrypoint_output" = "clean" ] &&
    ! grep -qE 'first-boot-only|external\.invalid' "$kb_entrypoint_stderr"; then
    pass "KB entrypoint clean-reexec removes inherited first-boot credentials"
else
    fail "KB entrypoint left first-boot credentials in its long-lived process image ($kb_entrypoint_output, stderr=$(tr '\n' ' ' <"$kb_entrypoint_stderr"))"
fi

# Treat the internal bootstrap marker as untrusted input. A container runtime
# can supply entrypoint arguments, so inheriting any credential must force one
# more credential-free exec even when that marker was present at first boot.
kb_bootstrap_log="$kb_entrypoint_test_dir/bootstrap.log"
kb_marked_stderr="$kb_entrypoint_test_dir/stderr-marked.log"
kb_marked_output=$(env -i PATH="$kb_entrypoint_test_dir:/usr/bin:/bin" \
    AIMEE_HOME="$kb_entrypoint_test_dir/home-marked" \
    AIMEE_DB2_URL=postgresql://external.invalid/aimee \
    ENTRYPOINT_TEST_API_KEY=first-boot-only \
    ENTRYPOINT_BOOTSTRAP_LOG="$kb_bootstrap_log" \
    EMBEDDER_URL=http://embedder.invalid \
    sh ../deploy/container/aimee-kb-entrypoint.sh \
    --aimee-internal-vault-bootstrapped-external-db 2>"$kb_marked_stderr")
kb_bootstrap_count=$(wc -c <"$kb_bootstrap_log")
kb_marked_stderr_text=$(tr '\n' ' ' <"$kb_marked_stderr")
kb_marked_stderr_dirty=0
grep -qE 'first-boot-only|external\.invalid' "$kb_marked_stderr" && kb_marked_stderr_dirty=1
if [ "$kb_marked_output" = "clean" ] && [ "$kb_bootstrap_count" -eq 2 ] &&
    [ "$kb_marked_stderr_dirty" -eq 0 ]; then
    pass "KB entrypoint ignores a spoofed bootstrap marker when credentials are inherited"
else
    fail "KB entrypoint trusted a bootstrap marker before a clean re-exec ($kb_marked_output, bootstraps=$kb_bootstrap_count, stderr=$kb_marked_stderr_text)"
fi

# A one-shot sharing the kb's volume (the managed deploy's aimee-server-identity
# job) finds the cluster already running and must CONNECT to it, not provision a
# second one over the same data directory. That job runs as root deliberately, and
# refusing it there -- PostgreSQL forbids running the server as root, not
# connecting as one -- failed managed server identity enrollment on every clean
# install. Stub pg_isready as "a cluster is up" and assert the entrypoint reaches
# the binary instead of exiting.
kb_shared_dir=$(mktemp -d /tmp/aimee-kb-shared.XXXXXX)
mkdir -p "$kb_shared_dir/bin" "$kb_shared_dir/pgbin"
cat >"$kb_shared_dir/bin/aimee-kb" <<'SH'
#!/bin/sh
case "${1:-}" in
    --bootstrap-vault-env|--list-credential-env-names) exit 0 ;;
    --vault-db2-external) exit 1 ;;   # embedded lane: the path that provisions
    --print-embedding-model) exit 1 ;;
esac
printf 'reached-binary:%s\n' "${1:-none}"
SH
cat >"$kb_shared_dir/pgbin/pg_isready" <<'SH'
#!/bin/sh
exit 0
SH
cat >"$kb_shared_dir/pgbin/initdb" <<'SH'
#!/bin/sh
echo "initdb-must-not-run" >&2
exit 1
SH
chmod +x "$kb_shared_dir/bin/aimee-kb" "$kb_shared_dir/pgbin/pg_isready" \
    "$kb_shared_dir/pgbin/initdb"
kb_shared_stderr="$kb_shared_dir/stderr.log"
kb_shared_output=$(env -i PATH="$kb_shared_dir/bin:/usr/bin:/bin" \
    AIMEE_HOME="$kb_shared_dir/home" \
    AIMEE_DB2_PG_BIN="$kb_shared_dir/pgbin" \
    sh ../deploy/container/aimee-kb-entrypoint.sh \
    managed-server-identity 2>"$kb_shared_stderr" || true)
rm -rf "$kb_entrypoint_test_dir"
if [ "$kb_shared_output" = "reached-binary:managed-server-identity" ] &&
    ! grep -q 'initdb-must-not-run' "$kb_shared_stderr"; then
    pass "KB entrypoint reuses an already-running cluster instead of provisioning a second"
else
    fail "KB entrypoint did not reuse the running cluster ($kb_shared_output, stderr=$(tr '\n' ' ' <"$kb_shared_stderr"))"
fi
rm -rf "$kb_shared_dir"

# pg_ctl --wait defaults to a 60s deadline, and crash recovery after an unclean
# stop routinely exceeds it: fsyncing the data directory alone measured 65s on a
# 27k-vector corpus. Timing out makes the entrypoint exit, `restart:
# unless-stopped` start the container again, and recovery replay FROM SCRATCH --
# a livelock where every attempt is killed at a deadline it could never meet.
# Assert the timeout is raised, and raised BEFORE the start it has to govern.
kb_pgctltimeout_line=$(grep -nE '^[[:space:]]+export PGCTLTIMEOUT=' \
    ../deploy/container/aimee-kb-entrypoint.sh | head -1 | cut -d: -f1)
kb_pgctl_start_line=$(grep -nF '"$PGBIN/pg_ctl" --pgdata="$PGDATA" --wait --silent' \
    ../deploy/container/aimee-kb-entrypoint.sh | head -1 | cut -d: -f1)
kb_pgctltimeout_default=$(sed -n 's/.*export PGCTLTIMEOUT="\${AIMEE_DB2_PGCTLTIMEOUT:-\([0-9]*\)}".*/\1/p' \
    ../deploy/container/aimee-kb-entrypoint.sh | head -1)
if [ -n "$kb_pgctltimeout_line" ] && [ -n "$kb_pgctl_start_line" ] &&
    [ -n "$kb_pgctltimeout_default" ] &&
    [ "$kb_pgctltimeout_line" -lt "$kb_pgctl_start_line" ] &&
    [ "$kb_pgctltimeout_default" -gt 60 ]; then
    pass "KB entrypoint waits past pg_ctl's 60s default so crash recovery can finish"
else
    fail "KB entrypoint must export PGCTLTIMEOUT>60 before starting the cluster (export=$kb_pgctltimeout_line, start=$kb_pgctl_start_line, default=$kb_pgctltimeout_default)"
fi

# An ordinary docker stop/restart must terminate the supervising shell after it
# forwards the signal and must stop embedded PostgreSQL before Docker's timeout
# escalates to SIGKILL. Merely trapping TERM without exiting returns to the
# monitor loop and makes every routine restart depend on WAL recovery.
if grep -qF 'shutdown_embedded() {' ../deploy/container/aimee-kb-entrypoint.sh &&
   grep -qF 'trap - EXIT HUP INT TERM' ../deploy/container/aimee-kb-entrypoint.sh &&
   grep -qF "trap 'shutdown_embedded; exit 0' HUP INT TERM" ../deploy/container/aimee-kb-entrypoint.sh; then
    pass "KB entrypoint makes signal-driven embedded PostgreSQL shutdown terminal"
else
    fail "KB entrypoint can return to its monitor loop after Docker requests shutdown"
fi

# The export path starts the same cluster in a stopped container, so it is
# exposed to the identical recovery wait -- and an export timing out aborts with
# the data intact but unread.
kb_export_timeout_line=$(grep -nE '^[[:space:]]+export PGCTLTIMEOUT=' \
    ../deploy/container/aimee-kb-db-export.sh | head -1 | cut -d: -f1)
kb_export_start_line=$(grep -nF '"$PGBIN/pg_ctl" --pgdata="$PGDATA" --wait --silent' \
    ../deploy/container/aimee-kb-db-export.sh | head -1 | cut -d: -f1)
if [ -n "$kb_export_timeout_line" ] && [ -n "$kb_export_start_line" ] &&
    [ "$kb_export_timeout_line" -lt "$kb_export_start_line" ]; then
    pass "KB db-export waits past pg_ctl's 60s default before reading the cluster"
else
    fail "KB db-export must export PGCTLTIMEOUT before starting the cluster (export=$kb_export_timeout_line, start=$kb_export_start_line)"
fi

# The server image intentionally supervises multiple long-lived planes, so it
# still needs a PID-1 subreaper. It must not start tini until after first-boot
# credentials have been sealed and unset: an earlier tini permanently retains
# the original container environment even when every child scrubs its copy.
vault_bootstrap_line=$(grep -nF 'aimee-server --bootstrap-vault-env --drop-user aimee' \
    ../deploy/container/server-entrypoint.sh | cut -d: -f1)
credential_unset_line=$(grep -nF 'unset "$_secret_name"' \
    ../deploy/container/server-entrypoint.sh | cut -d: -f1)
tini_exec_line=$(grep -nF 'exec /usr/bin/tini -- aimee-server-entrypoint --aimee-internal-vault-bootstrapped' \
    ../deploy/container/server-entrypoint.sh | cut -d: -f1)
credential_reexec_condition=$(grep -nF 'if [ "$vault_bootstrapped" -eq 0 ] || [ "$had_credential_env" -eq 1 ]; then' \
    ../deploy/container/server-entrypoint.sh | cut -d: -f1)
first_unrelated_child_line=$(grep -nF 'mkdir -p "$AIMEE_HOME"' \
    ../deploy/container/server-entrypoint.sh | head -1 | cut -d: -f1)
if grep -qE '^[[:space:]]+tini \\' ../Dockerfile.server &&
   grep -qF 'ENTRYPOINT ["aimee-server-entrypoint"]' ../Dockerfile.server &&
   [ -n "$vault_bootstrap_line" ] && [ -n "$credential_unset_line" ] &&
   [ -n "$tini_exec_line" ] && [ -n "$credential_reexec_condition" ] &&
   [ -n "$first_unrelated_child_line" ] &&
   [ "$vault_bootstrap_line" -lt "$credential_unset_line" ] &&
   [ "$credential_unset_line" -lt "$tini_exec_line" ] &&
   [ "$credential_unset_line" -lt "$first_unrelated_child_line" ] &&
   ! grep -qF 'env | sed' ../deploy/container/server-entrypoint.sh; then
    pass "server image Vault-ingests and scrubs before any unrelated child or PID 1 subreaper"
else
   fail "server image must seal and scrub credentials before spawning unrelated children"
fi

# The KB entrypoint has the same invariant. In particular, it must not seed a
# config, initialize PostgreSQL, or pipe environment values through text tools
# before the Vault bootstrap and parent-environment scrub have completed.
kb_vault_bootstrap_line=$(grep -nF 'aimee-kb --bootstrap-vault-env' \
    ../deploy/container/aimee-kb-entrypoint.sh | head -1 | cut -d: -f1)
kb_credential_unset_line=$(grep -nF 'unset "$_secret_name"' \
    ../deploy/container/aimee-kb-entrypoint.sh | cut -d: -f1)
kb_clean_reexec_first=$(grep -nF 'exec /bin/sh "$0" --aimee-internal-vault-bootstrapped-' \
    ../deploy/container/aimee-kb-entrypoint.sh | head -1 | cut -d: -f1)
kb_clean_reexec_last=$(grep -nF 'exec /bin/sh "$0" --aimee-internal-vault-bootstrapped-' \
    ../deploy/container/aimee-kb-entrypoint.sh | tail -1 | cut -d: -f1)
kb_credential_reexec_condition=$(grep -nF 'if [ "$vault_bootstrapped" -eq 0 ] || [ "$had_credential_env" -eq 1 ]; then' \
    ../deploy/container/aimee-kb-entrypoint.sh | cut -d: -f1)
kb_first_unrelated_child_line=$(grep -nF 'mkdir -p "$AIMEE_HOME"' \
    ../deploy/container/aimee-kb-entrypoint.sh | head -1 | cut -d: -f1)
if [ -n "$kb_vault_bootstrap_line" ] && [ -n "$kb_credential_unset_line" ] &&
   [ -n "$kb_clean_reexec_first" ] && [ -n "$kb_clean_reexec_last" ] &&
   [ -n "$kb_credential_reexec_condition" ] &&
   [ -n "$kb_first_unrelated_child_line" ] &&
   [ "$kb_vault_bootstrap_line" -lt "$kb_credential_unset_line" ] &&
   [ "$kb_credential_unset_line" -lt "$kb_clean_reexec_first" ] &&
   [ "$kb_clean_reexec_last" -lt "$kb_first_unrelated_child_line" ] &&
   [ "$kb_credential_unset_line" -lt "$kb_first_unrelated_child_line" ] &&
   ! grep -qF 'export AIMEE_DB2_URL' ../deploy/container/aimee-kb-entrypoint.sh &&
   ! grep -qF 'env | sed' ../deploy/container/aimee-kb-entrypoint.sh; then
    pass "KB image Vault-ingests, scrubs, and re-execs before any unrelated child"
else
    fail "KB image must seal, scrub, and clean-reexec before spawning unrelated children"
fi

# Core images can contain request credentials. Disable them in the supervising
# shell and independently in both unprivileged long-lived planes so runuser can
# never re-enable credential-bearing crash persistence.
if grep -qF 'ulimit -c 0' ../deploy/container/server-entrypoint.sh &&
   [ "$(grep -cF "runuser -u aimee -- sh -c 'set -eu; ulimit -c 0" ../deploy/container/server-entrypoint.sh)" -eq 2 ] &&
   ! grep -qF 'aimee_enable_core_dumps' ../deploy/container/server-entrypoint.sh; then
    pass "server entrypoint disables credential-bearing core dumps in every plane"
else
    fail "server entrypoint can persist credential-bearing core dumps"
fi
if ! grep -qF 'tail -s 0.1 --pid=' ../deploy/container/server-entrypoint.sh &&
   sh tests/test_server_plane_supervisor.sh; then
    pass "server plane supervisor detects zombie exits without tail --pid"
else
    fail "server plane supervisor can deadlock on an exited zombie child"
fi

# The optional-module gate decides which processes attach to the bus. Both
# entrypoints must ship it, and it must honour AIMEE_MODULE_<ID> in BOTH
# directions -- an enable-only gate cannot turn anything off.
if sh tests/test_optional_modules.sh > /dev/null 2>&1 &&
   grep -qF 'apply_optional_modules server' ../deploy/container/server-entrypoint.sh &&
   grep -qF 'apply_optional_modules kb' ../deploy/container/aimee-kb-entrypoint.sh &&
   grep -qF 'optional-modules-lib.sh' ../Dockerfile.server &&
   grep -qF 'optional-modules-lib.sh' ../Dockerfile; then
    pass "operator can enable and disable optional modules in both placements"
else
    fail "optional-module gate is missing, one-directional, or not shipped in an image"
fi

if grep -q 'go|c' ../deploy/container/server-entrypoint.sh ||
   grep -q 'wfe_autonomy_register();' server/server.c ||
   grep -q 'wfe_scheduler_init();' server/server.c; then
    fail "C WFE runtime ownership is still reachable"
else
    pass "Go is the exclusive WFE runtime owner"
fi

if grep -qF '[ -d "$AIMEE_HOME/workflows" ] && chown -R aimee:aimee "$AIMEE_HOME/workflows"' \
    ../deploy/container/server-entrypoint.sh; then
    pass "server entrypoint makes the workflow registry writable by the Go WFE"
else
    fail "server entrypoint leaves the workflow registry root-owned"
fi

if grep -qF 'chown aimee:aimee "$AIMEE_HOME/modules.d"' \
        ../deploy/container/server-entrypoint.sh &&
   grep -qF 'chmod 0700 "$AIMEE_HOME/modules.d" "$AIMEE_HOME/modules.d/server"' \
        ../deploy/container/server-entrypoint.sh; then
    pass "server entrypoint keeps the private module policy traversable by the daemon"
else
    fail "server entrypoint leaves the private module policy inaccessible to the daemon"
fi

# Upgraded persistent volumes can spend tens of seconds recovering WAL state
# before the C resource socket appears.  The entrypoint must not kill a live
# child at the old 15-second deadline, and the same-binary OAuth prewarm helper
# must start only after the real server owns the persisted pid file.
wfe_wait_tenths=$(sed -n 's/^WFE_SOCKET_WAIT_TENTHS="${AIMEE_WFE_SOCKET_WAIT_TENTHS:-\([0-9][0-9]*\)}"$/\1/p' \
    ../deploy/container/server-entrypoint.sh)
server_start_line=$(grep -nF 'log "starting aimee-server (socket=$SERVER_SOCK) as user aimee"' \
    ../deploy/container/server-entrypoint.sh | cut -d: -f1)
prewarm_line=$(grep -nF 'runuser -u aimee -- aimee-server --prewarm-cli-oauth' \
    ../deploy/container/server-entrypoint.sh | cut -d: -f1)
wfe_start_line=$(grep -nF 'log "starting Go WFE control plane (socket=$AIMEE_WFE_HTTP_SOCKET)"' \
    ../deploy/container/server-entrypoint.sh | cut -d: -f1)
if [ -n "$wfe_wait_tenths" ] && [ "$wfe_wait_tenths" -ge 1200 ] &&
   [ -n "$server_start_line" ] && [ -n "$wfe_start_line" ] && [ -n "$prewarm_line" ] &&
   [ "$server_start_line" -lt "$wfe_start_line" ] && [ "$wfe_start_line" -lt "$prewarm_line" ]; then
    pass "server entrypoint tolerates volume recovery before launching same-binary helpers"
else
    fail "server entrypoint can time out recovery or let prewarm collide with a stale server pid"
fi

server_verify_deps="build-essential clang-format-19 libcurl4-openssl-dev libpam0g-dev libp11-kit-dev libpq-dev libsqlite3-dev libssl-dev libzstd-dev pkg-config postgresql-client python3 python3-yaml ripgrep zlib1g-dev"
missing_server_verify_deps=""
for dep in $server_verify_deps; do
    if ! awk -v dep="$dep" '/^FROM /{runtime=($0=="FROM debian:bookworm-slim"); found=0} runtime && index($0, dep){found=1} END{exit !found}' \
        ../Dockerfile.server; then
        missing_server_verify_deps="$missing_server_verify_deps $dep"
    fi
done
if [ -z "$missing_server_verify_deps" ] &&
   grep -qF 'ARG VERIFY_PG_MAJOR=18' ../Dockerfile.server &&
   grep -qF 'bookworm-pgdg main' ../Dockerfile.server &&
   grep -qF 'COPY --from=wfe-build /usr/local/go/ /usr/local/go/' ../Dockerfile.server &&
   grep -qF 'ENV PATH=/var/lib/aimee/.npm-global/bin:/usr/local/go/bin:$PATH' ../Dockerfile.server &&
   grep -qF 'ENV AIMEE_VERIFY_MAKE_JOBS=2' ../Dockerfile.server &&
   grep -qF 'ENV AIMEE_VERIFY_TEST_JOBS=2' ../Dockerfile.server; then
    pass "server runtime carries the complete workflow verification toolchain"
else
    fail "server runtime is missing workflow verification packages or Go 1.25:$missing_server_verify_deps"
fi

# `git verify` may invoke make with parallelism. The shipping-artifact build has
# to finish before lint's bus blast-radius gate inspects those binaries, or a
# healthy clean checkout fails nondeterministically on partial artifact coverage.
if grep -qF 'verify-local: all' Makefile &&
   sed -n '/^verify-local:/,/^[^[:space:]#].*:/p' Makefile | grep -qF '@$(MAKE) check-linking' &&
   sed -n '/^verify-local:/,/^[^[:space:]#].*:/p' Makefile | grep -qF '@$(MAKE) lint' &&
   ! grep -qE '^verify-local:.*lint' Makefile; then
    pass "verify-local builds shipping artifacts before lint inspection"
else
    fail "verify-local can race lint against a partial shipping build"
fi

# The extracted-repository pins record which published core/module repository
# commit each vendored mirror was cut from, which is a fact about a RELEASE. They
# bind on `main` alone: every other branch is meant to carry vendored source
# ahead of the lock, so running the check there only demands a lock refresh after
# each edit under src/core|modules/**. Guard the placement in both directions --
# absent from the every-branch gates, present in the workflow under a main-only
# condition -- so neither half can drift back on its own.
if ! sed -n '/^verify-local:/,/^[^[:space:]#].*:/p' Makefile |
     grep -qF 'python3 -I scripts/check_c_repository_lock.py' &&
   ! grep '^LINT_CHECKS = ' Makefile | grep -qF 'repository-lock-check' &&
   grep -B2 -F 'run: python3 -I scripts/check_c_repository_lock.py' \
        ../.github/workflows/c-repositories.yml |
     grep -qF "if: github.ref == 'refs/heads/main' || github.base_ref == 'main'"; then
    pass "extracted-repository source pins are enforced for main alone"
else
    fail "extracted-repository source pins are enforced off main, or not on it"
fi

# Verification runs inside the server image, whose deployment posture is
# expressed through AIMEE_* environment overrides. Those values are correct for
# the live daemon but must not override config fixtures in repository unit tests.
# Match in-shell rather than piping into grep -q. Under `set -o pipefail` such
# a pipeline reports the SIGPIPE that grep's early exit sends back to its
# writer, so the check starts failing purely because the recipe grew past a
# 4KiB pipe block -- a false red that says nothing about the overrides.
unit_tests_recipe=$(sed -n '/^unit-tests:/,/^$(TESTPREFIX)\/unit-test-util:/p' tests/Rules.mk)
go_unit_tests_recipe=$(sed -n '/^go-unit-tests:/,/^verify-local:/p' Makefile)
if [[ "$unit_tests_recipe" == *'unset AIMEE_HOME AIMEE_API_REMOTE_WRITES AIMEE_API_MTLS AIMEE_API_BEARER_TOKEN'* &&
      "$unit_tests_recipe" == *'AIMEE_SERVER_HTTP_BIND AIMEE_WORKSPACES_DIR AIMEE_KB_API_URL'* &&
      "$unit_tests_recipe" == *'AIMEE_KB_API_BEARER_TOKEN AIMEE_WFE_ENGINE AIMEE_WFE_HTTP_SOCKET'* &&
      "$go_unit_tests_recipe" == *'unset AIMEE_WFE_ENGINE AIMEE_WFE_HTTP_SOCKET'* ]]; then
    pass "unit verification removes server deployment overrides"
else
    fail "unit verification inherits server deployment overrides"
fi

case "$MODE" in
    default) echo "build-integrity:" ;;
    --build-variants) echo "build-variants:" ;;
    *)
        echo "usage: $0 [--build-variants]" >&2
        exit 2
        ;;
esac

# 1. No duplicate variable assignments in Makefile (catches overwritten vars)
# Skip lines inside else..endif blocks: those are conditional alternatives, not duplicates.
dupes=$(awk '/^else/{skip=1;next}/^endif/{skip=0;next}skip{next}/^[A-Z_]+ =/{print $1}' Makefile | sort | uniq -d)
if [ -z "$dupes" ]; then
    pass "no duplicate Makefile variable assignments"
else
    fail "duplicate Makefile variable assignments: $dupes"
fi

# 2. Every .c in CORE/DATA/AGENT/CMD_SRCS actually exists
for var in CORE_SRCS DATA_SRCS AGENT_SRCS CMD_SRCS CLI_SRCS SERVER_SRCS; do
    files=$(make -p 2>/dev/null | grep "^$var = " | sed "s/^$var = //" | tr ' ' '\n' | grep '\.c$')
    missing=""
    for f in $files; do
        [ -f "$f" ] || missing="$missing $f"
    done
    if [ -z "$missing" ]; then
        pass "$var: all source files exist"
    else
        fail "$var: missing files:$missing"
    fi
done

# 3. Every test source listed in TEST_TARGETS has a corresponding .c file
targets=$(make -p 2>/dev/null | grep "^TEST_TARGETS" | sed "s/^TEST_TARGETS[: ]*= //" | tr ' ' '\n')
missing_tests=""
for t in $targets; do
    # Targets look like "<prefix>/unit-test-foo" where prefix varies with OBJDIR.
    # Drop the prefix, strip "unit-test-", and map hyphens to underscores:
    #   .../unit-test-foo-bar -> tests/test_foo_bar.c
    name=$(basename "$t" | sed 's|^unit-test-||; s|-|_|g')
    src="tests/test_${name}.c"
    if [ ! -f "$src" ]; then
        # Some variant targets intentionally reuse the base test source with
        # different backing objects, e.g. unit-test-working-memory-mock. A
        # shell-backed TEST_TARGET is also valid when the rule installs the
        # matching script as its executable artifact.
        alt="${src%_mock.c}.c"
        shell_src="${src%.c}.sh"
        [ -f "$alt" ] || [ -f "$shell_src" ] || missing_tests="$missing_tests $src"
    fi
done
if [ -z "$missing_tests" ]; then
    pass "all TEST_TARGETS have source files"
else
    fail "missing test sources:$missing_tests"
fi

# GNU libc extensions used by tests must be requested before any system header.
# Debian's compiler intentionally hides declarations such as memmem otherwise,
# so keep the clean verifier from depending on ambient compiler flags.
for src in $(grep -rl --include='*.c' '\bmemmem[[:space:]]*(' tests 2>/dev/null); do
    feature_line=$(grep -n '^#define _GNU_SOURCE' "$src" | head -1 | cut -d: -f1)
    include_line=$(grep -n '^#include' "$src" | head -1 | cut -d: -f1)
    if [ -z "$feature_line" ] || [ -z "$include_line" ] || [ "$feature_line" -ge "$include_line" ]; then
        fail "$src uses memmem without declaring _GNU_SOURCE before system headers"
    fi
done
pass "memmem tests declare GNU extensions before system headers"

# Debian Bookworm's supported SQLite predates the string_agg alias. DB2 reads
# that run in both PostgreSQL and the SQLite test shim must not hide backend-
# specific aggregate syntax in this shared artifact listing path.
if ! grep -q '\bstring_agg[[:space:]]*(' db2/artifacts.c; then
    pass "artifact proposal listing is portable across PostgreSQL and Bookworm SQLite"
else
    fail "artifact proposal listing depends on SQLite-unsupported string_agg"
fi

# 4. Rules.mk: TEST_TARGETS continuation lines (detect missing backslash)
# Every non-last line of a multi-line variable must end with backslash
in_targets=0
line_num=0
bad_lines=""
while IFS= read -r line; do
    line_num=$((line_num + 1))
    if echo "$line" | grep -q "^TEST_TARGETS"; then
        in_targets=1
    fi
    if [ "$in_targets" = "1" ]; then
        # If line has content and does NOT end with \ but the next line
        # is indented (continuation), that is a missing backslash
        if echo "$line" | grep -qE '^\s+.*unit-test-' && ! echo "$line" | grep -q '\\$'; then
            in_targets=0  # this should be the last line
        fi
    fi
done < tests/Rules.mk

# 5. Every test target linking config.o must also link platform_random.o
# Join continuation lines, then check each target rule
bad_targets=$(sed ':a; /\\$/N; s/\\\n//; ta' tests/Rules.mk | grep 'unit-test-' | while IFS= read -r rule; do
    target=$(echo "$rule" | cut -d: -f1 | tr -d ' ')
    deps=$(echo "$rule" | cut -d: -f2-)
    if echo "$deps" | grep -q 'config\.o' && \
       ! echo "$deps" | grep -q 'platform_random\.o' && \
       ! echo "$deps" | grep -q 'TEST_CORE_OBJS\|TEST_DATA_OBJS\|CORE_OBJS\|PLATFORM_BASIC_OBJS'; then
        echo "$target"
    fi
done | tr '\n' ' ')
if [ -z "$bad_targets" ]; then
    pass "all test targets with config.o also link platform_random.o"
else
    fail "config.o without platform_random.o in: $bad_targets"
fi

# 6. Parse the Make database once, then verify every literal .PHONY declaration
# has a rule in the source. The previous implementation started a fresh
# `make -n` for every phony target; reparsing this large graph 100+ times took
# more than two minutes in CI. GNU make returns 1 from -q when the default goal
# is merely out of date, while 2 means the database itself could not be parsed.
make_db_rc=0
make -qp >/dev/null 2>&1 || make_db_rc=$?
if [ "$make_db_rc" -gt 1 ]; then
    fail "Make database parses cleanly"
else
    pass "Make database parses cleanly"
fi

# Join continuations once so multi-line .PHONY declarations and target rules
# can be compared by one awk process. This deliberately checks the source rule,
# not the database's synthetic target: declaring `.PHONY: missing` causes GNU
# make to manufacture a target named `missing`, which made `make -n missing`
# report success even when no corresponding rule existed.
missing_rules=$(
    {
        sed ':a; /\\$/ { N; s/\\\n/ /; ba }' Makefile
        sed ':a; /\\$/ { N; s/\\\n/ /; ba }' tests/Rules.mk
    } | awk '
        /^\.PHONY:[[:space:]]/ {
            line = $0
            sub(/^\.PHONY:[[:space:]]*/, "", line)
            count = split(line, names, /[[:space:]]+/)
            for (i = 1; i <= count; i++)
                if (names[i] != "")
                    phony[names[i]] = 1
            next
        }
        /^[^#[:space:]][^=]*:/ {
            line = $0
            sub(/:.*/, "", line)
            count = split(line, names, /[[:space:]]+/)
            for (i = 1; i <= count; i++)
                if (names[i] != "")
                    rules[names[i]] = 1
        }
        END {
            for (target in phony)
                if (!(target in rules))
                    print target
        }
    ' | sort | tr '\n' ' '
)
if [ -z "$missing_rules" ]; then
    pass "all .PHONY targets have rules"
else
    fail ".PHONY targets with no rule: $missing_rules"
fi

# 7. Scripts don't reference non-existent make targets
# Only match lines where 'make' is the command (start of line or after && / ; / |)
bad_script_targets=""
for script in ../update.sh ../install.sh ../setup.sh; do
    [ -f "$script" ] || continue
    # Extract lines where make is invoked as a command, then pull targets
    targets=$(grep -E '(^|[;&|]\s*)make\s' "$script" 2>/dev/null \
        | sed 's/.*make //' \
        | tr ' ' '\n' \
        | grep -vE '^-|^\$|^$|^>|^2>' \
        || true)
    for target in $targets; do
        if ! make -n "$target" >/dev/null 2>&1; then
            bad_script_targets="$bad_script_targets $script:$target"
        fi
    done
done
if [ -z "$bad_script_targets" ]; then
    pass "scripts reference only valid make targets"
else
    fail "scripts reference missing make targets:$bad_script_targets"
fi

# The cross-platform smoke exports both modern URL and legacy endpoint forms.
# Preserve the transport scheme: POSIX gives AIMEE_API_ENDPOINT precedence, so
# mapping an https:// URL to tcp: makes a healthy TLS server look unreachable.
smoke_tmp=$(mktemp -d "${TMPDIR:-/tmp}/aimee-smoke-endpoint.XXXXXX") || smoke_tmp=
smoke_endpoints_ok=0
if [ -n "$smoke_tmp" ]; then
    printf '%s\n' '#!/bin/sh' 'test "${AIMEE_API_ENDPOINT:-}" = "${EXPECTED_ENDPOINT:?}"' \
        > "$smoke_tmp/fake-aimee"
    chmod +x "$smoke_tmp/fake-aimee"
    smoke_endpoints_ok=1
    for smoke_case in 'http://example.test:8740|tcp:example.test:8740' \
                      'https://example.test:8743|tls:example.test:8743'; do
        smoke_url=${smoke_case%%|*}
        smoke_expected=${smoke_case#*|}
        if ! AIMEE_BIN="$smoke_tmp/fake-aimee" SERVER_URL="$smoke_url" \
             EXPECTED_ENDPOINT="$smoke_expected" FORCE_MODE=full BEARER=test-only \
             bash ../scripts/aimee-thin-client-smoke.sh >/dev/null 2>&1; then
            smoke_endpoints_ok=0
        fi
    done
    rm -rf "$smoke_tmp"
fi
if [ "$smoke_endpoints_ok" = "1" ]; then
    pass "thin-client smoke preserves HTTP/TLS endpoint schemes"
else
    fail "thin-client smoke endpoint scheme regression"
fi

# 7a. PreToolUse grep redirect hook must keep targeted file inspection unblocked.
if python3 ../scripts/test-redirect-grep-hook.py >/dev/null 2>&1; then
    pass "redirect grep hook classifier"
else
    fail "redirect grep hook classifier regression"
fi

# 7a2. Claude-style clients can report non-zero PreToolUse exits as hook
# failures, so Aimee guardrail denials must be structured hook output.
if grep -q 'permissionDecision", "deny"' ../src/cli_main.c &&
   grep -q 'permissionDecision", "deny"' ../src/cmd_hooks.c &&
   grep -q 'cli_hook_client_uses_pretool_json' ../src/cli_main.c &&
   grep -q 'hook_client_uses_pretool_json()) ? 0 : rc' ../src/cmd_hooks.c; then
    pass "PreToolUse guardrail denials are structured"
else
    fail "PreToolUse guardrail denial regression"
fi

hook_payload=$(printf '{"tool_name":"spawn_agent","tool_input":{"prompt":"x"},"cwd":"%s"}' "$(pwd)")
set +e
# Force the transport-failure path this regression exercises.  A developer may
# already have a healthy local server; without an explicit unreachable target,
# that server's policy response makes this test depend on host state.
hook_out=$(printf '%s' "$hook_payload" |
    AIMEE_HOOK_CLIENT=claude AIMEE_SERVER_URL=http://127.0.0.1:9 ../aimee hooks pre 2>/dev/null)
hook_rc=$?
set -e
if [ "$hook_rc" -eq 0 ] &&
   echo "$hook_out" | grep -q '"hookEventName":"PreToolUse"' &&
   echo "$hook_out" | grep -q '"permissionDecision":"deny"'; then
    pass "PreToolUse denials exit cleanly for Claude-style hooks"
else
    fail "PreToolUse denial hook output regression"
fi

check_updated_input_gate() {
    local file="$1"
    local gate="$2"
    awk -v gate="$gate" '
        index($0, gate) { seen_gate=1 }
        index($0, "cJSON_AddItemToObject(hook_out, \"updatedInput\"") {
            if (!seen_gate) exit 1
            seen_emit=1
        }
        END { exit (seen_gate && seen_emit) ? 0 : 1 }
    ' "$file"
}

if check_updated_input_gate ../src/cmd_hooks.c 'hook_client_supports_updated_input()' &&
   check_updated_input_gate ../src/cli_main.c 'cli_hook_client_supports_updated_input()' &&
   grep -q 'hook_client_supports_updated_input' ../src/cmd_hooks.c &&
   grep -q 'cli_hook_client_supports_updated_input' ../src/cli_main.c &&
   grep -q 'strcmp(client, "claude") == 0' ../src/cmd_hooks.c &&
   grep -q 'strcmp(client, "claude") == 0' ../src/cli_main.c &&
   grep -q 'emit_pretool_rewrite_unsupported_json' ../src/cmd_hooks.c &&
   grep -q 'emit_pretool_rewrite_unsupported_json' ../src/cli_main.c; then
    pass "PreToolUse updatedInput is gated to supported clients in all hook entry paths"
else
    fail "PreToolUse updatedInput client gating regression"
fi

if grep -q "if client_id == 'codex'" ../configure-hooks.sh &&
   grep -q "return cmd + ' || true'" ../configure-hooks.sh; then
    pass "Codex PreToolUse hook is advisory"
else
    fail "Codex PreToolUse hook may surface failed-hook noise"
fi

# 7b. Installer non-interactive prompts stay wrapped behind helper functions
if ./tests/test_install_noninteractive.sh >/dev/null 2>&1; then
    pass "install.sh non-interactive prompts are centralized"
else
    fail "install.sh non-interactive prompt regression"
fi

# 7c. Install and update paths must refresh the same shipped systemd user units.
missing_systemd_units=""
for unit_path in ../systemd/user/*; do
    [ -f "$unit_path" ] || continue
    unit=$(basename "$unit_path")
    for script in ../install.sh ../update.sh; do
        if ! grep -q "$unit" "$script"; then
            missing_systemd_units="$missing_systemd_units $script:$unit"
        fi
    done
done
if [ -z "$missing_systemd_units" ]; then
    pass "install/update scripts refresh all systemd user units"
else
    fail "install/update scripts miss systemd user units:$missing_systemd_units"
fi

# 7c2. update.sh must refresh hooks/support files even when binaries are current.
if awk '
    /configure-hooks\.sh/ { hook_seen = 1 }
    !hook_seen && /Already up to date|Binaries already up to date/ { current_msg = 1 }
    !hook_seen && current_msg && /^[[:space:]]*exit[[:space:]]+0([[:space:]]|$)/ { bad = 1 }
    END { exit bad ? 1 : 0 }
' ../update.sh; then
    pass "update.sh refreshes support files when binaries are current"
else
    fail "update.sh exits before refreshing support files"
fi

# 7d. Bootstrap scripts must not call hidden legacy client commands. The thin
# client only exposes routed/local commands, so install/update/setup should not
# depend on unadvertised init/setup paths.
hidden_bootstrap_calls=$(grep -REn \
    'aimee[[:space:]]+(init|setup)\b|init --quiet|Database initialized|memory stats' \
    ../install.sh ../update.sh ../setup.sh 2>/dev/null || true)
if [ -z "$hidden_bootstrap_calls" ]; then
    pass "bootstrap scripts avoid hidden client init/setup commands"
else
    fail "bootstrap scripts call hidden client commands:$hidden_bootstrap_calls"
fi

# 7e. CMake shipped targets must mirror the Makefile artifact boundaries. The
# legacy helper libraries intentionally still exist for tests, but DB-free
# client/webchat and DB1-only server targets must not link them transitively.
cmake_file="../CMakeLists.txt"
cmake_target_links() {
    awk -v target="$1" '
        $0 ~ "target_link_libraries\\(" target "[ \t)]" {
            in_block=1
        }
        in_block {
            print
        }
        in_block && /\)/ {
            in_block=0
        }
    ' "$cmake_file"
}
cmake_client_links=$(cmake_target_links aimee)
cmake_webchat_links=$(cmake_target_links aimee-runtime-web)
cmake_server_links=$(cmake_target_links aimee-server)
cmake_boundary_failures=""
for target_block in client webchat; do
    block_var="cmake_${target_block}_links"
    block="${!block_var}"
    if echo "$block" | grep -Eq 'aimee-(cmd|git|agent|data|core)([[:space:]]|[)]|$)|SQLite::SQLite3|LIBPQ|libpq'; then
        cmake_boundary_failures="$cmake_boundary_failures aimee-$target_block"
    fi
done
if echo "$cmake_server_links" | grep -Eq 'aimee-(cmd|git|agent|data|core)([[:space:]]|[)]|$)|LIBPQ|libpq'; then
    cmake_boundary_failures="$cmake_boundary_failures aimee-server"
fi
if [ -z "$cmake_boundary_failures" ]; then
    pass "CMake shipped targets avoid legacy DB-bearing static libraries"
else
    fail "CMake shipped target boundary regressions:$cmake_boundary_failures"
fi

# 7f. Makefile shipped targets must not keep DB2/libpq implementation objects
# in core, and KB's DB2 objects must compile with the shipped KB profile.
makefile_file="Makefile"
make_var_block() {
    awk -v var="$1" '
        $0 ~ "^" var "[[:space:]]*=" {
            in_block=1
        }
        in_block {
            print
        }
        in_block && $0 !~ /\\[[:space:]]*$/ {
            exit
        }
    ' "$makefile_file"
}
make_core_srcs=$(make_var_block CORE_SRCS)
make_server_data_objs=$(make_var_block SERVER_DATA_OBJS)
make_kb_target=$(grep -F '$(KB):' "$makefile_file" || true)
make_kb_compile_rule=$(grep -A2 -F '$(OBJDIR)/kb/%.o:' "$makefile_file" || true)
make_boundary_failures=""
if echo "$make_core_srcs" | grep -Fq 'db2/db_postgres.c'; then
    make_boundary_failures="$make_boundary_failures core-has-db2-postgres"
fi
if echo "$make_server_data_objs" | grep -Fq '$(DATA_OBJS)'; then
    make_boundary_failures="$make_boundary_failures server-links-generic-data-objs"
fi
if ! echo "$make_server_data_objs" | grep -Fq '$(OBJDIR)/server/'; then
    make_boundary_failures="$make_boundary_failures server-data-not-db2-disabled"
fi
if echo "$make_kb_target" | grep -Fq '$(DB2_OBJS)'; then
    make_boundary_failures="$make_boundary_failures kb-links-generic-db2-objs"
fi
if ! echo "$make_kb_target" | grep -Fq '$(KB_DB2_PG_OBJS)'; then
    make_boundary_failures="$make_boundary_failures kb-missing-kb-db2-postgres-objs"
fi
if ! echo "$make_kb_target" | grep -Fq '$(KB_DB2_OBJS)'; then
    make_boundary_failures="$make_boundary_failures kb-missing-kb-db2-objs"
fi
if ! echo "$make_kb_compile_rule" | grep -Fq 'AIMEE_DISABLE_DB2_SQLITE_SHIM'; then
    make_boundary_failures="$make_boundary_failures kb-db2-sqlite-shim-enabled"
fi
if [ -z "$make_boundary_failures" ]; then
    pass "Makefile DB objects are target-owned and shim-disabled"
else
    fail "Makefile DB boundary regressions:$make_boundary_failures"
fi

# 7f-bis. Header dependency tracking must cover every compiled object.
#
# DEPS was $(ALL_OBJS:.o=.d). Objects reachable only as a direct prerequisite of a
# test target were in no *_OBJS variable, so their .d files were never included:
# ~959 of 2033 objects had NO header tracking. Editing a header then left them
# stale, and a stale object built against an older struct layout links cleanly and
# then crashes on a field offset at runtime. That is how unit-test-memory came to
# segfault on a tree whose tests all "passed".
#
# Two assertions: the wiring is structurally correct, and -- when a build tree is
# present -- it empirically covers every .d on disk.
dep_failures=""
deps_assign=$(grep -E '^DEPS[[:space:]]*=' "$makefile_file" || true)
if [ -z "$deps_assign" ]; then
    dep_failures="$dep_failures deps-unassigned"
elif ! echo "$deps_assign" | grep -Fq '$(OBJDIR)'; then
    # A purely variable-derived DEPS can only ever cover hand-listed objects.
    dep_failures="$dep_failures deps-not-discovered-from-objdir"
fi

objdir=$(sed -n 's/^OBJDIR[[:space:]]*=[[:space:]]*\(.*\)$/\1/p' "$makefile_file" | head -1)
[ -n "$objdir" ] || objdir="build/obj"
if find "$objdir" -name '*.d' 2>/dev/null | grep -q .; then
    printf 'print-%%:\n\t@echo $($%s)\n' '*' > /tmp/aimee_bi_print.mk 2>/dev/null ||
        : # non-writable /tmp: the structural check above still stands
    if [ -f /tmp/aimee_bi_print.mk ]; then
        find "$objdir" -name '*.d' | sort -u > /tmp/aimee_bi_ondisk.txt
        make -f "$makefile_file" -f /tmp/aimee_bi_print.mk print-DEPS 2>/dev/null |
            tr ' ' '\n' | grep '\.d$' | sort -u > /tmp/aimee_bi_included.txt
        uncovered=$(comm -13 /tmp/aimee_bi_included.txt /tmp/aimee_bi_ondisk.txt | wc -l)
        if [ "$uncovered" -ne 0 ]; then
            dep_failures="$dep_failures $uncovered-objects-without-header-tracking"
        fi
        rm -f /tmp/aimee_bi_print.mk /tmp/aimee_bi_ondisk.txt /tmp/aimee_bi_included.txt
    fi
fi

if [ -z "$dep_failures" ]; then
    pass "every compiled object has header dependency tracking"
else
    fail "header dependency tracking gaps:$dep_failures"
fi

# 7g. The KB service split must keep explicit module-boundary directories and
# container packaging for the headless aimee-kb deployment shape.
split_failures=""
for d in kb server shared; do
    [ -d "$d" ] || split_failures="$split_failures missing-src-$d"
    [ -f "$d/README.md" ] || split_failures="$split_failures missing-src-$d-readme"
    find "$d" -maxdepth 1 -name '*.h' | grep -q . ||
        split_failures="$split_failures missing-src-$d-header"
done
[ -d kb/http ] || split_failures="$split_failures missing-src-kb-http"
[ -f kb/http/README.md ] || split_failures="$split_failures missing-src-kb-http-readme"
find kb/http -maxdepth 1 -name '*.h' | grep -q . ||
    split_failures="$split_failures missing-src-kb-http-header"
if [ -f ../Dockerfile ]; then
    if ! grep -Fq 'make -C src ../aimee-kb' ../Dockerfile; then
        split_failures="$split_failures dockerfile-not-building-aimee-kb"
    fi
    if grep -Eq 'aimee-server|DB1|db1/' ../Dockerfile; then
        split_failures="$split_failures dockerfile-links-server-or-db1"
    fi
    if ! grep -Fq '"postgresql-${PG_MAJOR}"' ../Dockerfile ||
       ! grep -Fq '"postgresql-${PG_MAJOR}-pgvector"' ../Dockerfile; then
        split_failures="$split_failures dockerfile-missing-embedded-postgres"
    fi
    if ! grep -Fq 'ENTRYPOINT ["/usr/local/bin/aimee-kb-entrypoint.sh"]' ../Dockerfile; then
        split_failures="$split_failures dockerfile-missing-kb-db-entrypoint"
    fi
else
    split_failures="$split_failures missing-dockerfile"
fi
if [ -f ../compose.yaml ]; then
    if ! grep -Eq '^[[:space:]]+aimee-kb:' ../compose.yaml; then
        split_failures="$split_failures compose-missing-aimee-kb-service"
    fi
    if grep -Eq '^[[:space:]]+postgres:' ../compose.yaml; then
        split_failures="$split_failures compose-retains-sibling-postgres-service"
    fi
    if grep -Eq 'AIMEE_DB2_URL[=:]' ../compose.yaml; then
        split_failures="$split_failures compose-persists-db2-url-outside-vault"
    fi
else
    split_failures="$split_failures missing-compose-yaml"
fi
if [ -z "$split_failures" ]; then
    pass "aimee-kb split module directories and container packaging exist"
else
    fail "aimee-kb split packaging regressions:$split_failures"
fi

# 7h. The retired chat frontends must stay removed.
#
# Codex went first; the OpenCode frontend, its native fallback loop and `aimee
# chat` followed — the whole interactive TUI surface is gone, and bare `aimee`
# prints usage instead of launching one. What used to sit here were ~20
# assertions that the OpenCode TUI was WIRED (prompt extraction, queued-turn
# acknowledgements, GlobalEvent envelopes, aggregateID sync). Those pinned code
# that no longer exists, so they are replaced by the inverse guard: the surface
# must not come back. cli_chat_stream.c keeps the headless /v1 streaming path
# that acp-serve needs, and it must stay terminal-free.
retired_frontend_refs=$(find . ../CMakeLists.txt -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.inc' -o -name 'Makefile' -o -name 'CMakeLists.txt' \) \
    ! -path './tests/test_build_integrity.sh' \
    -print0 | xargs -0 grep -En \
    'cli_tui_codex|codex_exec_tui|codex_ui_|AIMEE_CODEX_(FRONTEND|NATIVE)_BIN|codex-frontend|codex-branded|opencode_exec_tui|opencode_v2_|builtin_chat_loop|builtin_chat_native_loop|AIMEE_OPENCODE_BIN|cli_tui_opencode' \
    2>/dev/null || true)
if [ -z "$retired_frontend_refs" ]; then
    pass "Retired chat frontends (Codex, OpenCode, native TUI) are absent"
else
    fail "Retired chat frontend references remain:$retired_frontend_refs"
fi

route_drift=""
for platform_client in posix/cli_client.c windows/cli_client.c; do
    # The shared /v1 route table is its own translation unit (cli_v1_routes*.c),
    # linked into every client; both platforms pull its API via this header.
    if ! grep -q '#include "cli_v1_routes.h"' "$platform_client"; then
        route_drift="$route_drift $platform_client:missing-shared-routes"
    fi
    if grep -q 'rpc_routes\[\]' "$platform_client"; then
        route_drift="$route_drift $platform_client:local-route-table"
    fi
done
if [ -z "$route_drift" ]; then
    pass "platform clients share one RPC route table"
else
    fail "platform client RPC route drift:$route_drift"
fi

# 10. Source file line-count policy (mirrors the Makefile line-check):
#   <= 1000 lines : ideal
#   > 2000 lines  : warning — acceptable but should be addressed
#   > 2500 lines  : error   — must fix before merge
# tests/*.c are exempt (test suites are case-heavy), same as `make line-check`.
#
# LINE_EXEMPT: files permitted to exceed the hard limit. Consolidated
# subsystems (memory_logic.c, memory_advanced.c per memory-consolidation
# proposal) intentionally exceed the limit.
WARN_LINES=2000
ERROR_LINES=2500
LINE_EXEMPT="memory_logic.c memory_advanced.c"
oversized=""
warned=""
for f in *.c posix/*.c linux/*.c mac/*.c windows/*.c; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    skip=0
    for e in $LINE_EXEMPT; do
        if [ "$base" = "$e" ]; then skip=1; break; fi
    done
    [ "$skip" = "1" ] && continue
    lines=$(wc -l < "$f")
    if [ "$lines" -gt "$ERROR_LINES" ]; then
        oversized="$oversized $f($lines>$ERROR_LINES)"
    elif [ "$lines" -gt "$WARN_LINES" ]; then
        warned="$warned $f($lines)"
    fi
done
if [ -n "$warned" ]; then
    echo "  NOTE: files over $WARN_LINES lines (aim to reduce):$warned"
fi
if [ -z "$oversized" ]; then
    pass "all source files within $ERROR_LINES-line hard limit"
else
    fail "source files exceed $ERROR_LINES-line hard limit:$oversized"
fi

# 11. Layer boundary enforcement: lower layers must not include higher-layer headers.
# Architecture: Layer 0 (core) -> Layer 1 (data) -> Layer 2 (agent) -> Layer 3 (cmd/UI)
L1_HDRS="memory\.h|index\.h|extractors_extra\.h|rules\.h|tasks\.h|feedback\.h|guardrails\.h|worktree\.h|branch_ownership\.h|workspace\.h|working_memory\.h|agent_config\.h|trace_analysis\.h"
L2_HDRS="agent\.h|agent_protocol\.h|agent_exec\.h|agent_types\.h|agent_tools\.h|agent_eval\.h|agent_plan\.h|agent_coord\.h|agent_jobs\.h|agent_tunnel\.h|http_retry\.h|failover\.h"
L3_HDRS="commands\.h|dashboard\.h|cmd_branch\.h"

# Existing violations tracked for reduction (file:header).
# Sanctioned cross-layer dependencies: the verify gate (git_verify_ops.c, layer 0)
# loads the guardrails session_state_t to scope verification to the current
# project/session (#24). session_state_t is defined in guardrails.h, so reading it
# here is an intentional, reviewed dependency rather than new tech debt; relocating
# the struct out of guardrails is a separate refactor.
declare -A LAYER_EXEMPT=(
    ["git_verify_ops.c:guardrails.h"]=1
)

LAYER0_FILES="db.c db_migrations.c config.c util.c text.c render.c log.c dstr.c platform_random.c client_integrations.c mcp_tools.c git_verify.c git_verify_ops.c \
posix/platform_ipc.c posix/platform_path.c posix/platform_process.c posix/platform_random.c posix/util.c \
linux/platform_event.c linux/platform_ipc.c linux/platform_process.c \
mac/platform_event.c mac/platform_ipc.c mac/platform_process.c \
windows/platform_event.c windows/platform_ipc.c windows/platform_path.c windows/platform_process.c windows/platform_random.c windows/util.c"
LAYER1_FILES="memory.c memory_promote.c memory_context.c memory_scan.c memory_graph.c memory_advanced.c trace_analysis.c index.c extractors.c extractors_extra.c rules.c tasks.c feedback.c guardrails.c branch_ownership.c workspace.c working_memory.c agent_config.c"
LAYER2_FILES="agent.c agent_protocol.c agent_policy.c agent_context.c agent_plan.c agent_eval.c agent_eval_memory_support.c agent_coord.c agent_jobs.c agent_tools.c agent_tools_defs.c agent_http.c agent_fallback.c http_retry.c failover.c agent_tunnel.c"

layer_violations=""
check_layer_includes() {
    local file="$1" forbidden="$2"
    [ -f "$file" ] || return 0
    local bad
    bad=$(grep -oP '#include\s+"(headers/)?\K[^"]+' "$file" | grep -E "^($forbidden)$" || true)
    for h in $bad; do
        if [ -z "${LAYER_EXEMPT[$file:$h]:-}" ]; then
            layer_violations="$layer_violations $file->$h"
        fi
    done
}

for f in $LAYER0_FILES; do check_layer_includes "$f" "$L1_HDRS|$L2_HDRS|$L3_HDRS"; done
for f in $LAYER1_FILES; do check_layer_includes "$f" "$L2_HDRS|$L3_HDRS"; done
for f in $LAYER2_FILES; do check_layer_includes "$f" "$L3_HDRS"; done

if [ -z "$layer_violations" ]; then
    pass "no layer boundary violations (${#LAYER_EXEMPT[@]} exempt)"
else
    fail "layer boundary violations:$layer_violations"
fi

# ────── Parallel build groups ──────────────────────────────────────────────
# Checks 8, 9, 9b, 9c, 12, 13 each use an isolated OBJDIR with no shared
# filesystem state. Run them concurrently; buffer each group's output and
# replay in order so the log is deterministic.

PAR_TMPDIR=$(mktemp -d)
trap 'rm -rf "$PAR_TMPDIR"' EXIT

_par_run() {
    local name="$1"; shift
    (
        FAIL=0
        fail() { echo "  FAIL: $1"; FAIL=1; }
        pass() { echo "  PASS: $1"; }
        "$@"
        exit $FAIL
    ) > "$PAR_TMPDIR/$name.out" 2>&1 &
    echo $! > "$PAR_TMPDIR/$name.pid"
}

_par_collect() {
    local name="$1"
    wait "$(cat "$PAR_TMPDIR/$name.pid")"
    local rc=$?
    cat "$PAR_TMPDIR/$name.out"
    [ "$rc" = "0" ] || FAIL=1
}

_check_existing_shipped_artifacts() {
    local INTEG_BINARY="../aimee"
    local INTEG_WEBCHAT="../aimee-runtime-web"
    local INTEG_SERVER="../aimee-server"
    local INTEG_KB="../aimee-kb"
    local INTEG_GATEWAY="../aimee-gateway"
    local INTEG_BINARY_ABS
    INTEG_BINARY_ABS="$(pwd)/$INTEG_BINARY"

    local missing_shipped=""
    for f in "$INTEG_BINARY" "$INTEG_WEBCHAT" "$INTEG_SERVER" "$INTEG_KB" "$INTEG_GATEWAY"; do
        [ -x "$f" ] || missing_shipped="$missing_shipped $f"
    done
    if [ -z "$missing_shipped" ]; then
        pass "existing shipped artifacts are present"
    else
        fail "existing shipped artifacts missing:$missing_shipped (run make check-linking first)"
        return
    fi

    local legacy_artifacts=""
    for f in ../aimee-client ../aimee-worker ../aimee-mcp ../aimem \
             ../aimee-client.exe ../aimee-worker.exe ../aimee-mcp.exe ../aimem.exe; do
        [ -e "$f" ] && legacy_artifacts="$legacy_artifacts $f"
    done
    if [ -z "$legacy_artifacts" ]; then
        pass "legacy root artifacts are retired"
    else
        fail "legacy root artifacts remain:$legacy_artifacts"
    fi

    local client_help_leaks
    client_help_leaks=$("$INTEG_BINARY" help --all 2>&1 | \
        grep -E '\b(init|setup|config|verify|doctor|kb|database|DB[123]|aimee-kb|db|dashboard|migrate|export|import|eval|branch)\b' || true)
    if [ -z "$client_help_leaks" ]; then
        pass "client help exposes only routed storage-neutral commands"
    else
        fail "client help exposes unported/storage-internal terms: $client_help_leaks"
    fi

    local help_tmp help_output help_rc
    help_tmp=$(mktemp -d)
    help_output=$(cd "$help_tmp" && "$INTEG_BINARY_ABS" identity snapshot --help 2>&1)
    help_rc=$?
    if [ "$help_rc" -eq 0 ] &&
       grep -q "identity" <<< "$help_output" &&
       [ ! -e "$help_tmp/benchmarks/identity" ]; then
        pass "server-routed identity snapshot help has no side effects"
    else
        fail "identity snapshot --help should print help without writing a snapshot"
    fi
    rm -rf "$help_tmp"

    local provider_help_output provider_help_rc
    provider_help_output=$("$INTEG_BINARY_ABS" provider --help 2>&1)
    provider_help_rc=$?
    if [ "$provider_help_rc" -eq 0 ] &&
       grep -q "provider" <<< "$provider_help_output" &&
       grep -q -- "--all" <<< "$provider_help_output" &&
       ! grep -q "Unknown command" <<< "$provider_help_output"; then
        pass "server-routed provider help is client-side"
    else
        fail "provider --help should print client help instead of server errors"
    fi

    local storage_string_leaks=""
    local leaks
    leaks=$(strings "$INTEG_BINARY" 2>/dev/null | \
        grep -E 'DB[12]|(^|[^[:alnum:]_])db[12]([^[:alnum:]_]|$)|db[12]_|database|postgres|sqlite|Hybrid DB|db status|db check|db backup|db recover|db pragma' || true)
    if [ -n "$leaks" ]; then
        storage_string_leaks="$storage_string_leaks $INTEG_BINARY:$leaks"
    fi
    leaks=$(strings "$INTEG_WEBCHAT" 2>/dev/null | \
        grep -E 'aimee_db_|kb_client_|db1_|DB[12]_DISABLED' || true)
    if [ -n "$leaks" ]; then
        storage_string_leaks="$storage_string_leaks $INTEG_WEBCHAT:$leaks"
    fi
    if [ -z "$storage_string_leaks" ]; then
        pass "client/webchat binaries expose no DB-tier storage strings"
    else
        fail "client/webchat storage string leaks:$storage_string_leaks"
    fi

    local retired_artifact_string_leaks=""
    for bin in "$INTEG_BINARY" "$INTEG_WEBCHAT"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E "Run 'aimee'|aimee-worker|aimee-mcp|aimem" || true)
        if [ -n "$leaks" ]; then
            retired_artifact_string_leaks="$retired_artifact_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$retired_artifact_string_leaks" ]; then
        pass "client/webchat binaries expose no retired artifact instructions"
    else
        fail "client/webchat retired artifact string leaks:$retired_artifact_string_leaks"
    fi

    local retired_command_string_leaks=""
    for bin in "$INTEG_SERVER" "$INTEG_KB"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E "aimee agent reference|aimee agent test|aimee memory antipattern|aimee autopilot resume|Local commands \\(memory, index, rules, db\\)|aimee-worker|aimee-mcp|aimem" || true)
        if [ -n "$leaks" ]; then
            retired_command_string_leaks="$retired_command_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$retired_command_string_leaks" ]; then
        pass "server/kb binaries expose no retired command instructions"
    else
        fail "server/kb retired command string leaks:$retired_command_string_leaks"
    fi

    local unrouted_repair_string_leaks=""
    for bin in "$INTEG_BINARY" "$INTEG_WEBCHAT" "$INTEG_SERVER" "$INTEG_KB"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E 'aimee (doctor --fix|kb repair|memory repair --all)' || true)
        if [ -n "$leaks" ]; then
            unrouted_repair_string_leaks="$unrouted_repair_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$unrouted_repair_string_leaks" ]; then
        pass "shipped binaries avoid unrouted repair command guidance"
    else
        fail "shipped binaries suggest unrouted repair commands:$unrouted_repair_string_leaks"
    fi
}

_check_branchswitch_objdir_recreate() {
    # Regression check for Makefile branch-switch cleanup. Build one nested
    # object before and after a fake branch change instead of rebuilding all
    # shipped artifacts.
    local BSOBJ=build/obj-branchswitch-check
    local BSBRANCH=build/branchswitch-check-branch.txt
    local BSTARGET="$BSOBJ/posix/platform_path.o"
    rm -rf "$BSOBJ" "$BSBRANCH"
    if make "$BSTARGET" OBJDIR="$BSOBJ" BRANCH_FILE="$BSBRANCH" >/dev/null 2>&1; then
        echo "fake-previous-branch" > "$BSBRANCH"
        if make "$BSTARGET" OBJDIR="$BSOBJ" BRANCH_FILE="$BSBRANCH" >/dev/null 2>&1; then
            pass "branch-switch OBJDIR subdirectories are recreated"
        else
            fail "branch-switch object rebuild failed after OBJDIR cleanup"
        fi
    else
        fail "branch-switch object build failed"
    fi
    rm -rf "$BSOBJ" "$BSBRANCH"
}

_group_integ() {
    # 8. Clean build succeeds (compilation + link)
    # Use isolated OBJDIR/BINARY/SERVER to avoid clobbering parallel builds.
    INTEG_OBJDIR=build/obj-integrity
    INTEG_BINARY=build/aimee-integrity
    INTEG_SERVER=build/aimee-server-integrity
    INTEG_WEBCHAT=build/aimee-runtime-web-integrity
    INTEG_KB=build/aimee-kb-integrity
    INTEG_GATEWAY=build/aimee-gateway-integrity
    rm -rf "$INTEG_OBJDIR" "$INTEG_BINARY" "$INTEG_SERVER" "$INTEG_WEBCHAT" "$INTEG_KB" "$INTEG_GATEWAY"
    if make all OBJDIR=$INTEG_OBJDIR BINARY=$INTEG_BINARY SERVER=$INTEG_SERVER \
            WEBCHAT=$INTEG_WEBCHAT KB=$INTEG_KB GATEWAY=$INTEG_GATEWAY >/dev/null 2>&1; then
        pass "clean build succeeds"
    else
        fail "clean build failed"
    fi
    missing_shipped=""
    for f in "$INTEG_BINARY" "$INTEG_WEBCHAT" "$INTEG_SERVER" "$INTEG_KB" "$INTEG_GATEWAY"; do
        [ -x "$f" ] || missing_shipped="$missing_shipped $f"
    done
    if [ -z "$missing_shipped" ]; then
        pass "make all builds all shipped artifacts"
    else
        fail "make all missing shipped artifacts:$missing_shipped"
    fi
    INTEG_BINARY_ABS="$(pwd)/$INTEG_BINARY"
    legacy_artifacts=""
    for f in ../aimee-client ../aimee-worker ../aimee-mcp ../aimem \
             ../aimee-client.exe ../aimee-worker.exe ../aimee-mcp.exe ../aimem.exe; do
        [ -e "$f" ] && legacy_artifacts="$legacy_artifacts $f"
    done
    if [ -z "$legacy_artifacts" ]; then
        pass "legacy root artifacts are retired"
    else
        fail "legacy root artifacts remain:$legacy_artifacts"
    fi

    client_help_leaks=$("$INTEG_BINARY" help --all 2>&1 | \
        grep -E '\b(init|setup|config|verify|doctor|kb|database|DB[123]|aimee-kb|db|dashboard|migrate|export|import|eval|branch)\b' || true)
    if [ -z "$client_help_leaks" ]; then
        pass "client help exposes only routed storage-neutral commands"
    else
        fail "client help exposes unported/storage-internal terms: $client_help_leaks"
    fi

    help_tmp=$(mktemp -d)
    help_output=$(cd "$help_tmp" && "$INTEG_BINARY_ABS" identity snapshot --help 2>&1)
    help_rc=$?
    if [ "$help_rc" -eq 0 ] &&
       grep -q "identity" <<< "$help_output" &&
       [ ! -e "$help_tmp/benchmarks/identity" ]; then
        pass "server-routed identity snapshot help has no side effects"
    else
        fail "identity snapshot --help should print help without writing a snapshot"
    fi
    rm -rf "$help_tmp"

    provider_help_output=$("$INTEG_BINARY_ABS" provider --help 2>&1)
    provider_help_rc=$?
    if [ "$provider_help_rc" -eq 0 ] &&
       grep -q "provider" <<< "$provider_help_output" &&
       grep -q -- "--all" <<< "$provider_help_output" &&
       ! grep -q "Unknown command" <<< "$provider_help_output"; then
        pass "server-routed provider help is client-side"
    else
        fail "provider --help should print client help instead of server errors"
    fi

    doc_client_contract_leaks=$(grep -REn \
        'aimee[[:space:]]+(\+|doctor|init|setup|plan|implement|usage|work|workspace)\b|aimee[[:space:]]+memory[[:space:]]+(embed|history|reembed|repair|stats|supersede|maintain|verify|review)\b|aimee[[:space:]]+index[[:space:]]+(overview|blast-radius|scan)\b' \
        ../README.md ../docs/COMMANDS.md ../docs/agent.md ../docs/WORKSPACES.md ../docs/DELEGATES.md \
        ../docs/BENCHMARKS.md ../docs/embedder-sweep.md ../docs/STATUS.md ../src/README.md \
        ../benchmarks/locomo/EVAL_CONFIG.md ../benchmarks/longmemeval/EVAL_CONFIG.md \
        ../benchmarks/lora/README.md \
        2>/dev/null || true)
    if [ -z "$doc_client_contract_leaks" ]; then
        pass "user docs advertise only routed client commands"
    else
        fail "user docs advertise unported client commands:$doc_client_contract_leaks"
    fi

    doc_retired_artifact_leaks=$(grep -REn \
        '`aimee[[:space:]]+(doctor|init|setup|kb|migrate|db|eval|branch)\b|aimee-worker|aimee-mcp|aimem|aimee-client' \
        ../README.md ../docs/COMMANDS.md ../docs/agent.md ../docs/WORKSPACES.md ../docs/DELEGATES.md \
        ../docs/BENCHMARKS.md ../docs/embedder-sweep.md ../docs/STATUS.md ../src/README.md \
        ../benchmarks/locomo/EVAL_CONFIG.md ../benchmarks/longmemeval/EVAL_CONFIG.md \
        ../benchmarks/lora/README.md \
        2>/dev/null || true)
    if [ -z "$doc_retired_artifact_leaks" ]; then
        pass "user docs avoid retired aimee command artifacts"
    else
        fail "user docs reference retired command artifacts:$doc_retired_artifact_leaks"
    fi

    # The CLI client (aimee) is a DB-free thin wrapper — no DB libraries allowed.
    # aimee-runtime-web is now a full HTTP server process with its own SQLite session
    # store (PAM auth sessions, rate-limit state); it may link sqlite but must not
    # contain aimee DB1/DB2 API strings (aimee_db_, kb_client, etc.).
    storage_string_leaks=""
    for bin in "$INTEG_BINARY"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E 'DB[12]|(^|[^[:alnum:]_])db[12]([^[:alnum:]_]|$)|db[12]_|database|postgres|sqlite|Hybrid DB|db status|db check|db backup|db recover|db pragma' || true)
        if [ -n "$leaks" ]; then
            storage_string_leaks="$storage_string_leaks $bin:$leaks"
        fi
    done
    # Webchat may use sqlite for session storage but must not expose aimee DB APIs.
    webchat_aimee_db_leaks=$(strings "$INTEG_WEBCHAT" 2>/dev/null | \
        grep -E 'aimee_db_|kb_client_|db1_|DB[12]_DISABLED' || true)
    if [ -n "$webchat_aimee_db_leaks" ]; then
        storage_string_leaks="$storage_string_leaks $INTEG_WEBCHAT:$webchat_aimee_db_leaks"
    fi
    if [ -z "$storage_string_leaks" ]; then
        pass "client/webchat binaries expose no DB-tier storage strings"
    else
        fail "client/webchat storage string leaks:$storage_string_leaks"
    fi

    retired_artifact_string_leaks=""
    for bin in "$INTEG_BINARY" "$INTEG_WEBCHAT"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E "Run 'aimee'|aimee-worker|aimee-mcp|aimem" || true)
        if [ -n "$leaks" ]; then
            retired_artifact_string_leaks="$retired_artifact_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$retired_artifact_string_leaks" ]; then
        pass "client/webchat binaries expose no retired artifact instructions"
    else
        fail "client/webchat retired artifact string leaks:$retired_artifact_string_leaks"
    fi

    retired_command_string_leaks=""
    for bin in "$INTEG_SERVER" "$INTEG_KB"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E "aimee agent reference|aimee agent test|aimee memory antipattern|aimee autopilot resume|Local commands \\(memory, index, rules, db\\)|aimee-worker|aimee-mcp|aimem" || true)
        if [ -n "$leaks" ]; then
            retired_command_string_leaks="$retired_command_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$retired_command_string_leaks" ]; then
        pass "server/kb binaries expose no retired command instructions"
    else
        fail "server/kb retired command string leaks:$retired_command_string_leaks"
    fi

    unrouted_repair_string_leaks=""
    for bin in "$INTEG_BINARY" "$INTEG_WEBCHAT" "$INTEG_SERVER" "$INTEG_KB"; do
        leaks=$(strings "$bin" 2>/dev/null | \
            grep -E 'aimee (doctor --fix|kb repair|memory repair --all)' || true)
        if [ -n "$leaks" ]; then
            unrouted_repair_string_leaks="$unrouted_repair_string_leaks $bin:$leaks"
        fi
    done
    if [ -z "$unrouted_repair_string_leaks" ]; then
        pass "shipped binaries avoid unrouted repair command guidance"
    else
        fail "shipped binaries suggest unrouted repair commands:$unrouted_repair_string_leaks"
    fi

    rm -rf "$INTEG_OBJDIR" "$INTEG_BINARY" "$INTEG_SERVER" "$INTEG_WEBCHAT" "$INTEG_KB" "$INTEG_GATEWAY"
}

_group_lean() {
    # 9b. Lean build succeeds and meets size limit
    LEAN_BIN=build/aimee-lean-integrity
    LEAN_SRV=build/aimee-server-lean-integrity
    LEAN_WEB=build/aimee-runtime-web-lean-integrity
    LEAN_KB=build/aimee-kb-lean-integrity
    LEAN_GW=build/aimee-gateway-lean-integrity
    LEAN_OBJ=build/obj-lean-integrity
    rm -rf "$LEAN_OBJ" "$LEAN_BIN" "$LEAN_SRV" "$LEAN_WEB" "$LEAN_KB" "$LEAN_GW"
    if make all server OBJDIR=$LEAN_OBJ BINARY=$LEAN_BIN SERVER=$LEAN_SRV \
            WEBCHAT=$LEAN_WEB KB=$LEAN_KB GATEWAY=$LEAN_GW \
            EXTRA_C_FLAGS="-Os -g0 -ffunction-sections -fdata-sections" \
            EXTRA_L_FLAGS="-s -Wl,--gc-sections" >/dev/null 2>&1; then
        lean_size=$(stat -c%s "$LEAN_SRV" 2>/dev/null || stat -f%z "$LEAN_SRV" 2>/dev/null)
        limit=$((1500 * 1024))
        if [ "$lean_size" -le "$limit" ]; then
            pass "lean build succeeds and server binary ${lean_size} bytes <= 1.5MB"
        else
            fail "lean server binary ${lean_size} bytes exceeds 1.5MB limit"
        fi
    else
        fail "lean build failed"
    fi
    rm -rf "$LEAN_OBJ" "$LEAN_BIN" "$LEAN_SRV" "$LEAN_WEB" "$LEAN_KB" "$LEAN_GW"
}

_group_dynlink() {
    # 9c. Dynamic linking policy: system libraries must be dynamically linked.
    # Only checked on Linux (ldd) — macOS uses otool and Windows is intentionally static.
    if ! command -v ldd >/dev/null 2>&1; then
        pass "dynamic linking check: skipped (no ldd — non-Linux platform)"
        return
    fi
    # Rebuild with a dedicated OBJDIR so we have binaries to inspect
    DLOBJ=build/obj-dynlink
    DLBIN=build/aimee-dynlink
    DLSRV=build/aimee-server-dynlink
    DLWEB=build/aimee-runtime-web-dynlink
    DLKB=build/aimee-kb-dynlink
    DLGW=build/aimee-gateway-dynlink
    rm -rf "$DLOBJ" "$DLBIN" "$DLSRV" "$DLWEB" "$DLKB" "$DLGW"
    if make all server "$DLKB" OBJDIR=$DLOBJ BINARY=$DLBIN SERVER=$DLSRV \
            WEBCHAT=$DLWEB KB=$DLKB GATEWAY=$DLGW >/dev/null 2>&1; then
        dl_fail=0
        for bin in "$DLBIN" "$DLWEB" "$DLSRV" "$DLKB"; do
            if ldd "$bin" 2>&1 | grep -q 'not a dynamic executable'; then
                fail "$bin is statically linked (policy requires dynamic system libs)"
                dl_fail=1
            fi
        done
        # CLI client must not link any DB backend libraries.
        if ldd "$DLBIN" | grep -q 'libsqlite3'; then
            fail "$DLBIN: libsqlite3 linked into DB-free client binary"
            dl_fail=1
        fi
        if ldd "$DLBIN" | grep -q 'libpq'; then
            fail "$DLBIN: libpq linked into DB-free client binary"
            dl_fail=1
        fi
        # Webchat (full server) may use libsqlite3 for session storage but not libpq.
        if ldd "$DLWEB" 2>/dev/null | grep -q 'libpq'; then
            fail "$DLWEB: libpq linked into webchat binary"
            dl_fail=1
        fi
        # Server owns DB1 only: sqlite3 yes, libpq no.
        if ! ldd "$DLSRV" | grep -q 'libsqlite3'; then
            fail "aimee-server: libsqlite3 not dynamically linked"
            dl_fail=1
        fi
        if ldd "$DLSRV" | grep -q 'libpq'; then
            fail "aimee-server: libpq linked into DB1-only server"
            dl_fail=1
        fi
        if command -v readelf >/dev/null 2>&1 && readelf -Ws "$DLSRV" | grep -Eq 'db2_|PQ[A-Z]'; then
            fail "aimee-server: DB2 symbols present in DB1-only server"
            dl_fail=1
        fi
        if command -v nm >/dev/null 2>&1; then
            for server_db_free_obj in memory_maintenance.o memory_prospective.o memory_lifecycle.o memory_directives.o memory_health.o memory_context.o memory_graph.o memory_scan.o memory_episodes.o memory_improve.o index.o learning_router.o memory_conflict.o memory_logic.o memory_assemble.o kb.o memory_advanced.o memory_core.o; do
                if nm --undefined-only "$DLOBJ/server/$server_db_free_obj" 2>/dev/null | grep -Eq ' db2_'; then
                    fail "aimee-server: $server_db_free_obj references DB2"
                    dl_fail=1
                fi
            done
        fi
        # KB owns DB2 only (incl. pgvector): libpq yes, sqlite3 no.
        if ldd "$DLKB" | grep -q 'libsqlite3'; then
            fail "aimee-kb: libsqlite3 linked into DB2-only kb"
            dl_fail=1
        fi
        if ! ldd "$DLKB" | grep -q 'libpq'; then
            fail "aimee-kb: libpq not dynamically linked"
            dl_fail=1
        fi
        if command -v readelf >/dev/null 2>&1 && readelf -Ws "$DLKB" | grep -Eq 'db1_|sqlite3_'; then
            fail "aimee-kb: DB1/sqlite symbols present in DB2-only kb"
            dl_fail=1
        fi
        if command -v readelf >/dev/null 2>&1 && readelf -Ws "$DLKB" | grep -Eq 'kb_mgmt_status_provision|db2_management_status_provision'; then
            fail "aimee-kb: offline status-provisioner symbols present in runtime kb"
            dl_fail=1
        fi
        # Server must dynamically link ssl and crypto
        if ! ldd "$DLSRV" | grep -q 'libssl'; then
            fail "aimee-server: libssl not dynamically linked"
            dl_fail=1
        fi
        if ! ldd "$DLSRV" | grep -q 'libcrypto'; then
            fail "aimee-server: libcrypto not dynamically linked"
            dl_fail=1
        fi
        if [ "$dl_fail" = "0" ]; then
            pass "dynamic linking policy: client/webchat DB-free, server DB1-only, kb DB2-only"
        fi
    else
        fail "dynamic linking check: build failed"
    fi
    rm -rf "$DLOBJ" "$DLBIN" "$DLSRV" "$DLWEB" "$DLKB" "$DLGW"
}

_group_branchswitch() {
    # 12. Branch-switch build: OBJDIR subdirectories must be recreated after a branch change.
    # The branch-switch logic deletes OBJDIR; _DUMMY must run AFTER that block to recreate subdirs.
    # Without this ordering, gcc fails writing .d dependency files into non-existent directories.
    BSOBJ=build/obj-branchswitch
    BSBIN=build/aimee-branchswitch
    BSSRV=build/aimee-server-branchswitch
    BSWEB=build/aimee-runtime-web-branchswitch
    BSKB=build/aimee-kb-branchswitch
    BSGW=build/aimee-gateway-branchswitch
    BSBRANCH=build/branchswitch-branch.txt
    rm -rf "$BSOBJ" "$BSBIN" "$BSSRV" "$BSWEB" "$BSKB" "$BSGW" "$BSBRANCH"
    if make all server OBJDIR=$BSOBJ BINARY=$BSBIN SERVER=$BSSRV WEBCHAT=$BSWEB \
            KB=$BSKB GATEWAY=$BSGW BRANCH_FILE=$BSBRANCH >/dev/null 2>&1; then
        # Fake a prior branch so the next make triggers the branch-switch cleanup + re-mkdir
        echo "fake-previous-branch" > "$BSBRANCH"
        if make all server OBJDIR=$BSOBJ BINARY=$BSBIN SERVER=$BSSRV WEBCHAT=$BSWEB \
                KB=$BSKB GATEWAY=$BSGW BRANCH_FILE=$BSBRANCH >/dev/null 2>&1; then
            pass "build succeeds after simulated branch switch (subdirectories recreated)"
        else
            fail "build fails after simulated branch switch (missing OBJDIR subdirectories — _DUMMY must run after branch-switch block)"
        fi
    else
        fail "branch-switch test: initial build failed"
    fi
    rm -rf "$BSOBJ" "$BSBIN" "$BSSRV" "$BSWEB" "$BSKB" "$BSGW" "$BSBRANCH"
}

if [ "$MODE" = "--build-variants" ]; then
    _par_run integ       _group_integ
    _par_run lean        _group_lean
    _par_run dynlink     _group_dynlink
    _par_run bs          _group_branchswitch

    _par_collect integ
    _par_collect lean
    _par_collect dynlink
    _par_collect bs
else
    _check_existing_shipped_artifacts
    _check_branchswitch_objdir_recreate
fi

echo ""
# NOTE: `set -e` is active here, and `grep -c` exits 1 on zero matches -- hence
# `|| true` below. Without it this block silently ended the run.
# indexed -> embedded -> curated. The scan handler must NOT enqueue curator work:
# at that point the project is indexed and its vectors do not exist yet, so
# curation would race embedding. The enqueue belongs to the embed stage, gated on
# the project being fully embedded. It also kept a one-row-per-symbol INSERT on
# the synchronous path of /v1/code/scan (~173,000 rows, ~215s on a 4,018-file
# corpus, against a 300s client deadline), so a drift back here is both a
# correctness and a latency regression.
scan_enqueues=$(grep -c 'kb_curator_queue_code_units_for_project' ../src/kb/http/kb_http_code.c 2>/dev/null || true)
embed_enqueues=$(grep -c 'kb_curator_queue_code_units_for_project' ../src/modules/kb-synthesis/kb_curator_drain.c 2>/dev/null || true)
embed_gated=$(grep -c 'kb_code_embed_project_fully_embedded' ../src/modules/kb-synthesis/kb_curator_drain.c 2>/dev/null || true)
if [ "$scan_enqueues" -eq 0 ] && [ "$embed_enqueues" -ge 1 ] && [ "$embed_gated" -ge 1 ]; then
    pass "curator work is enqueued by the embed stage, not by the scan handler"
else
    fail "curation must be enqueued after embedding, not during scan (scan=$scan_enqueues, embed=$embed_enqueues, gated=$embed_gated)"
fi

# A hook aimee never registers is not a guard. `aimee hooks` implements the
# PreToolUse contract and require_aimee_git is ON by default, but the codex plugin
# shipped no hooks file at all -- so across the benchmark's aimee cells the agent
# made 98 shell `git` calls and zero calls to the aimee git tool. The manifest
# entry and the emitted file must both exist, and must name the same path.
hooks_decl=$(grep -c 'hooks/codex-hooks.json' ../src/client_integrations.c 2>/dev/null || true)
hooks_cmd=$(grep -c '%s hooks' ../src/client_integrations.c 2>/dev/null || true)
if [ "${hooks_decl:-0}" -ge 2 ] && [ "${hooks_cmd:-0}" -ge 1 ]; then
    pass "codex plugin registers a PreToolUse hook and emits the file it declares"
else
    fail "codex plugin must declare hooks/codex-hooks.json in the manifest AND write it (decl=$hooks_decl cmd=$hooks_cmd)"
fi

# ── No co-located aimee-server ──────────────────────────────────────────────
# aimee is a thin client against a REMOTE aimee-server running in its own
# container. There is no co-located topology. The client used to fall back to
# <aimee_home>/aimee-http.sock whenever no remote was configured, which meant a
# stray locally-started server silently captured the client and every answer
# described the wrong host. That failure is invisible at the call site -- the
# command succeeds -- so it needs a build-time guard, not a runtime one.
#
# The rule enforced here: the client may reach a co-located server only when an
# operator EXPLICITLY configures a unix: endpoint (which is how the CLI inside
# the server's own container talks to it). Discovery and silent fallback are
# banned.

# 1. The /v1 transport must not construct a local socket path of its own.
if grep -nE '"unix:%s/aimee-http\.sock"|/aimee-http\.sock"' cli_v1_routes_d.c >/dev/null 2>&1; then
    fail "cli_v1_routes_d.c builds a co-located aimee-http.sock path; the /v1 transport must use the configured endpoint only"
else
    pass "/v1 transport has no co-located socket path"
fi

# 2. Liveness must be about the CONFIGURED server, not "any server on this box".
if grep -nE 'cli_http_health_ok' -A6 posix/cli_client.c | grep -qE 'cli_http_sock_path'; then
    fail "cli_http_health_ok probes the local UDS; liveness must target the configured endpoint"
else
    pass "liveness probe targets the configured endpoint"
fi

# 2b. The SAME rule on Windows. The checks above read posix/cli_client.c only,
#     which is exactly how Windows kept both banned behaviours after the POSIX
#     cutover: it still probed the well-known socket AND auto-spawned
#     aimee-server.exe, while its own cli_start_server() told the user local
#     servers were unsupported. A guard that names one platform does not enforce
#     a rule; it enforces it there.
#     Comment lines are skipped so the note recording WHY this is banned (which
#     names the removed functions) does not trip its own guard.
if grep -nE 'spawn_server|AIMEE_NO_AUTOSTART' windows/cli_client.c \
        | grep -qvE '^[0-9]+: *(\*|/\*|//)'; then
    fail "windows/cli_client.c can still start a local aimee-server; the CLI is a pure client on every platform"
else
    pass "windows client never starts a local aimee-server"
fi

# 2c. And the same rule everywhere else. The two checks above name two files,
#     which is the very argument 2b makes against naming one platform: a guard
#     bound to a path enforces the rule at that path. AIMEE_NO_AUTOSTART is now
#     a dead name -- nothing reads it, because there is no autostart to
#     suppress -- so any C source mentioning it again means the behaviour came
#     back somewhere new. Tests are excluded: a harness may legitimately assert
#     the name is absent.
AUTOSTART_HITS=$(grep -rnE 'AIMEE_NO_AUTOSTART' --include='*.c' --include='*.h' . \
        | grep -vE '/tests/' | grep -vE ':[0-9]+: *(\*|/\*|//)' || true)
if [ -n "$AUTOSTART_HITS" ]; then
    fail "AIMEE_NO_AUTOSTART is read again, so something starts a local server: $AUTOSTART_HITS"
else
    pass "no source suppresses an autostart, because there is none to suppress"
fi

if grep -nE 'cli_existing_server_for_method' -A12 windows/cli_client.c | grep -qE 'cli_default_socket_path'; then
    fail "windows/cli_client.c still DISCOVERS a co-located server via the well-known socket; only an explicit AIMEE_SOCK is allowed"
else
    pass "windows client does not discover a co-located server"
fi

# 3. No user-facing message may tell an operator to start a server locally.
#    This is how the anti-pattern spread: the advice appeared exactly when
#    someone was stuck, and following it "fixed" the symptom.
#    Comment lines are skipped so the note explaining WHY this is banned (which
#    necessarily quotes the old advice) does not trip its own guard.
if grep -nE 'systemctl --user start aimee-server|aimee server start' cli_main.c \
        | grep -qvE '^[0-9]+: *(\*|/\*|//)'; then
    fail "cli_main.c still advises starting a local aimee-server"
else
    pass "no remediation text advises a local aimee-server"
fi

if [ "$FAIL" = "0" ]; then
    if [ "$MODE" = "--build-variants" ]; then
        echo "All build variant checks passed."
    else
        echo "All build integrity checks passed."
    fi
else
    if [ "$MODE" = "--build-variants" ]; then
        echo "Build variant checks FAILED."
    else
        echo "Build integrity checks FAILED."
    fi
    exit 1
fi
