#!/usr/bin/env python3
"""Keep offline authority tools isolated and absent from the local model wizard."""

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

    # The local wizard no longer installs a KB or its management authority.
    # Retained offline tools still obey their custody boundary below.
    services = re.findall(r"^  ([a-z][a-z0-9-]*):$", managed.split("\nvolumes:", 1)[0], re.M)
    if set(services) != {"aimee-embedder", "aimee-llm"}:
        failures.append("wizard may provision only model services")
    if any(name in deploy for name in ("deploy_authority_bootstrap_argv", "aimee-authority-bootstrap")):
        failures.append("wizard must not install a KB authority")
    for text in (managed, outer, (ROOT / "compose.yaml").read_text()):
        if "aimee-managed-authority-home" in text or "aimee-authority-bootstrap:" in text:
            failures.append("local application must not mount or initialize authority custody")
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
