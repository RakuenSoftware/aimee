#!/usr/bin/env bash
# test-check-deployed-image.sh: prove check-deployed-image.sh reaches the right
# verdict, including the one it used to get wrong.
#
# That check drives a 15-minute systemd timer on a production host, and it had no
# test. It also had a false positive that mattered: it built the expected image
# reference from the SERVICE name, so a host legitimately running
# aimee-kb-llm-e4b:testing was reported as drifted against aimee-kb:testing forever.
# A monitor that alarms constantly gets ignored, which costs exactly the two hours
# of stale deployment it exists to prevent.
#
# `docker` is stubbed rather than run: the point is the check's DECISION for a given
# registry/container state, and every state below (locally built, digest-pinned,
# recreated without a pull) is awkward to arrange for real and trivial to describe.
set -uo pipefail

CHECK="$(cd "$(dirname "$0")" && pwd)/check-deployed-image.sh"
[ -x "$CHECK" ] || { echo "FAIL: $CHECK not executable"; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin"

pass=0
fail=0

# Each case writes the world into these files, and the stub answers from them.
#   RUNNING_IMAGE  what .Config.Image reports for the container
#   RUNNING_ID     the image ID the container is actually running
#   LOCAL_ID       the image ID the local tag now resolves to
#   REPO_DIGEST    RepoDigests[0] for the local tag; empty = built here
cat > "$tmp/bin/docker" <<'STUB'
#!/usr/bin/env bash
. "$STATE"
case "$1" in
  ps)
    # Any filter matches: the check only uses this to learn the container name.
    echo "prod-${RUNNING_SVC}-1"
    ;;
  inspect)
    case "$*" in
      *.Config.Image*) echo "$RUNNING_IMAGE" ;;
      *'{{.Image}}'*)  echo "$RUNNING_ID" ;;
      *) exit 1 ;;
    esac
    ;;
  image)
    # docker image inspect <ref> --format ...
    case "$*" in
      *RepoDigests*)
        [ -n "$REPO_DIGEST" ] || exit 1
        echo "$REPO_DIGEST"
        ;;
      *'{{.Id}}'*)     echo "$LOCAL_ID" ;;
      *'{{.Created}}'*) echo "2026-08-02T00:00:00Z" ;;
      *) exit 1 ;;
    esac
    ;;
  *) exit 1 ;;
esac
STUB
chmod +x "$tmp/bin/docker"

# want_rc: expected exit status. want_re: regex the output must match.
run_case() {
   local name="$1" want_rc="$2" want_re="$3"
   local out rc
   # The server intentionally exports its deployed stack tag so managed compose
   # operations use the same immutable image. Keep that host environment out of
   # these fixed :testing fixtures or the test becomes deployment-dependent.
   out=$(PATH="$tmp/bin:$PATH" STATE="$tmp/state" \
         AIMEE_IMAGE_TAG=testing AIMEE_IMAGE_REPO=ghcr.io/rakuensoftware \
         bash "$CHECK" aimee-kb 2>&1)
   rc=$?
   if [ "$rc" != "$want_rc" ]; then
      echo "  FAIL  $name: exit $rc, expected $want_rc"
      echo "        output: $out"
      fail=$((fail + 1))
      return
   fi
   if ! printf '%s' "$out" | grep -Eq "$want_re"; then
      echo "  FAIL  $name: output did not match /$want_re/"
      echo "        output: $out"
      fail=$((fail + 1))
      return
   fi
   echo "  PASS  $name"
   pass=$((pass + 1))
}

state() {
   cat > "$tmp/state" <<EOF
RUNNING_SVC=aimee-kb
RUNNING_IMAGE=$1
RUNNING_ID=$2
LOCAL_ID=$3
REPO_DIGEST=$4
EOF
}

echo "==> check-deployed-image verdicts"

# The plain image, current. The baseline that always worked.
state ghcr.io/rakuensoftware/aimee-kb:testing sha256:aaa sha256:aaa \
      ghcr.io/rakuensoftware/aimee-kb@sha256:dddd
run_case "plain aimee-kb, current -> ok" 0 'ok \(ghcr.io/rakuensoftware/aimee-kb:testing'

# THE REGRESSION. A -llm variant is a legitimate on-channel choice, and naming the
# expected repository after the service reported it as drift against a repository
# this host was never meant to run.
state ghcr.io/rakuensoftware/aimee-kb-llm-e4b:testing sha256:bbb sha256:bbb \
      ghcr.io/rakuensoftware/aimee-kb-llm-e4b@sha256:eeee
run_case "aimee-kb-llm-e4b, current -> ok (not drift)" 0 \
         'ok \(ghcr.io/rakuensoftware/aimee-kb-llm-e4b:testing'

# A nomic variant is equally on-channel.
state ghcr.io/rakuensoftware/aimee-kb-nomic-llm-e2b:testing sha256:ccc sha256:ccc \
      ghcr.io/rakuensoftware/aimee-kb-nomic-llm-e2b@sha256:ffff
run_case "aimee-kb-nomic-llm-e2b, current -> ok" 0 'ok \('

# Drift mode 1: a local pin off the published repo entirely.
state aimee-kb:local sha256:ddd sha256:ddd ""
run_case "local pin off the repo -> PINNED" 1 'PINNED'

# Still drift mode 1, subtler: on the right repo, wrong channel. A published build
# of :testing can never reach this host.
state ghcr.io/rakuensoftware/aimee-kb:latest sha256:eee sha256:eee \
      ghcr.io/rakuensoftware/aimee-kb@sha256:1111
run_case "right repo, wrong tag -> OFF-CHANNEL" 1 'OFF-CHANNEL'

# An on-channel tag that was built here rather than pulled: no registry digest.
state ghcr.io/rakuensoftware/aimee-kb-llm-e4b:testing sha256:fff sha256:fff ""
run_case "built locally, no registry digest -> drift" 1 'no registry digest'

# Drift mode 2: recreated without a pull, so the tag moved and the container did not.
state ghcr.io/rakuensoftware/aimee-kb-llm-e4b:testing sha256:old sha256:new \
      ghcr.io/rakuensoftware/aimee-kb-llm-e4b@sha256:2222
run_case "recreated without a pull -> STALE" 1 'STALE'

echo
echo "==> Summary: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
echo "check-deployed-image: all verdicts correct"
