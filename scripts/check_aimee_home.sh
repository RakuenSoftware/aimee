#!/usr/bin/env bash
# check_aimee_home.sh: lint gate for the aimee_home() audit.
#
# Per the aimee-profiles-and-usage-insights proposal, every code site
# that constructs a path under ~/.config/aimee/ must route through
# aimee_home() so the AIMEE_HOME / AIMEE_PROFILE overrides apply.
# This script greps for the legacy snprintf("%s/.config/aimee", home)
# pattern and fails on any new offender.
#
# Run via `make schema-sync-check`-style invocation; wired into the
# `lint` target.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src"

# Allowlist: the product path implementation, its platform facades, and the
# dependency-free core TLS path implementation. The latter must duplicate the
# same tiny environment contract because the extracted core package cannot
# import product code; its behavior is pinned by the TLS/profile tests.
ALLOWLIST=(
  "src/aimee_home.c"
  "src/aimee_home.h"
  "src/headers/aimee_home.h"
  "src/headers/platform_path.h"
  "src/posix/platform_path.c"
  "src/windows/platform_path.c"
  "src/core/connection/native_tls_path.c"
)

# Forbidden patterns:
# - snprintf-style format strings that splice "$HOME" + ".config/aimee"
#   together at runtime. Catches the common "%s/.config/aimee" and
#   "%s/.config/aimee/..." variants.
# - Direct platform_config_dir() callers outside the path abstraction layer.
PATTERN='snprintf[^;]*"%s/\.config/aimee|platform_config_dir\('

# Build a single egrep -v expression for the allowlist.
allowlist_filter() {
  local args=()
  for f in "${ALLOWLIST[@]}"; do
    args+=(-e "^$f:")
  done
  if [ ${#args[@]} -eq 0 ]; then
    cat
  else
    grep -v "${args[@]}" || true
  fi
}

cd "$ROOT"

offenders=$(grep -rnE "$PATTERN" src/ --include="*.c" --include="*.h" 2>/dev/null \
            | grep -v "test_" \
            | allowlist_filter || true)

if [ -n "$offenders" ]; then
  echo "check_aimee_home: found legacy ~/.config/aimee snprintf patterns:" >&2
  echo "$offenders" >&2
  echo "" >&2
  echo "These must route through aimee_home() (src/aimee_home.h) so the" >&2
  echo "AIMEE_HOME / AIMEE_PROFILE override applies. Replace:" >&2
  echo "  snprintf(buf, sz, \"%s/.config/aimee/<rest>\", getenv(\"HOME\"));" >&2
  echo "with:" >&2
  echo "  snprintf(buf, sz, \"%s/<rest>\", aimee_home());" >&2
  exit 1
fi

echo "check_aimee_home: ok"
