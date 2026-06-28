#!/usr/bin/env bash
# dev-verify-runner.sh — autonomous-dev-execution-substrate §1 ephemeral runner.
#
# Runs ONE verify step (a make target, e.g. "make -j$(nproc) all server",
# "make unit-tests", "make lint") for a work-item worktree inside a throwaway,
# sandboxed container started from the pinned aimee-dev-runner image, and prints a
# structured verdict matching git_verify's format=json contract:
#   {schema_version, verdict, reason, tier, step, exit, log}
#
# verdict: passed | failed | unavailable. "unavailable" (runner unreachable, image
# missing, toolchain pin drift, timeout, OOM) is kept DISTINCT from a real pass, so
# a missing/broken runner is never read as verified (the §1 false-pass rule). The
# autonomous driver fail-closes on unavailable.
#
# Sandbox posture: ephemeral (--rm), no host network (--network=none), all caps
# dropped, no-new-privileges, non-root (uid 1001), memory/PID limits, wall-clock
# timeout. The tree is shipped in via `git archive HEAD` (NO .git, so no vaulted
# remote tokens), re-seeded as a throwaway one-commit repo so git-dependent lint
# gates work. No host bind of secrets, no docker.sock.
#
# Runner host: AIMEE_RUNNER_SSH=user@host routes docker over ssh (homelab default:
# docker lives on pve). Set it empty to use a local `docker`.
set -uo pipefail

IMAGE="${AIMEE_RUNNER_IMAGE:-aimee-dev-runner:latest}"
RUNNER_SSH="${AIMEE_RUNNER_SSH-root@192.168.1.253}"
MEM="${AIMEE_RUNNER_MEM:-6g}"
PIDS="${AIMEE_RUNNER_PIDS:-1024}"
TIMEOUT="${AIMEE_RUNNER_TIMEOUT:-1800}" # wall-clock seconds for the step
HERE="$(cd "$(dirname "$0")" && pwd)"
PIN="$HERE/dev-runner-pin.json"

TREE="." STEP="" TIER="mechanical" PRINT_PIN=0
while [ $# -gt 0 ]; do
   case "$1" in
   --tree) TREE="$2"; shift 2 ;;
   --step) STEP="$2"; shift 2 ;;
   --tier) TIER="$2"; shift 2 ;;
   --image) IMAGE="$2"; shift 2 ;;
   --print-pin) PRINT_PIN=1; shift ;;
   -h | --help) sed -n '2,30p' "$0"; exit 0 ;;
   *) echo "dev-verify-runner: unknown arg '$1'" >&2; exit 2 ;;
   esac
done

# Run a shell command STRING on the runner host (local or over ssh). The command
# is one argument, so its own quoting survives a single remote shell parse.
# run_host closes stdin (-n / /dev/null) — vital under command substitution, where
# an ssh that inherits the script's stdin blocks forever. run_host_pipe forwards
# stdin, for the one call that streams a tar in (`git archive | run_host_pipe`).
run_host() {
   if [ -n "$RUNNER_SSH" ]; then
      ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$RUNNER_SSH" "$1"
   else
      bash -c "$1" </dev/null
   fi
}
run_host_pipe() {
   if [ -n "$RUNNER_SSH" ]; then
      ssh -o BatchMode=yes -o ConnectTimeout=10 "$RUNNER_SSH" "$1"
   else
      bash -c "$1"
   fi
}

emit() { # verdict reason [exit] [logfile]
   local verdict="$1" reason="$2" ec="${3:-0}" logf="${4:-}"
   python3 - "$verdict" "$reason" "$ec" "$TIER" "$STEP" "$logf" <<'PY'
import json, sys
verdict, reason, ec, tier, step, logf = sys.argv[1:7]
log = ""
if logf:
    try:
        log = open(logf, encoding="utf-8", errors="replace").read()[-8192:]
    except OSError:
        pass
print(json.dumps({"schema_version": 1, "verdict": verdict, "reason": reason,
                  "tier": tier, "step": step, "exit": int(ec), "log": log}))
PY
   [ "$verdict" = "passed" ] && exit 0 || exit 1
}

# Probe the running image's toolchain (one dpkg-query line per package).
probe_versions() {
   run_host "docker run --rm --network=none $IMAGE dpkg-query -W gcc make pkg-config clang-format-19 libsqlite3-dev libcurl4-openssl-dev libpam0g-dev libssl-dev libpq-dev libzstd-dev postgresql-client python3" 2>/dev/null
}

if [ "$PRINT_PIN" = 1 ]; then
   probe_versions || { echo "dev-verify-runner: cannot probe image '$IMAGE'" >&2; exit 1; }
   exit 0
fi

[ -n "$STEP" ] || { echo "dev-verify-runner: --step is required" >&2; exit 2; }

# Runner reachable?
run_host "docker version >/dev/null 2>&1" || emit unavailable runner-unreachable 1

# Pin check: the running image's toolchain must match dev-runner-pin.json; a
# mismatch (or missing image) is unavailable/pin-mismatch, never a pass.
probe="$(probe_versions)"
[ -n "$probe" ] || emit unavailable image-missing 1
mismatch="$(PIN="$PIN" PROBE="$probe" python3 <<'PY'
import json, os
pin = json.load(open(os.environ["PIN"]))["toolchain"]
got = {}
for line in os.environ["PROBE"].splitlines():
    if "\t" in line:
        k, v = line.split("\t", 1)
        got[k.split(":")[0]] = v
bad = [f"{pkg}: expected {want}, got {got.get(pkg)}" for pkg, want in pin.items() if got.get(pkg) != want]
print("\n".join(bad))
PY
)"
if [ -n "$mismatch" ]; then
   echo "PIN_CHECK_FAIL:" >&2
   echo "$mismatch" >&2
   emit unavailable pin-mismatch 1
fi

# Stage on the runner host: git archive (no .git -> no vault tokens), extract,
# re-seed a throwaway one-commit repo (so git-dependent gates work), drop the step
# script in (base64 to dodge all quoting), own it to the container uid.
STAGE="/tmp/aimee-runner-$$-$RANDOM"
STEP_B64="$(printf '%s' "$STEP" | base64 -w0)"
cleanup() { run_host "rm -rf $STAGE" >/dev/null 2>&1 || true; }
trap cleanup EXIT
if ! git -C "$TREE" archive HEAD 2>/dev/null | run_host_pipe "set -e; rm -rf $STAGE; mkdir -p $STAGE; tar x -C $STAGE; cd $STAGE; git init -q; git -c user.email=r@r -c user.name=r add -A; git -c user.email=r@r -c user.name=r commit -qm snapshot >/dev/null; echo $STEP_B64 | base64 -d > .runner-step.sh; chown -R 1001:1001 $STAGE"; then
   emit unavailable stage-failed 1
fi

# Run the step in the sandbox. The docker command is a fixed string (the step
# lives in .runner-step.sh), so nothing user-controlled is parsed by the remote
# shell. The step records its OWN exit code to /work/.runner-rc because some
# container runtimes (e.g. the homelab LXC `docker run`) do NOT propagate the
# container's exit code to the docker client — so the docker/ssh rc is unreliable
# and we read the recorded one instead. An ABSENT .runner-rc means the step was
# killed before it could record (wall-clock `timeout` or an OOM kill) -> unavailable.
LOG="$(mktemp)"
trap 'rm -f "$LOG"; cleanup' EXIT
run_host "docker run --rm --network=none --cap-drop=ALL --security-opt=no-new-privileges --memory=$MEM --pids-limit=$PIDS --user 1001:1001 -v $STAGE:/work -w /work/src $IMAGE timeout $TIMEOUT bash -lc 'bash /work/.runner-step.sh; echo \$? > /work/.runner-rc'" >"$LOG" 2>&1
rc="$(run_host "cat $STAGE/.runner-rc 2>/dev/null" | tr -dc '0-9')"

if [ -z "$rc" ]; then
   emit unavailable runner-killed 137 "$LOG" # timeout / OOM / runtime kill
elif [ "$rc" = 0 ]; then
   emit passed ok 0 "$LOG"
else
   emit failed step-failed "$rc" "$LOG"
fi
