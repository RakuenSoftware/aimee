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
COMPOSE_TARGETS = {
    "compose.yaml": ("aimee-kb",),
    "compose.server.yaml": ("aimee-kb", "aimee-server"),
    "compose.server-managed.yaml": ("aimee-server",),
    "compose.server-standalone.yaml": ("aimee-server",),
    "deploy/compose/aimee.yaml": ("aimee-kb", "aimee-server"),
    "deploy/container/aimee-managed.compose.yaml": ("aimee-kb",),
    "deploy/smoothnas/aimee.compose.yaml": ("aimee-kb", "aimee-server"),
    "deploy/smoothnas/aimee-server.compose.yaml": ("aimee-server",),
    "deploy/smoothnas/aimee-kb.compose.yaml": ("aimee-kb",),
}
PLUGIN_TARGETS = ("deploy/smoothnas/aimee-server.plugin.yaml",)
EXACT = {
    "AIMEE_DB2_URL",
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
    for relative, service_names in COMPOSE_TARGETS.items():
        path = ROOT / relative
        model = yaml.safe_load(path.read_text(encoding="utf-8"))
        services = model.get("services", {}) if isinstance(model, dict) else {}
        for service_name in service_names:
            service = services.get(service_name) if isinstance(services, dict) else None
            if not isinstance(service, dict):
                failures.append(f"{relative}: missing {service_name}")
                continue
            for name in sorted(environment_names(service.get("environment"))):
                if credential_name(name):
                    failures.append(
                        f"{relative}:{service_name}: credential {name} is persisted in Config.Env"
                    )

    for relative in PLUGIN_TARGETS:
        path = ROOT / relative
        model = yaml.safe_load(path.read_text(encoding="utf-8"))
        services = model.get("spec", {}).get("services", []) if isinstance(model, dict) else []
        for service in services if isinstance(services, list) else []:
            if not isinstance(service, dict) or service.get("name") != "aimee-server":
                continue
            for name in sorted(environment_names(service.get("environment"))):
                if credential_name(name):
                    failures.append(f"{relative}:aimee-server: credential {name} is persisted")
            for item in service.get("config", []):
                name = item.get("key") if isinstance(item, dict) else None
                if isinstance(name, str) and credential_name(name):
                    failures.append(f"{relative}: config field {name} persists outside Vault")

    if failures:
        print("check-vault-only-container-env: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check-vault-only-container-env: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
