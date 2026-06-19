#!/usr/bin/env python3
"""Re-key the shared db2/aimee-kb index from checkout-path basenames to canonical
repository identity.

Companion migration to the code change that routes every db2 scope/index key
through workspace_repo_identity() (canonical remote URL, e.g.
https://github.com/owner/repo, or local:<root> for remote-less repos) instead of
the local checkout's directory basename. Existing rows were written under the old
basename keys and stay orphaned until re-keyed here.

Tables re-keyed (all in db2 / Postgres):
  - projects(name, workspace)            name basename -> identity, workspace -> org parent
  - kb_documents(project)                project basename -> identity
  - memory_scopes(scope_value)           scope_type='project'  : basename -> identity
                                         scope_type='workspace': old ws label -> org parent
  - memory_workspaces(workspace)         old ws label -> org parent

Identity is derived from each project's stored `root` via `git -C <root> remote
get-url origin`, normalized identically to src/util_url.c. The migration must run
ON THE KB HOST, where the project roots exist on disk and db2 creds are present.

Safety:
  * DRY-RUN BY DEFAULT — prints the planned UPDATEs and a per-project mapping.
    Nothing is written without --apply.
  * --apply runs everything in a SINGLE TRANSACTION (all-or-nothing).
  * Idempotent: a row already keyed by an identity (https://... or local:...) is
    left untouched, so re-running is safe.
  * Take a db2 snapshot/backup and run a dry-run first. Review the mapping —
    especially any project whose root has no git remote (becomes local:<root>)
    or could not be resolved (skipped, reported).

Usage:
  AIMEE_DB2_URL=postgres://... python3 2026-06-rekey-project-identity.py            # dry run
  AIMEE_DB2_URL=postgres://... python3 2026-06-rekey-project-identity.py --apply    # commit
  python3 2026-06-rekey-project-identity.py --dsn "postgres://..." [--apply]

Requires psycopg2 (or psycopg). If neither is installed, falls back to emitting a
.sql plan file you can review and run with psql.
"""

import argparse
import os
import subprocess
import sys

# Hosts whose path segments route case-insensitively — must match
# util_url_host_is_case_insensitive() in src/util_url.c.
CASE_INSENSITIVE_HOSTS = ("github.com", "gitlab.com", "bitbucket.org")
CASE_INSENSITIVE_SUFFIXES = (".github.com", ".gitlab.com", ".bitbucket.org")


def host_is_case_insensitive(host: str) -> bool:
    if not host:
        return False
    if host in CASE_INSENSITIVE_HOSTS:
        return True
    return any(host.endswith(s) for s in CASE_INSENSITIVE_SUFFIXES)


def normalize_url(url: str):
    """Port of util_url_normalize(): any transport/case -> https://host/owner/repo.
    Returns None for empty/unrecognized/malformed input."""
    if not url:
        return None
    proto = url.find("://")
    colon = url.find(":")
    if proto != -1:
        scheme = url[:proto].lower()
        if scheme not in ("https", "http", "ssh", "git"):
            return None
        authority = url[proto + 3:]
        slash = authority.find("/")
        if slash <= 0:
            return None  # must have a host before the path
        at = authority.find("@")
        host_start = at + 1 if (at != -1 and at < slash) else 0
        host = authority[host_start:slash]
        path = authority[slash + 1:]
    elif colon != -1:
        # scp-like: user@host:path (must have '@' before ':')
        at = url.find("@")
        if at == -1 or at > colon:
            return None
        host = url[at + 1:colon]
        path = url[colon + 1:]
    else:
        return None

    host = host.split(":", 1)[0].lower()  # drop port, lowercase
    if not host:
        return None

    # Collapse '//' runs, optional case-lower, strip trailing '/', strip one '.git'.
    segs = [s for s in path.split("/") if s != ""]
    path = "/".join(segs)
    if host_is_case_insensitive(host):
        path = path.lower()
    if path.endswith(".git"):
        path = path[:-4]
    if not path:
        return None
    return f"https://{host}/{path}"


def workspace_parent(identity: str):
    """Port of util_url_workspace_parent(): https://host/a/b -> https://host/a.
    Returns None when there is no parent (host root, local:, non-https)."""
    if not identity or identity.startswith("local:") or not identity.startswith("https://"):
        return None
    rest = identity[len("https://"):]
    first = rest.find("/")
    last = rest.rfind("/")
    if first == -1 or last <= first:
        return None  # already host/leaf
    return identity[: len("https://") + last]


def repo_identity(root: str):
    """Mirror workspace_repo_identity(): (project_identity, workspace_identity).
    Returns (None, None) if root missing/unreadable."""
    if not root or not os.path.isdir(root):
        return (None, None)
    try:
        out = subprocess.run(
            ["git", "-C", root, "remote", "get-url", "origin"],
            capture_output=True, text=True, timeout=15,
        )
        remote = out.stdout.strip() if out.returncode == 0 else ""
    except Exception:
        remote = ""
    identity = normalize_url(remote) if remote else None
    if not identity:
        identity = f"local:{os.path.realpath(root)}"
    return (identity, workspace_parent(identity) or identity)


def is_already_identity(name: str) -> bool:
    return bool(name) and (name.startswith("https://") or name.startswith("local:"))


def build_plan(rows):
    """rows: list of (id, name, root, workspace). Returns (statements, mapping, skipped)."""
    stmts, mapping, skipped = [], [], []
    for pid, name, root, ws in rows:
        if is_already_identity(name):
            continue  # idempotent: leave identity-keyed rows alone
        identity, org = repo_identity(root)
        if identity is None:
            skipped.append((pid, name, root, "root missing/unreadable on this host"))
            continue
        mapping.append((name, identity, ws, org))
        # Order matters only for readability; all run in one transaction.
        stmts.append(("kb_documents",
                      "UPDATE kb_documents SET project=%s WHERE project=%s", (identity, name)))
        stmts.append(("memory_scopes(project)",
                      "UPDATE memory_scopes SET scope_value=%s "
                      "WHERE scope_type='project' AND scope_value=%s", (identity, name)))
        if ws:
            stmts.append(("memory_scopes(workspace)",
                          "UPDATE memory_scopes SET scope_value=%s "
                          "WHERE scope_type='workspace' AND scope_value=%s", (org, ws)))
            stmts.append(("memory_workspaces",
                          "UPDATE memory_workspaces SET workspace=%s WHERE workspace=%s", (org, ws)))
        stmts.append(("projects",
                      "UPDATE projects SET name=%s, workspace=%s WHERE id=%s", (identity, org, pid)))
    return stmts, mapping, skipped


def render_sql(stmts) -> str:
    def lit(v):
        return "'" + str(v).replace("'", "''") + "'"
    lines = ["BEGIN;"]
    for _, sql, params in stmts:
        rendered = sql
        for p in params:
            rendered = rendered.replace("%s", lit(p), 1)
        lines.append(rendered + ";")
    lines.append("COMMIT;")
    return "\n".join(lines)


def connect(dsn):
    try:
        import psycopg2  # type: ignore
        return ("psycopg2", psycopg2.connect(dsn))
    except ImportError:
        pass
    try:
        import psycopg  # type: ignore
        return ("psycopg", psycopg.connect(dsn))
    except ImportError:
        return (None, None)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dsn", default=os.environ.get("AIMEE_DB2_URL", ""),
                    help="Postgres conninfo (default: $AIMEE_DB2_URL)")
    ap.add_argument("--apply", action="store_true", help="commit changes (default: dry run)")
    ap.add_argument("--sql-out", default="", help="write the plan as a .sql file and exit")
    args = ap.parse_args()

    if not args.dsn:
        sys.exit("error: no DSN — set AIMEE_DB2_URL or pass --dsn")

    driver, conn = connect(args.dsn)
    if conn is None:
        sys.exit("error: no psycopg2/psycopg installed. Re-run with --sql-out plan.sql on a host "
                 "that can read the projects table, then apply with psql.")

    cur = conn.cursor()
    cur.execute("SELECT id, name, root, COALESCE(workspace,'') FROM projects ORDER BY id")
    rows = cur.fetchall()
    stmts, mapping, skipped = build_plan(rows)

    print(f"# {len(rows)} projects; {len(mapping)} to re-key; "
          f"{len(rows) - len(mapping) - len(skipped)} already identity-keyed; "
          f"{len(skipped)} skipped (driver={driver})\n")
    for old, new, ws, org in mapping:
        print(f"  project  {old!r:40} -> {new}")
        if ws:
            print(f"  workspace{ws!r:40} -> {org}")
    for pid, name, root, why in skipped:
        print(f"  SKIP id={pid} name={name!r} root={root!r}: {why}")

    if args.sql_out:
        with open(args.sql_out, "w") as f:
            f.write(render_sql(stmts) + "\n")
        print(f"\nwrote {len(stmts)} statements to {args.sql_out}")
        return

    if not args.apply:
        print(f"\nDRY RUN — {len(stmts)} UPDATE statements not executed. Re-run with --apply.")
        return

    try:
        for _, sql, params in stmts:
            cur.execute(sql, params)
        conn.commit()
        print(f"\nAPPLIED {len(stmts)} statements in one transaction.")
    except Exception as e:
        conn.rollback()
        sys.exit(f"\nROLLED BACK on error: {e}")


if __name__ == "__main__":
    main()
