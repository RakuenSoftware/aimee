#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
scratch=$(mktemp -d /tmp/aimee-p5c2d-regressions.XXXXXX)
trap 'rm -rf -- "$scratch"' EXIT HUP INT TERM

compiler=${CC:-cc}
# Keep -UNDEBUG after inherited flags: these regression binaries intentionally
# use assert for syscall transcript checks and must never compile into no-ops.
common="${CFLAGS:-} -UNDEBUG -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I$repo_dir/src/headers -I$repo_dir/src/kb"

# shellcheck disable=SC2086
$compiler $common "$repo_dir/src/tests/test_kb_mgmt_token_authority_client_commit.c" \
  -lcrypto -lpthread -o "$scratch/client-commit"
# shellcheck disable=SC2086
$compiler $common "$repo_dir/src/tests/test_kb_mgmt_token_authority_daemon_stop.c" \
  -lcrypto -lpthread -o "$scratch/daemon-stop"

"$scratch/client-commit"
"$scratch/daemon-stop"

main="$repo_dir/src/kb/kb_mgmt_token_authority_main.c"
signal_line=$(awk '/sigaction\(SIGINT/ { print NR; exit }' "$main")
harden_line=$(awk '/kb_mgmt_token_authority_daemon_harden\(&daemon\)/ { print NR; exit }' "$main")
custody_line=$(awk '/vault_custody_set_provider/ { print NR; exit }' "$main")
unseal_line=$(awk '/vault_unseal\(/ { print NR; exit }' "$main")
test -n "$signal_line" && test -n "$harden_line" && test -n "$custody_line" && test -n "$unseal_line"
test "$signal_line" -lt "$harden_line"
test "$harden_line" -lt "$custody_line"
test "$harden_line" -lt "$unseal_line"

echo "p5c2d IPC commit/hardening/SIGTERM regressions: ok"
