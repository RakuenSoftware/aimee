#!/usr/bin/env python3
"""Fail on a config_t that is read before anything initialises it.

This exists because the compiler does not catch it. `config_t cfg;` followed by
`cfg.field` is a read of uninitialised stack, but config_t is ~750 KB of mostly
char arrays and gcc's -Wmaybe-uninitialized gives up on it -- five such sites
built clean at -Werror and passed the full suite.

Every one of them came from the same edit: removing

    if (config_load(&cfg) != 0)
       return -1;

as a "guard that can't fail". Deleting those two lines also deletes the
config_load INSIDE the condition, which is the only thing that filled the struct.
The mistake is easy to make and invisible afterwards, so it gets a check.

A declaration is fine if the first thing that touches the variable is
config_load, config_load_file, memset, config_snapshot_get, or
config_snapshot_init. Anything else reaching it first is an error.

The config module and tests are exempt: config.c owns the struct, and tests
legitimately build one by hand.
"""
import re
import subprocess
import sys
from pathlib import Path

INIT = re.compile(
    r"(config_load|config_load_file|memset|config_snapshot_get|config_snapshot_init)\s*\(\s*&%s\b"
)
DECL = re.compile(r"^\s*config_t\s+(\w+)\s*;\s*$")


def repo_root():
    return Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )


def sources(root):
    """Paths are anchored at the repo root, not the cwd -- this runs from src/."""
    out = subprocess.run(
        ["git", "ls-files", "src/**/*.c", "src/*.c"],
        cwd=root,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    return [
        p
        for p in out
        if not p.startswith("src/modules/config/") and not p.startswith("src/tests/")
    ]


def main():
    root = repo_root()
    scanned = 0
    findings = []
    for rel in sources(root):
        path = root / rel
        if not path.exists():
            continue
        scanned += 1
        lines = path.read_text(errors="replace").split("\n")
        for i, line in enumerate(lines):
            m = DECL.match(line)
            if not m:
                continue
            name = m.group(1)
            init = re.compile(INIT.pattern % re.escape(name))
            use = re.compile(rf"\b{re.escape(name)}\s*(\.|->)|&{re.escape(name)}\b")
            for j in range(i + 1, min(i + 400, len(lines))):
                nxt = lines[j]
                if init.search(nxt):
                    break
                if use.search(nxt):
                    findings.append((rel, i + 1, name))
                    break
                if re.match(r"^\}", nxt):  # left the function without a use
                    break

    if findings:
        print("check-config-uninitialized: FAIL")
        for rel, line, name in findings:
            print(f"  {rel}:{line}: '{name}' is read before it is initialised")
        print(
            "\n  A config_t must be filled by config_load/config_load_file/memset/"
            "\n  config_snapshot_get/config_snapshot_init before anything reads it."
            "\n  If you removed a `if (config_load(&x) != 0)` guard, you removed the"
            "\n  config_load with it -- see this script's docstring."
        )
        return 1

    if scanned == 0:
        print("check-config-uninitialized: FAIL -- scanned 0 files (bad path resolution)")
        return 1

    print(f"check-config-uninitialized: ok ({scanned} file(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
