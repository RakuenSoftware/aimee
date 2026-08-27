#!/usr/bin/env python3
"""check-dispatch-caps.py: every NDJSON method in server_dispatch_table[] must
resolve an explicit capability policy in method_registry[].

A method that is dispatchable but absent from the registry falls through
server_capability_for_method() to CAPS_ALL (deny-by-default) — i.e. it becomes
silently UDS/local-trust-only by accident, unreachable over an authenticated or
scoped bearer. That is the "silent drop" class this gate forecloses: adding a
method to the dispatch table now *requires* a matching, intentional cap entry.

Pure static check (no build) — mirrors check-module-boundary.py et al.
"""
import argparse
import os
import re
import sys

DISPATCH_RE = re.compile(r'\{"([a-z0-9_.]+)",\s*handle_\w+\}')
# {"pattern", CAP_..., "desc"} — pattern may end with '*' for a prefix rule.
REGISTRY_RE = re.compile(r'\{"([a-z0-9_.*]+)",\s*[A-Z0-9_]+,')


def parse_dispatch(server_c):
    # The rows were split into server/server_dispatch_defs_data.h to keep
    # server.c under the line ceiling; read both so the table stays covered
    # wherever a row is declared.
    text = open(server_c, encoding="utf-8", errors="ignore").read()
    rows = os.path.join(os.path.dirname(server_c), "server_dispatch_defs_data.h")
    if os.path.exists(rows):
        text += open(rows, encoding="utf-8", errors="ignore").read()
    return DISPATCH_RE.findall(text)


def parse_registry(server_auth_c):
    pats = REGISTRY_RE.findall(open(server_auth_c, encoding="utf-8", errors="ignore").read())
    exact = {p for p in pats if not p.endswith("*")}
    prefixes = [p[:-1] for p in pats if p.endswith("*")]
    return exact, prefixes


def covered(method, exact, prefixes):
    if method in exact:
        return True
    return any(method.startswith(pre) for pre in prefixes)


def audit(methods, exact, prefixes):
    return [m for m in methods if not covered(m, exact, prefixes)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src-dir", default=".")
    ap.add_argument("--plant-test", action="store_true",
                    help="self-test: inject a fake uncovered method and confirm the audit flags it")
    args = ap.parse_args()

    server_c = f"{args.src_dir}/server/server.c"
    server_auth_c = f"{args.src_dir}/server/server_auth.c"

    methods = parse_dispatch(server_c)
    exact, prefixes = parse_registry(server_auth_c)
    if not methods or not exact:
        print("dispatch-caps: ERROR could not parse the dispatch table or method registry")
        return 1

    if args.plant_test:
        planted = audit(methods + ["zz.deliberately_uncovered_method"], exact, prefixes)
        if "zz.deliberately_uncovered_method" not in planted:
            print("dispatch-caps: ERROR plant-test failed — the gate did not flag a planted gap")
            return 1
        print("dispatch-caps plant: ok")
        return 0

    missing = audit(methods, exact, prefixes)
    if missing:
        print(f"dispatch-caps: ERROR {len(missing)} dispatched method(s) have no capability "
              "policy in method_registry[] (they silently require CAPS_ALL = UDS-only):")
        for m in missing:
            print(f"  - {m}")
        print("Add an explicit { \"method\", CAP_..., \"desc\" } entry in server/server_auth.c.")
        return 1

    print(f"dispatch-caps: ok ({len(methods)} methods, all covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
