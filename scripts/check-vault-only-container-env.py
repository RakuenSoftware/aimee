#!/usr/bin/env python3
"""Reject credential storage in long-lived aimee-server/aimee-kb metadata."""

from __future__ import annotations

import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    raise SystemExit("check-vault-only-container-env: PyYAML is required")


ROOT = Path(__file__).resolve().parents[1]
# Inspect every declared service in every shipped composition, including the
# fragments behind include/extends. This avoids silently skipping inherited
# credentials and needs no operator secrets or Docker daemon for validation.
COMPOSE_TARGETS = ["compose.yaml", "compose.kb.yaml", "compose.server.yaml",
    "compose.server-managed.yaml", "compose.server-standalone.yaml",
    "deploy/compose/aimee.yaml", "deploy/container/managed.override.yaml",
    "deploy/container/aimee-managed.compose.yaml", "deploy/smoothnas/aimee.compose.yaml",
    "deploy/smoothnas/aimee-server.compose.yaml", "deploy/smoothnas/aimee-kb.compose.yaml",
    "deploy/smoothnas/storage.override.yaml"]

class ComposeLoader(yaml.SafeLoader):
    pass
ComposeLoader.add_constructor('!override', lambda loader, node: loader.construct_mapping(node) if isinstance(node, yaml.MappingNode) else loader.construct_sequence(node))

EXACT = {
    "AIMEE_DB2_URL",
    "AIMEE_STORE_URL",
    "AIMEE_STORE_MIGRATION_URL",
    "AIMEE_KB_CONN",
    "AIMEE_VAULT_PKCS11_PIN",
    "AIMEE_WEBCHAT_USER",
    "AIMEE_WEBCHAT_USERS",
    "DATABASE_URL",
    "AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE",
}
SUFFIXES = (
    "_TOKEN",
    "_SECRET",
    "_PASSWORD",
    "_PRIVATE_KEY",
    "_API_KEY",
    "_DSN",
    "_BEARER",
    "_PASS",
    "_CREDENTIAL",
    "_CREDENTIALS",
)


def credential_name(name: str) -> bool:
    return (
        name in EXACT
        or name.startswith("AIMEE_DELEGATE_KEY_")
        or name.endswith(SUFFIXES)
        or "_SECRET_" in name
    )


def environment_names(value: object) -> set[str]:
    if isinstance(value, dict):
        return {str(key) for key in value}
    if isinstance(value, list):
        return {str(item).split("=", 1)[0] for item in value}
    return set()


def main() -> int:
    failures: list[str] = []
    for relative in COMPOSE_TARGETS:
        path = ROOT / relative
        model = yaml.load(path.read_text(encoding="utf-8"), Loader=ComposeLoader)
        services = model.get("services", {}) if isinstance(model, dict) else {}
        for service_name in services:
            if service_name not in ('aimee-server', 'aimee-kb'):
                continue
            service = services.get(service_name) if isinstance(services, dict) else None
            if not isinstance(service, dict):
                failures.append(f"{relative}: missing {service_name}")
                continue
            for name in sorted(environment_names(service.get("environment"))):
                if credential_name(name):
                    failures.append(
                        f"{relative}:{service_name}: credential {name} is persisted in Config.Env"
                    )

    if failures:
        print("check-vault-only-container-env: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check-vault-only-container-env: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
