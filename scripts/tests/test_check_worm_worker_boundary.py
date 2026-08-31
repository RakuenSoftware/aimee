#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_worm_worker_boundary", ROOT / "scripts/check_worm_worker_boundary.py"
)
assert SPEC and SPEC.loader
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


def sources() -> dict[str, str]:
    paths = {
        "schema": "src/modules/db2/c/schema.sql",
        "grants": "src/modules/db2/c/schema_grants.sql",
        "roles": "src/modules/db2/c/schema_roles.sql",
        "c_appender": "src/modules/db2/c/kb_audit_worm.c",
        "fact_mutation": "src/modules/db2/c/fact_mutation.c",
        "worker": "src/kb/kb_worm_worker_main.c",
        "server": "src/server/server_main.c",
        "worm_store": "src/modules/audit/audit_worm.c",
        "makefile": "src/Makefile",
        "dockerfile": "Dockerfile",
        "entrypoint": "deploy/container/aimee-kb-entrypoint.sh",
        "compose": "deploy/compose/worm-worker.yaml",
    }
    return {name: (ROOT / path).read_text(encoding="utf-8") for name, path in paths.items()}


class WormWorkerBoundaryTest(unittest.TestCase):
    def test_repository_contract_passes(self) -> None:
        self.assertEqual([], CHECK.audit(**sources()))

    def test_postgres_chain_table_is_rejected(self) -> None:
        data = sources()
        data["schema"] += "\nCREATE TABLE IF NOT EXISTS kb_audit_event(seq BIGINT);\n"
        self.assertIn(
            "schema: PostgreSQL still owns a WORM chain table", CHECK.audit(**data)
        )

    def test_retired_chain_guard_dependency_is_rejected(self) -> None:
        data = sources()
        data["schema"] = data["schema"].replace(
            "EXECUTE FUNCTION kb_management_jwks_publication_root_guard()",
            "EXECUTE FUNCTION kb_worm_block()",
            1,
        )
        self.assertIn(
            "schema: an immutable table still depends on the retired PG chain guard",
            CHECK.audit(**data),
        )

    def test_foreground_chain_builder_is_rejected(self) -> None:
        data = sources()
        data["c_appender"] = data["c_appender"].replace(
            "#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM",
            '#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM\n'
            '   const char *bad = "INSERT INTO kb_audit_event";\n'
            "   (void)bad;",
            1,
        )
        self.assertIn(
            "C appender: production path still constructs the chain", CHECK.audit(**data)
        )

    def test_worker_dependency_growth_is_rejected(self) -> None:
        data = sources()
        data["worker"] += '\n#include "fact_mutation.h"\n'
        self.assertIn(
            "worker binary: forbidden dependency marker fact_mutation.h", CHECK.audit(**data)
        )

    def test_shared_startup_admission_is_required(self) -> None:
        data = sources()
        data["server"] = data["server"].replace(
            "audit_worm_startup_verify", "audit_worm_verify", 1
        )
        self.assertIn(
            "server binary: shared WORM startup admission is not enforced",
            CHECK.audit(**data),
        )

    def test_worker_link_growth_is_rejected(self) -> None:
        data = sources()
        data["makefile"] = data["makefile"].replace(
            "$(KB_WORM): $(KB_WORM_OBJS)",
            "$(KB_WORM): $(KB_WORM_OBJS) $(KB_DB2_OBJS)",
            1,
        )
        self.assertIn(
            "Makefile: WORM binary does not use its narrow object closure",
            CHECK.audit(**data),
        )

    def test_worker_public_schema_access_is_rejected(self) -> None:
        data = sources()
        data["grants"] = data["grants"].replace(
            "REVOKE ALL ON SCHEMA public FROM aimee_kb_worm_worker;",
            "GRANT USAGE ON SCHEMA public TO aimee_kb_worm_worker;",
            1,
        )
        self.assertIn(
            "grants: missing worker public schema revoke", CHECK.audit(**data)
        )

    def test_worker_role_membership_repair_is_required(self) -> None:
        data = sources()
        data["roles"] = data["roles"].replace(
            "OR member.rolname='aimee_kb_worm_worker'",
            "OR member.rolname='deleted_worker_role'",
            1,
        )
        self.assertIn(
            "roles: missing worker membership-edge repair", CHECK.audit(**data)
        )

    def test_embedded_worker_public_inheritance_is_rejected(self) -> None:
        data = sources()
        data["entrypoint"] = data["entrypoint"].replace(
            "REVOKE USAGE ON SCHEMA public FROM PUBLIC;",
            "REVOKE USAGE ON SCHEMA public FROM aimee_kb_worm_worker;",
            1,
        )
        self.assertIn(
            "entrypoint: embedded worker inherits public schema access",
            CHECK.audit(**data),
        )


if __name__ == "__main__":
    unittest.main()
