#!/usr/bin/env python3
"""Static gate for wizard-managed authority isolation and volume direction."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    dockerfile = (ROOT / "Dockerfile.authority-bootstrap").read_text(encoding="utf-8")
    managed = (ROOT / "deploy/container/aimee-managed.compose.yaml").read_text(encoding="utf-8")
    outer = (ROOT / "compose.server-managed.yaml").read_text(encoding="utf-8")
    deploy = (ROOT / "src/server/deploy_apply.c").read_text(encoding="utf-8")
    failures: list[str] = []

    required_docker = {
        "isolated roots build": r"make -C src token-roots-provisioner-core jwks-publisher-core",
        "roots binary": r"COPY --from=build /src/aimee-kb-token-roots-provision",
        "publisher binary": r"COPY --from=build /src/aimee-kb-jwks-publish",
        "root one-shot": r'USER 0:0\s+ENTRYPOINT \["/usr/local/bin/aimee-managed-authority-bootstrap"\]',
        "ordinary binary exclusion": r"! find /usr/local/bin /usr/libexec/aimee",
    }
    for name, pattern in required_docker.items():
        if not re.search(pattern, dockerfile, re.S):
            failures.append(name)

    if 'profiles: ["authority-bootstrap"]' not in managed:
        failures.append("authority one-shot profile")
    if "cap_add: [IPC_LOCK]" not in managed or "memlock:" not in managed:
        failures.append("protected-memory capability")
    if "network_mode: none" not in managed:
        failures.append("offline bootstrap network isolation")
    if "aimee-managed-jwks-trust:/run/aimee-trust" not in managed:
        failures.append("authority writable trust volume")
    if "aimee-managed-jwks-trust:/run/aimee/managed-trust:ro" not in outer:
        failures.append("server read-only trust volume")
    if "aimee-managed-authority-home" in outer:
        failures.append("server must not mount authority custody volume")
    if "aimee-authority-bootstrap" not in deploy or "deploy_authority_bootstrap_argv" not in deploy:
        failures.append("wizard deploy orchestration")

    for ordinary in (ROOT / "Dockerfile", ROOT / "Dockerfile.server"):
        text = ordinary.read_text(encoding="utf-8")
        if "aimee-kb-token-roots-provision" in text or "aimee-kb-jwks-publish" in text:
            failures.append(f"private authority binary leaked into {ordinary.name}")

    if failures:
        print("managed authority packaging: failed: " + ", ".join(failures), file=sys.stderr)
        return 1
    print("managed authority packaging: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
