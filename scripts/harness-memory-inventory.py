#!/usr/bin/env python3
"""Read-only inventory of the .md harness-memory store, for the db1/db2 memory
migration (Proposal 2, memory-arch retirement — Slice 1).

This is the ONLY zero-data-loss first increment of the .md retirement: it WRITES
NOTHING and DELETES NOTHING. It produces the manifest the operator needs to
classify each memory as user (-> db1) vs org (-> db2) before any migration runs,
because — per the design roundtable — scope is NOT mechanically derivable from
the legacy schema (harness_memory.type is {fact,index,note,scratch}, and
auto-defaulting either way is a silent, irreversible leak-or-lockaway).

It inventories the LOCAL .md files (their frontmatter `metadata.type` is the
scope signal) and, best-effort, reconciles against the canonical server
harness_memory table via the existing read-only /v1/harness_memory/list route,
flagging drift (server-only / local-only / present-in-both). Reconciliation is
by name; content divergence is left for operator spot-check (the exact
content_hash is a length-prefixed server-side tuple not replicated here).

Usage:
  harness-memory-inventory.py [--memory-dir DIR] [--project KEY] [--json]
  harness-memory-inventory.py --worklist   # CSV worklist for operator classification

`--worklist` emits the operator classification worklist (Proposal 2 §2 Slice 5,
step 1 gate) as CSV on stdout: one row per .md memory with path, frontmatter
type, scope signal, a NON-BINDING suggested disposition, and a blank
`operator_decision` column the operator fills with user|org|archive|drop. The
suggestion is only a hint — per the design roundtable, scope is not mechanically
derivable, so the operator's decision column is authoritative and the migration
writes nothing until it is filled.

Nothing here mutates state; safe to run repeatedly.
"""
import argparse
import csv
import json
import os
import subprocess
import sys
from collections import Counter


def parse_frontmatter(text):
    """Return (metadata_dict, first_content_line). Tolerant of the simple YAML
    frontmatter aimee memories use (key: value, one nested `metadata:` block)."""
    meta = {}
    first_line = ""
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        # No frontmatter: first non-empty line is the content lead.
        for ln in lines:
            if ln.strip():
                first_line = ln.strip()
                break
        return meta, first_line
    i = 1
    in_metadata = False
    while i < len(lines) and lines[i].strip() != "---":
        raw = lines[i]
        stripped = raw.strip()
        if stripped == "metadata:":
            in_metadata = True
        elif in_metadata and raw[:1] in (" ", "\t") and ":" in stripped:
            k, _, v = stripped.partition(":")
            meta[k.strip()] = v.strip().strip('"')
        elif ":" in stripped and not raw[:1].isspace():
            in_metadata = False
            k, _, v = stripped.partition(":")
            meta.setdefault(k.strip(), v.strip().strip('"'))
        i += 1
    # First content line after the closing '---'
    for ln in lines[i + 1:]:
        if ln.strip():
            first_line = ln.strip()
            break
    return meta, first_line


def scope_signal(meta):
    """Map a memory's frontmatter type to a scope SIGNAL (not a decision).
    user/feedback -> likely user (db1); project/reference -> ambiguous (needs
    operator review); anything else -> unknown."""
    t = (meta.get("type") or "").lower()
    if t in ("user", "feedback"):
        return f"user? ({t})"
    if t in ("project", "reference"):
        return f"review ({t})"
    return f"unknown ({t or 'no-type'})"


def suggested_disposition(meta):
    """NON-BINDING hint from the frontmatter type. The operator's decision
    column overrides this; the migration never acts on the suggestion alone."""
    t = (meta.get("type") or "").lower()
    if t in ("user", "feedback"):
        return "user (db1)"
    if t in ("reference",):
        return "org (db2)?"
    if t in ("project",):
        return "review: user-or-org"
    return "review: unknown"


def worklist_rows(memory_dir):
    """One dict per .md memory for the operator classification CSV."""
    out = []
    for root, _dirs, files in os.walk(memory_dir):
        for f in sorted(files):
            if not f.endswith(".md"):
                continue
            path = os.path.join(root, f)
            rel = os.path.relpath(path, memory_dir)
            name = rel[:-3]
            if name == "MEMORY":
                continue
            try:
                text = open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            meta, first = parse_frontmatter(text)
            out.append({
                "name": name,
                "frontmatter_type": meta.get("type", ""),
                "scope_signal": scope_signal(meta),
                "suggested_disposition": suggested_disposition(meta),
                "operator_decision": "",  # operator fills: user|org|archive|drop
                "first_line": first[:120],
            })
    return sorted(out, key=lambda r: r["name"])


def inventory_local(memory_dir):
    rows = []
    for root, _dirs, files in os.walk(memory_dir):
        for f in sorted(files):
            if not f.endswith(".md"):
                continue
            path = os.path.join(root, f)
            rel = os.path.relpath(path, memory_dir)
            name = rel[:-3]  # strip .md, matches harness_memory `name`
            if name == "MEMORY":
                continue  # the index, not a memory
            try:
                text = open(path, encoding="utf-8", errors="replace").read()
            except OSError as e:
                rows.append({"name": name, "error": str(e)})
                continue
            meta, first = parse_frontmatter(text)
            rows.append({
                "name": name,
                "bytes": len(text.encode("utf-8")),
                "scope_signal": scope_signal(meta),
                "frontmatter_type": meta.get("type", ""),
                "first_line": first[:100],
            })
    return rows


def fetch_server_rows(project):
    """Best-effort read of the canonical server harness_memory table via the
    existing read-only /v1 route (through the aimee CLI). Returns a list of
    {name, type, deleted_at, ...} or None if unavailable (never fatal)."""
    args = ["./aimee", "memory", "harness-list", "--json"]
    if project:
        args += ["--project", project]
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=30)
        if out.returncode != 0:
            return None
        data = json.loads(out.stdout or "{}")
        return data.get("rows") or data.get("memories") or None
    except (OSError, ValueError, subprocess.SubprocessError):
        return None


def main():
    ap = argparse.ArgumentParser(description="Read-only .md harness-memory inventory.")
    ap.add_argument("--memory-dir", default=os.path.expanduser(
        "~/.claude/projects/-home-virant-dev-aimee/memory"))
    ap.add_argument("--project", default="")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--worklist", action="store_true",
                    help="emit the operator classification worklist as CSV on stdout")
    a = ap.parse_args()

    if not os.path.isdir(a.memory_dir):
        print(f"harness-memory-inventory: no memory dir at {a.memory_dir}", file=sys.stderr)
        return 1

    if a.worklist:
        cols = ["name", "frontmatter_type", "scope_signal", "suggested_disposition",
                "operator_decision", "first_line"]
        w = csv.DictWriter(sys.stdout, fieldnames=cols)
        w.writeheader()
        for r in worklist_rows(a.memory_dir):
            w.writerow(r)
        return 0

    local = inventory_local(a.memory_dir)
    server = fetch_server_rows(a.project)

    # Name-based reconciliation (server is canonical; local is a derivative view).
    recon = None
    if server is not None:
        local_names = {r["name"] for r in local if "name" in r}
        server_names = {r.get("name") for r in server if r.get("name")}
        recon = {
            "server_total": len(server_names),
            "local_total": len(local_names),
            "in_both": sorted(local_names & server_names),
            "server_only": sorted(server_names - local_names),
            "local_only": sorted(local_names - server_names),
        }

    if a.json:
        print(json.dumps({"local": local, "reconcile": recon,
                          "server_available": server is not None}, indent=2))
        return 0

    by_scope = Counter(r.get("scope_signal", "?") for r in local if "scope_signal" in r)
    print(f"=== .md harness-memory inventory ({a.memory_dir}) ===")
    print(f"local .md memories: {sum(1 for r in local if 'name' in r and 'error' not in r)}")
    print("scope signals (for operator classification — NOT a decision):")
    for sig, n in by_scope.most_common():
        print(f"  {n:4d}  {sig}")
    if recon is None:
        print("\nserver harness_memory table: UNAVAILABLE "
              "(no `aimee memory harness-list` / server unreachable) — local view only.")
    else:
        print(f"\nreconcile vs server (canonical): in-both={len(recon['in_both'])} "
              f"server-only={len(recon['server_only'])} local-only={len(recon['local_only'])}")
        for label in ("server_only", "local_only"):
            if recon[label]:
                print(f"  DRIFT [{label}]: {', '.join(recon[label][:10])}"
                      + (" …" if len(recon[label]) > 10 else ""))
    print("\nNEXT: operator classifies each memory user(db1)/org(db2)/drop; "
          "migration WRITES nothing until that classification exists. This tool is read-only.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
