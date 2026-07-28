#!/usr/bin/env python3
"""config_t is a secret of the config module. Nothing else should know its shape.

Two facts make this a correctness problem, not a style preference:

  * sizeof(config_t) is ~750 KB, dominated by fixed-size arrays (workspaces
    262 KB, cron_jobs 245 KB, mcp_clients 42 KB, ...). Every site that declares
    one as a local puts three quarters of a megabyte on the stack.
  * Those locals nest. The memory-search path stacks several across nested
    frames; a measured chain had consumed ~6 MB of an 8 MB stack, and adding a
    single 1.8 KB field to config_t segfaulted unit-test-memory-advanced inside
    config_load_file. The struct is one small field away from breaking for
    whoever adds the next one.

The fix is encapsulation: callers ask config for the value they need
(config_kb_enabled(), config_embedding_command(), ...) and never materialise the
struct. That is a large migration, so this check is a RATCHET rather than a
gate: the existing sites are recorded as a baseline, no new ones may appear, and
the baseline may only shrink. Migrate a file, run --update-baseline, commit.

Usage:
  check-config-encapsulation.py                 # enforce
  check-config-encapsulation.py --update-baseline
  check-config-encapsulation.py --plant-test    # prove the check can fail
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "scripts", "config-encapsulation-baseline.json")

# The config module owns the struct. Everything else — tests included — must go
# through accessors; test sites are in the baseline and shrink with the rest,
# since a 750 KB stack local is no safer in a test than in the server.
EXEMPT_PREFIXES = ("src/modules/config/",)

DECL = re.compile(r"^\s*(?:static\s+)?config_t\s+[a-z_][a-z0-9_]*\s*(?:\[[^\]]*\])?\s*;")
LOAD = re.compile(r"\bconfig_load(?:_file)?\s*\(")


def source_files(root):
    out = []
    for dirpath, dirnames, filenames in os.walk(os.path.join(root, "src")):
        dirnames[:] = [d for d in dirnames if d not in ("build", "vendor")]
        for fn in filenames:
            if not fn.endswith((".c", ".h")):
                continue
            path = os.path.relpath(os.path.join(dirpath, fn), root)
            if path.startswith(EXEMPT_PREFIXES):
                continue
            out.append(path)
    return sorted(out)


def scan(root):
    """Return {relpath: {"decls": n, "loads": n}} for files with any violation."""
    found = {}
    for path in source_files(root):
        try:
            with open(os.path.join(root, path), encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError:
            continue
        decls = sum(1 for ln in lines if DECL.match(ln))
        loads = sum(len(LOAD.findall(ln)) for ln in lines)
        if decls or loads:
            found[path] = {"decls": decls, "loads": loads}
    return found


def load_baseline():
    if not os.path.exists(BASELINE):
        return {}
    with open(BASELINE, encoding="utf-8") as fh:
        return json.load(fh).get("files", {})


def write_baseline(found):
    total_d = sum(v["decls"] for v in found.values())
    total_l = sum(v["loads"] for v in found.values())
    with open(BASELINE, "w", encoding="utf-8") as fh:
        json.dump(
            {
                "_note": (
                    "Ratchet baseline for check-config-encapsulation.py. config_t is a "
                    "secret of the config module; these are the sites that still "
                    "materialise it. Counts may only DECREASE. Migrate a file to config "
                    "accessors, then rerun with --update-baseline."
                ),
                "totals": {"files": len(found), "decls": total_d, "loads": total_l},
                "files": dict(sorted(found.items())),
            },
            fh,
            indent=2,
            sort_keys=True,
        )
        fh.write("\n")
    return total_d, total_l


def enforce(root):
    found = scan(root)
    base = load_baseline()
    regressions = []
    for path, counts in sorted(found.items()):
        allowed = base.get(path, {"decls": 0, "loads": 0})
        if counts["decls"] > allowed["decls"]:
            regressions.append(
                f"{path}: {counts['decls']} config_t declaration(s), baseline allows "
                f"{allowed['decls']}"
            )
        if counts["loads"] > allowed["loads"]:
            regressions.append(
                f"{path}: {counts['loads']} config_load() call(s), baseline allows "
                f"{allowed['loads']}"
            )

    # A file that improved should be re-baselined, so the ratchet cannot slip back.
    stale = []
    for path, allowed in sorted(base.items()):
        counts = found.get(path, {"decls": 0, "loads": 0})
        if counts["decls"] < allowed["decls"] or counts["loads"] < allowed["loads"]:
            stale.append(
                f"{path}: improved to {counts['decls']} decl / {counts['loads']} load "
                f"(baseline {allowed['decls']}/{allowed['loads']})"
            )

    if regressions:
        print("check-config-encapsulation: FAIL — new config_t exposure", file=sys.stderr)
        for r in regressions:
            print(f"  {r}", file=sys.stderr)
        print(
            "\n  config_t is ~750 KB and is stack-allocated by every one of these sites.\n"
            "  Ask config for the value instead (a config_<thing>() accessor); add one to\n"
            "  src/modules/config/ if it does not exist yet. See the header of this script.",
            file=sys.stderr,
        )
        return 1

    if stale:
        print(
            "check-config-encapsulation: FAIL — baseline is stale (this is good news)",
            file=sys.stderr,
        )
        for s in stale:
            print(f"  {s}", file=sys.stderr)
        print(
            "\n  Sites were migrated but the baseline still reserves room for them.\n"
            "  Run: python3 scripts/check-config-encapsulation.py --update-baseline",
            file=sys.stderr,
        )
        return 1

    total_d = sum(v["decls"] for v in found.values())
    total_l = sum(v["loads"] for v in found.values())
    print(
        f"check-config-encapsulation: ok ({len(found)} file(s) still expose config_t: "
        f"{total_d} declaration(s), {total_l} config_load() call(s); ratchet holding)"
    )
    return 0


def plant_test(root):
    """Prove the check actually fails on a new violation, not just that it passes."""
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(
            ["cp", "-r", os.path.join(root, "src"), os.path.join(root, "scripts"), tmp],
            check=True,
        )
        planted = os.path.join(tmp, "src", "planted_config_leak.c")
        with open(planted, "w", encoding="utf-8") as fh:
            fh.write("#include \"config.h\"\n"
                     "void planted(void)\n{\n   config_t cfg;\n   config_load(&cfg);\n}\n")
        rc = subprocess.run(
            [sys.executable, os.path.join(tmp, "scripts", "check-config-encapsulation.py")],
            capture_output=True,
            text=True,
            cwd=tmp,
        )
        if rc.returncode == 0:
            print(
                "check-config-encapsulation: PLANT TEST FAILED — a new config_t "
                "declaration did not trip the check",
                file=sys.stderr,
            )
            return 1
        if "planted_config_leak.c" not in rc.stderr:
            print(
                "check-config-encapsulation: PLANT TEST FAILED — check failed but did "
                f"not name the planted file:\n{rc.stderr}",
                file=sys.stderr,
            )
            return 1
    print("check-config-encapsulation: plant-test ok (a new leak is caught and named)")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--plant-test", action="store_true")
    ap.add_argument("--root", default=ROOT)
    args = ap.parse_args()

    if args.plant_test:
        return plant_test(args.root)
    if args.update_baseline:
        d, l = write_baseline(scan(args.root))
        print(f"check-config-encapsulation: baseline updated ({d} declaration(s), {l} load(s))")
        return 0
    return enforce(args.root)


if __name__ == "__main__":
    sys.exit(main())
