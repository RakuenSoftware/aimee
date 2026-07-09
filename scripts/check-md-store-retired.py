#!/usr/bin/env python3
"""check-md-store-retired: the legacy `.md` agent-memory mirror subsystem was
retired (Proposal 2, §2 Slice 5). Intercepted memory now lands ONLY in db1 as a
private, non-recallable archive row (via db1_user_memory_upsert); there is no
`.md` re-materialization, no harness_memory mirror table, and no
`/v1/harness_memory/*` RPC surface.

This guard fails if any of the retired symbols, RPC methods, routes, or files
reappear in the C sources — so the retirement cannot silently regress. It is the
"CI guard enforces it" half of the proposal's Removal acceptance criterion.

Allowlist note: the RETAINED interception infra (harness_memory_common /_scope
/_audit /_spill and their hmem_* helpers — hmem_audit, hmem_spill_write,
hmem_resolve_project, hmem_project_key_ok, hmem_scope_for_client, hmem_sha256_hex,
hmem_content_hash) is still used by the replacement write path (memory_redirect)
and is intentionally NOT forbidden here.

Enforced as part of `make lint`. See docs/proposals/done/
memory-db1-db2-architecture.md §2.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Files that must stay deleted (the legacy .md-mirror subsystem).
FORBIDDEN_FILES = [
    "src/db1/harness_memory.c",
    "src/db1/harness_memory.h",
    "src/harness_memory_hydrate.c",
    "src/harness_memory_hydrate.h",
    "src/harness_memory_watch.c",
    "src/harness_memory_watch.h",
    "src/server/harness_memory_routes.c",
]

# Retired tokens that must not reappear in the C sources. The retired mirror
# TABLE api (never the retained common/scope/audit/spill helpers), the retired
# hydrate/watch modules, the .md re-materializer, and the retired RPC surface.
FORBIDDEN_RE = re.compile(
    r"\bhmem_(?:upsert|get|list|search|tombstone|tombstone_prefix|page_end|"
    r"row_t|rows_free|row_free_fields)\b"
    r"|\bharness_memory_(?:hydrate|watch)\b"
    r"|\bmemory_redirect_rematerialize\b"
    r"|\"harness_memory\.(?:upsert|get|list|search|tombstone|tombstone_prefix)\""
    r"|/v1/harness_memory/"
)


def c_sources(root):
    src = os.path.join(root, "src")
    for base, _dirs, files in os.walk(src):
        for f in files:
            if f.endswith((".c", ".h")):
                yield os.path.join(base, f)


def main():
    violations = []

    for rel in FORBIDDEN_FILES:
        if os.path.exists(os.path.join(ROOT, rel)):
            violations.append(f"{rel}: retired legacy .md-mirror file has reappeared")

    for path in c_sources(ROOT):
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for i, line in enumerate(text.splitlines(), 1):
            if FORBIDDEN_RE.search(line):
                rel = os.path.relpath(path, ROOT)
                violations.append(f"{rel}:{i}: retired .md-store token: {line.strip()[:100]}")

    if violations:
        print("check-md-store-retired: FAIL — the retired .md memory subsystem "
              "must not return (Proposal 2 §2 Slice 5):", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        return 1

    print("check-md-store-retired: ok (no retired .md-store symbols/routes/files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
