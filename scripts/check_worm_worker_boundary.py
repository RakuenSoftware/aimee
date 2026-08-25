#!/usr/bin/env python3
"""Fail when a producer regains direct WORM-chain authority."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def audit(schema: str, grants: str, roles: str, c_appender: str, fact_mutation: str,
          worker: str, makefile: str, dockerfile: str, entrypoint: str,
          compose: str) -> list[str]:
    failures: list[str] = []

    required_schema = {
        "immutable audit outbox": "CREATE TABLE IF NOT EXISTS kb_audit_outbox",
        "immutable delivery ledger": "CREATE TABLE IF NOT EXISTS kb_audit_delivery",
        "producer submit function": "CREATE OR REPLACE FUNCTION kb_audit_worm_submit",
        "worker drain function": "CREATE OR REPLACE FUNCTION kb_audit_worm_drain",
        "isolated worker API": "CREATE OR REPLACE FUNCTION aimee_kb_worm_api.drain",
        "worker-only appender": "CREATE OR REPLACE FUNCTION kb_audit_worm_append_internal",
        "transactional wakeup": "pg_notify('kb_audit_worm'",
        "concurrent claim": "FOR UPDATE OF o SKIP LOCKED",
        "atomic delivery acknowledgement": "INSERT INTO kb_audit_delivery(outbox_id,audit_seq)",
        "outbox update block": "kb_audit_outbox_no_update",
        "outbox delete block": "kb_audit_outbox_no_delete",
        "delivery update block": "kb_audit_delivery_no_update",
        "delivery delete block": "kb_audit_delivery_no_delete",
    }
    for label, needle in required_schema.items():
        if needle not in schema:
            failures.append(f"schema: missing {label}")

    if re.search(
        r"GRANT\s+[A-Z, ]*INSERT[A-Z, ]*\s+ON(?:\s+TABLE)?\s+"
        r"(?:public\.)?kb_audit_event\s+TO\s+aimee_kb_runtime",
        grants,
        re.I,
    ):
        failures.append("grants: runtime has direct INSERT on kb_audit_event")
    for needle, label in (
        ("REVOKE ALL ON TABLE kb_audit_event,kb_audit_outbox,kb_audit_delivery",
         "runtime storage revoke"),
        ("GRANT EXECUTE ON FUNCTION kb_audit_worm_submit", "runtime submit grant"),
        ("REVOKE ALL ON ALL TABLES IN SCHEMA public FROM aimee_kb_worm_worker",
         "worker table revoke"),
        ("REVOKE ALL ON SCHEMA public FROM aimee_kb_worm_worker",
         "worker public schema revoke"),
        ("GRANT USAGE ON SCHEMA aimee_kb_worm_api", "worker API schema grant"),
        ("GRANT EXECUTE ON FUNCTION aimee_kb_worm_api.drain", "worker API drain grant"),
    ):
        if needle not in grants:
            failures.append(f"grants: missing {label}")

    for needle, label in (
        ("ALTER ROLE aimee_kb_worm_worker LOGIN NOINHERIT NOBYPASSRLS",
         "non-inheriting worker role"),
        ("member.rolname='aimee_kb_worm_worker'", "worker membership-edge repair"),
        ("REVOKE ALL ON SCHEMA public FROM aimee_kb_worm_worker",
         "worker public schema denial"),
    ):
        if needle not in roles:
            failures.append(f"roles: missing {label}")

    production = c_appender.split("#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM", 1)
    if len(production) != 2:
        failures.append("C appender: production branch is not explicit")
    else:
        foreground = production[1].split("#else", 1)[0]
        if "kb_audit_worm_submit" not in foreground:
            failures.append("C appender: production path does not submit to outbox")
        if "INSERT INTO kb_audit_event" in foreground or "pg_advisory_xact_lock" in foreground:
            failures.append("C appender: production path still constructs the chain")

    fm_production = fact_mutation.split("#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM", 1)
    if len(fm_production) != 2 or "kb_fact_commit_worm_seal" not in fm_production[1].split(
        "#else", 1
    )[0]:
        failures.append("fact mutation: production close does not use the narrow DB seal")

    for forbidden in ("fact_mutation.h", "kb_service", "vault_", "provider_"):
        if forbidden in worker:
            failures.append(f"worker binary: forbidden dependency marker {forbidden}")
    if ("AIMEE_WORM_DB2_URL is required" not in worker or
            "refusing runtime credential " not in worker):
        failures.append("worker binary: separate credential is not mandatory")
    if ("aimee_kb_worm_api.drain" not in worker or
            "NOT has_schema_privilege(current_user,'public','USAGE')" not in worker):
        failures.append("worker binary: isolated database capability is not enforced")
    target = re.search(r"^\$\(KB_WORM\):([^\n]+)", makefile, re.M)
    if not target or target.group(1).strip() != "$(OBJDIR)/kb/kb_worm_worker_main.o":
        failures.append("Makefile: WORM binary link surface is not the single worker object")
    if "COPY --from=build /src/aimee-kb-worm /usr/local/bin/aimee-kb-worm" not in dockerfile:
        failures.append("Dockerfile: WORM worker is not packaged")
    if 'exec env AIMEE_WORM_DB2_URL="$embedded_worm_dsn" aimee-kb-worm' not in entrypoint:
        failures.append("entrypoint: self-contained tier does not supervise a separate worker")
    if 'entrypoint: ["/usr/local/bin/aimee-kb-worm"]' not in compose:
        failures.append("compose: hardened worker does not replace the KB entrypoint")

    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    root = args.root
    inputs = {
        "schema": root / "src/modules/db2/c/schema.sql",
        "grants": root / "src/modules/db2/c/schema_grants.sql",
        "roles": root / "src/modules/db2/c/schema_roles.sql",
        "c_appender": root / "src/modules/db2/c/kb_audit_worm.c",
        "fact_mutation": root / "src/modules/db2/c/fact_mutation.c",
        "worker": root / "src/kb/kb_worm_worker_main.c",
        "makefile": root / "src/Makefile",
        "dockerfile": root / "Dockerfile",
        "entrypoint": root / "deploy/container/aimee-kb-entrypoint.sh",
        "compose": root / "deploy/compose/worm-worker.yaml",
    }
    failures = audit(**{name: path.read_text(encoding="utf-8") for name, path in inputs.items()})
    for failure in failures:
        print(f"check_worm_worker_boundary: {failure}", file=sys.stderr)
    if failures:
        return 1
    print("check_worm_worker_boundary: producer/worker privilege split verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
