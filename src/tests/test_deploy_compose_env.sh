#!/bin/sh
# The managed compose `.env` is derived from config at every container start.
#
# WHY THIS EXISTS. A managed deployment's identity -- which kb image variant,
# which embedder -- lived only in the running container's Config.Env, put there
# by whichever shell first ran compose. Rebooting is safe: restart=unless-stopped
# restarts the SAME container object with its environment intact. Recreating is
# not, and recreating is what every image upgrade does. `docker compose up -d`
# from a caller whose environment lacks the variables reinterpolates them:
#
#   EMBEDDER_MODEL   unset -> the kb refuses to serve. Loud, recoverable.
#   AIMEE_KB_VARIANT unset -> ${AIMEE_KB_VARIANT:+-${AIMEE_KB_VARIANT}} resolves
#                             to the EMBEDDERLESS aimee-kb image. Silent.
#
# The second is the one worth a test. Nothing fails, nothing logs; the deployment
# simply stops having an embedder. This was hit for real recreating aimee-kb on a
# live host, and the resulting image swap from aimee-kb-a25m to aimee-kb went
# unnoticed until the container's own refusal message named a DIFFERENT cause.
#
# The invariant asserted here is the one that makes recreate equal reboot: after
# a start, the compose project directory carries a .env from which compose can
# rebuild the same topology with no help from the caller's environment.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
entrypoint="$root/deploy/container/server-entrypoint.sh"
tmp=$(mktemp -d)
cleanup() { rm -rf "$tmp"; }
trap 'cleanup' EXIT HUP INT TERM

fails=0
check() {
    if [ "$2" = "$3" ]; then
        printf '  ok    %s\n' "$1"
    else
        printf '  FAIL  %s\n     expected: %s\n     actual:   %s\n' "$1" "$2" "$3" >&2
        fails=$((fails + 1))
    fi
}

[ -r "$entrypoint" ] || { echo "deploy-compose-env: missing $entrypoint" >&2; exit 1; }

# 1. The entrypoint must derive the file rather than assume an inherited env.
grep -q -- '--emit-deploy-env' "$entrypoint" \
    && r=yes || r=no
check "entrypoint derives the env from config" "yes" "$r"

grep -q 'DEPLOY_ENV_DIR' "$entrypoint" && r=yes || r=no
check "entrypoint targets the compose project directory" "yes" "$r"

# 2. It must be written atomically. A half-written .env is worse than none:
#    compose would interpolate a truncated variable set and silently build the
#    wrong topology, which is the exact failure this whole mechanism prevents.
grep -q 'mv -f "$DEPLOY_ENV_DIR/.env.tmp" "$DEPLOY_ENV_DIR/.env"' "$entrypoint" \
    && r=yes || r=no
check "written via a temp file and renamed into place" "yes" "$r"

# 3. A failure to write must NOT stop the server. The file is a safety net for
#    later compose callers; the server's own deploy path builds its child
#    environment directly and works without it. Refusing to boot over it would
#    trade a silent recreate bug for a loud outage.
awk '/--emit-deploy-env/,/^fi$/' "$entrypoint" | grep -q 'WARNING' && r=yes || r=no
check "a write failure warns rather than aborting the start" "yes" "$r"

# 4. No secret may reach the file. config_emit_deploy_env omits the API keys by
#    design and check-vault-only-container-env enforces it; the managed-inference
#    bearer is added to the deploy child's envp in C, never emitted. Assert the
#    entrypoint does not reintroduce one by hand.
if awk '/DEPLOY_ENV_DIR=/,/^fi$/' "$entrypoint" | grep -qiE 'API_KEY|SECRET|TOKEN|PASSWORD'; then
    r=leaked
else
    r=clean
fi
check "no credential is written to the compose env file" "clean" "$r"

# 5. The emitted env itself must carry the variable whose absence is silent.
#    Run the real emitter against a config that selects the bundled model.
server_bin="$root/aimee-server"
if [ -x "$server_bin" ]; then
    home="$tmp/home"
    mkdir -p "$home"
    printf 'embedder_model: bekko-a25m\n' > "$home/aimee.yaml"
    env_out=$(AIMEE_HOME="$home" "$server_bin" --emit-deploy-env 2>/dev/null || true)

    case "$env_out" in
    *AIMEE_KB_VARIANT=a25m*) r=a25m ;;
    *AIMEE_KB_VARIANT=*)     r=other ;;
    *)                       r=absent ;;
    esac
    check "emitted env pins the kb image variant" "a25m" "$r"

    case "$env_out" in
    *EMBEDDER_MODEL=bekko-a25m*) r=set ;;
    *)                           r=missing ;;
    esac
    check "emitted env carries the selected embedder" "set" "$r"

    # An empty variant would resolve the image to the embedderless aimee-kb, so
    # "present but blank" is a failure, not a pass.
    case "$env_out" in
    *"AIMEE_KB_VARIANT="[!a-z]*|*"AIMEE_KB_VARIANT=") r=blank ;;
    *) r=nonblank ;;
    esac
    check "the variant is never emitted blank for a bundled model" "nonblank" "$r"
else
    printf '  skip  emitter cases (no built aimee-server at %s)\n' "$server_bin"
fi

if [ "$fails" -ne 0 ]; then
    printf 'deploy-compose-env: %d failure(s)\n' "$fails" >&2
    exit 1
fi
printf 'deploy-compose-env: all passed\n'
