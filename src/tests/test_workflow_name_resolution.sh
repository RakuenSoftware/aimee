#!/bin/sh
# `aimee workflow show/validate` must accept the name `aimee workflow list` prints.
#
# `list` prints bare filenames out of $AIMEE_HOME/workflows ("build.yaml"), while
# show and validate opened their argument relative to the CWD. So the obvious
# composition -- read a name out of `list`, hand it to `show` -- answered
#
#     workflow: cannot open 'build.yaml'
#
# for a file that plainly exists and that `list` had just printed as valid. Two
# commands in the same family disagreeing about what a name means is the kind of
# thing you work around rather than report, so it can sit there indefinitely.
#
# The fourth case is the one that keeps the fix honest: a BROKEN file of the same
# name in the CWD must still report its own parse error. Resolving to the good
# copy in AIMEE_HOME instead would mean an operator editing a workflow could see
# "ok" while reading a stale file -- a far worse bug than the one being fixed.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
bin="$root/aimee"
[ -x "$bin" ] || { echo "workflow-name-resolution: no built aimee at $bin" >&2; exit 1; }

tmp=$(mktemp -d)
cleanup() { rm -rf "$tmp"; }
trap 'cleanup' EXIT HUP INT TERM

mkdir -p "$tmp/workflows"
cat > "$tmp/workflows/demo.yaml" <<'YAML'
name: demo
start: draft
nodes:
  - id: draft
    block: author.proposal
YAML

fails=0
check() {
    if [ "$2" = "$3" ]; then
        printf '  ok    %s\n' "$1"
    else
        printf '  FAIL  %s\n     expected: %s\n     actual:   %s\n' "$1" "$2" "$3" >&2
        fails=$((fails + 1))
    fi
}

cd "$tmp"
AIMEE_HOME="$tmp"; export AIMEE_HOME

# 1. list prints the bare name.
out=$("$bin" workflow list 2>&1 || true)
case "$out" in *demo.yaml*) r=listed ;; *) r=absent ;; esac
check "list prints the workflow name" "listed" "$r"

# 2. show accepts exactly that name.
out=$("$bin" workflow show demo.yaml 2>&1 || true)
case "$out" in *"name: demo"*) r=shown ;; *) r=$(printf '%s' "$out" | head -1) ;; esac
check "show accepts the listed name" "shown" "$r"

# 3. validate accepts it too (same loader).
out=$("$bin" workflow validate demo.yaml 2>&1 || true)
case "$out" in *valid*) r=valid ;; *) r=$(printf '%s' "$out" | head -1) ;; esac
check "validate accepts the listed name" "valid" "$r"

# 4. A same-named file in the CWD wins, and its own error is reported. The
#    fallback must never mask what the operator is actually editing.
printf 'name: broken\n' > "$tmp/demo.yaml"
out=$("$bin" workflow show demo.yaml 2>&1 || true)
case "$out" in
*"no 'nodes'"*) r=local_error ;;
*"name: demo"*) r=masked_by_home ;;
*)              r=$(printf '%s' "$out" | head -1) ;;
esac
check "a local file of the same name is not masked" "local_error" "$r"

# 5. An explicit path keeps working unchanged.
out=$("$bin" workflow show "$tmp/workflows/demo.yaml" 2>&1 || true)
case "$out" in *"name: demo"*) r=shown ;; *) r=$(printf '%s' "$out" | head -1) ;; esac
check "an explicit path still resolves" "shown" "$r"

# 6. A name that exists nowhere still fails, naming what was asked for.
out=$("$bin" workflow show no-such-workflow.yaml 2>&1 || true)
case "$out" in *"no-such-workflow.yaml"*) r=named ;; *) r=$(printf '%s' "$out" | head -1) ;; esac
check "an unknown name is reported as asked for" "named" "$r"

if [ "$fails" -ne 0 ]; then
    printf 'workflow-name-resolution: %d failure(s)\n' "$fails" >&2
    exit 1
fi
printf 'workflow-name-resolution: all passed\n'
