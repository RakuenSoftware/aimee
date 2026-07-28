#!/usr/bin/env python3
"""config_t is a secret of the config module. NOTHING outside it may name the type.

The target is ZERO mentions outside src/modules/config/ — not fewer, not only in
headers, not "pointers are fine". A `const config_t *` parameter is a leak: the
caller includes config.h, learns the struct's shape, and can dereference any of
its 653 fields. Everything a caller needs comes from an accessor in the config
module that takes no config_t.

The baseline below is DEBT, not permission.

Two facts make this a correctness problem, not a style preference:

  * sizeof(config_t) is ~750 KB, dominated by fixed-size arrays (workspaces
    262 KB, cron_jobs 245 KB, mcp_clients 42 KB, ...). Every site that declares
    one as a local puts three quarters of a megabyte on the stack.
  * Those locals nest. The memory-search path stacks several across nested
    frames; a measured chain had consumed ~6 MB of an 8 MB stack, and adding a
    single 1.8 KB field to config_t segfaulted unit-test-memory-advanced inside
    config_load_file. The struct is one small field away from breaking for
    whoever adds the next one.

The fix is accessors: callers ask config for the value they need
(config_kb_enabled(), config_embedding_command_current(), ...) and never name the
type. That migration is large, so this check is a RATCHET rather than a gate:
existing mentions are recorded as debt, no new ones may appear, and the number
may only fall. Migrate a file, run --update-baseline, commit. Done means the
baseline is empty and EXEMPT_PREFIXES is the only place config_t appears.

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

# Every mention of the type is a leak, not just a stack declaration. A
# `const config_t *cfg` parameter still forces the caller to include config.h,
# still exposes all 653 fields to dereference, and still makes the struct's
# shape part of the module's interface. Counting only `config_t x;` measured
# the stack cost while missing the encapsulation break entirely — the pointer
# form is the more common leak (464 of the mentions) and the harder one to
# remove, because it is baked into signatures.
TYPE = re.compile(r"\bconfig_t\b")
LOAD = re.compile(r"\bconfig_load(?:_file)?\s*\(")


COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def strip_comments(text):
    """Blank out comments, preserving newlines so counts stay per-file honest."""
    return COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)


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
        # Strip comments first: naming the type while EXPLAINING why a site
        # still holds one is not itself a leak, and counting it punishes the
        # rationale that makes remaining debt reviewable.
        code = strip_comments("".join(lines))
        mentions = len(TYPE.findall(code))
        loads = len(LOAD.findall(code))
        if mentions or loads:
            found[path] = {"decls": mentions, "loads": loads}
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
                f"{path}: {counts['decls']} config_t mention(s), baseline allows "
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
            "\n  config_t is a secret of the config module. Naming the type at all —\n"
            "  including as `const config_t *` — exposes its shape and its ~750 KB.\n"
            "  Ask config for the value instead (a config_<thing>() accessor taking no\n"
            "  config_t); add one to src/modules/config/ if it does not exist yet.",
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
        f"check-config-encapsulation: ok ({len(found)} file(s) still name config_t: "
        f"{total_d} mention(s), {total_l} config_load() call(s); ratchet holding)"
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
            # A POINTER parameter only — no declaration, no config_load. This is
            # the case the first version of this check missed entirely.
            fh.write("#include \"config.h\"\n"
                     "int planted(const config_t *cfg);\n"
                     "int planted(const config_t *cfg)\n{\n   return cfg != 0;\n}\n")
        rc = subprocess.run(
            [sys.executable, os.path.join(tmp, "scripts", "check-config-encapsulation.py")],
            capture_output=True,
            text=True,
            cwd=tmp,
        )
        if rc.returncode == 0:
            print(
                "check-config-encapsulation: PLANT TEST FAILED — a new config_t "
                "POINTER PARAMETER did not trip the check",
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
    print("check-config-encapsulation: plant-test ok (a pointer-only leak is caught and named)")
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
