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
    bootstrap = (ROOT / "deploy/container/aimee-managed-authority-bootstrap.sh").read_text(
        encoding="utf-8"
    )
    helper = (ROOT / "scripts/managed_kms_helper.py").read_text(encoding="utf-8")
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
    identity_service = managed.split("  aimee-server-identity:", 1)[-1].split(
        "  aimee-authority-bootstrap:", 1
    )[0]
    authority_service = managed.split("  aimee-authority-bootstrap:", 1)[-1].split(
        "\n  aimee-llm:", 1
    )[0]
    if "networks: [aimee]" not in identity_service:
        failures.append("server identity must share the managed KB network")
    if 'AIMEE_OFFLINE_ALLOW_NO_SWAP_MLOCK_FALLBACK: "1"' not in managed:
        failures.append("explicit no-swap memory-hardening fallback")
    if (
        "mem_limit: 1g" not in authority_service
        or "memswap_limit: 1g" not in authority_service
    ):
        failures.append("authority bootstrap must enforce a zero-swap child cgroup")
    # An unlimited memlock ulimit is not portable to nested/unprivileged
    # containers. The OCI runtime rejects it before the bootstrap process can
    # exercise the explicit no-swap fallback above.
    if re.search(r"(?:^|\n)\s+memlock:\s*(?:\n|$)", authority_service):
        failures.append("authority bootstrap must inherit the runtime memlock limit")
    if "privileged: true" in managed or "cap_add:" in managed:
        failures.append("authority bootstrap must not request ineffective extra privilege")
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
    if "authority_db_role=aimee_managed_authority_login" not in bootstrap:
        failures.append("dedicated authority database login")
    if 'user=$authority_db_role' not in bootstrap:
        failures.append("authority tools must not use the database superuser")
    if "GRANT aimee_kb_migrate TO $authority_db_role" not in bootstrap:
        failures.append("authority migration membership")
    if '"$helper" hwm-read "$AIMEE_VAULT_KMS_KEY_ID" 0 0' not in bootstrap:
        failures.append("software KMS self-check must use the custody provider argv contract")
    if "SET ROLE aimee_kb_jwks_publish" not in bootstrap:
        failures.append("publisher metadata queries must enter the isolated publisher role")
    for table in (
        "kb_management_jwks_publication_candidate",
        "kb_management_jwks_publication_generation",
    ):
        if re.search(rf"FROM\s+(?:public\.)?{table}\b", bootstrap, re.I):
            failures.append(f"authority script must not directly read {table}")
    for function in (
        "public.kb_management_jwks_publication_inspect()",
        "public.kb_management_jwks_publication_final()",
    ):
        if function not in bootstrap:
            failures.append(f"authority script missing narrow metadata reader {function}")
    if 'sys.argv[3:] != ["0", "0"]' not in helper:
        failures.append("managed KMS HWM read argument contract")
    for dsn in ("AIMEE_KB_TOKEN_ROOTS_PROVISION_DSN", "AIMEE_KB_JWKS_PUBLISH_DSN"):
        if f'export {dsn}="$authority_db_url"' not in bootstrap:
            failures.append(f"least-privilege {dsn}")

    for offline_main in (
        ROOT / "src/kb/kb_mgmt_token_roots_provision_main.c",
        ROOT / "src/kb/kb_mgmt_jwks_publish_main.c",
    ):
        text = offline_main.read_text(encoding="utf-8")
        if "kb_mgmt_offline_harden_process()" not in text:
            failures.append(f"managed no-swap hardening missing from {offline_main.name}")

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
