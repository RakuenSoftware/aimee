#!/usr/bin/env python3
"""Gate: every git/forge credential resolution routes through the ONE policy.

Aimee handles git/forge credentials centrally:
`git_cred_inject_build_env_for_repo()` (src/server/git_cred_inject.c) is the
single resolver — preferred/broker → per-host server vault → webuser vault →
server identity — so the precedence can never drift between call sites and a
single point exists to audit/extend. No downstream caller may hand-roll the
credential ladder.

This gate fails if any source file OUTSIDE the policy itself calls a low-level
credential-ladder primitive:

  git_host_resolve_token            (the per-host vault rung)
  forge_cred_build_server_env       (server-identity git env builder)
  forge_cred_build_env_from_token   (token-to-git-env builder)
  forge_cred_build_env              (broker-token git env builder)

Callers must instead call git_cred_inject_build_env_for_repo() /
git_cred_inject_build_env() and exec under the returned envp. Tests and the
files that DEFINE these primitives (or the policy that legitimately composes
them) are exempt. Run as part of `make lint`.

  --plant-test  inject a known-bad call in-memory and confirm the check fails,
                proving the gate is wired and not vacuously passing.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# Primitives that must live only inside the one policy.
BANNED = (
    "git_host_resolve_token",
    "forge_cred_build_server_env",
    "forge_cred_build_env_from_token",
    "forge_cred_build_env",
)
# A call: NAME immediately followed by '(' (not a definition prototype, which we
# allow only in the exempt files anyway).
CALL_RE = re.compile(r"\b(" + "|".join(BANNED) + r")\s*\(")

# Files allowed to name these primitives: the policy that composes them, the
# files that define them, and the per-host-vault seam. Paths are repo-relative.
EXEMPT = {
    "src/server/git_cred_inject.c",   # THE policy — composes the rungs
    "src/server/git_host_resolve.c",  # defines git_host_resolve_token
    "src/forge_credentials.c",        # defines the forge_cred_build_* primitives
}


def _is_test(rel):
    return "/tests/" in rel or rel.startswith("src/tests/")


def scan_text(rel, text):
    """Yield (rel, lineno, symbol) for each banned call in non-exempt code."""
    if rel in EXEMPT or _is_test(rel):
        return
    for i, line in enumerate(text.splitlines(), 1):
        # ignore comment-only lines so prose mentioning a symbol doesn't trip it
        stripped = line.lstrip()
        if stripped.startswith("*") or stripped.startswith("//") or stripped.startswith("/*"):
            continue
        m = CALL_RE.search(line)
        if m:
            yield (rel, i, m.group(1))


def main():
    plant = "--plant-test" in sys.argv

    if plant:
        bad = "   char **e = forge_cred_build_server_env(environ, shim);\n"
        hits = list(scan_text("src/server/some_new_caller.c", bad))
        if not hits:
            print("check-git-cred-centralized: FAIL — plant-test did not catch a "
                  "hand-rolled credential call")
            return 1
        print("check-git-cred-centralized: plant-test ok (hand-rolled call detected)")
        return 0

    # Scan .c translation units only — credential-ladder *calls* live there; the
    # headers merely declare the API surface (the point is no .c calls them).
    failures = []
    scanned = 0
    for f in sorted(SRC.rglob("*.c")):
        rel = f.relative_to(ROOT).as_posix()
        if rel in EXEMPT or _is_test(rel):
            continue
        scanned += 1
        failures += list(scan_text(rel, f.read_text(encoding="utf-8", errors="replace")))

    if failures:
        print(f"check-git-cred-centralized: FAIL — {len(failures)} hand-rolled "
              f"git-credential call(s) outside the one policy:")
        for rel, ln, sym in failures:
            print(f"  {rel}:{ln}: {sym}(")
        print("  Fix: resolve credentials via git_cred_inject_build_env_for_repo() "
              "and exec under the returned envp. Aimee handles git creds centrally; "
              "no downstream caller resolves a token.")
        return 1

    print(f"check-git-cred-centralized: ok (no hand-rolled credential calls in "
          f"{scanned} source file(s); all route through git_cred_inject)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
