#!/usr/bin/env python3
"""Static/plant gate for the P5-C2d token-authority process boundary."""

from pathlib import Path
import argparse
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = ROOT / "src" / "Makefile"
MAIN = ROOT / "src" / "kb" / "kb_mgmt_token_authority_main.c"
DAEMON = ROOT / "src" / "kb" / "kb_mgmt_token_authority_daemon.c"
IPC = ROOT / "src" / "kb" / "kb_mgmt_token_authority_ipc.c"
PRIVATE = {
    "kb/kb_mgmt_token_authority_main.o",
    "kb/kb_mgmt_token_authority_daemon.o",
    "kb/kb_mgmt_token_authority_service.o",
    "kb/kb_mgmt_token_authority.o",
    "modules/db2/c/management_token_authority.o",
}
CLIENT = "kb/kb_mgmt_token_authority_ipc.o"


def make_var(name: str, extra: str = "") -> set[str]:
    program = f"include {MAKEFILE}\n{extra}\nprint:\n\t@printf '%s\\n' '$({name})'\n"
    proc = subprocess.run(
        ["make", "--no-print-directory", "-f", "-", "print"],
        cwd=MAKEFILE.parent,
        input=program,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode:
        raise RuntimeError(proc.stderr.strip() or f"cannot expand {name}")
    return {
        word.removeprefix("build/obj/")
        for word in proc.stdout.split()
        if word.endswith(".o")
    }


def check(extra: str = "") -> list[str]:
    failures: list[str] = []
    authority = make_var("TOKEN_AUTHORITY_OBJS", extra)
    kb = make_var("KB_OBJS", extra)
    for obj in sorted(PRIVATE - authority):
        failures.append(f"authority closure omits private object {obj}")
    if CLIENT not in authority:
        failures.append("authority closure omits bounded IPC object")
    if CLIENT not in kb:
        failures.append("ordinary KB omits bounded IPC client")

    for variable in (
        "KB_OBJS",
        "SERVER_OBJS",
        "STATUS_AUTHORITY_OBJS",
        "STATUS_PROVISIONER_OBJS",
        "TOKEN_ROOTS_PROVISIONER_OBJS",
        "JWKS_PUBLISHER_OBJS",
    ):
        leaked = PRIVATE & make_var(variable, extra)
        for obj in sorted(leaked):
            failures.append(f"{variable} includes token-authority private object {obj}")

    forbidden_fragments = (
        "/http/",
        "console",
        "provider",
        "kb_mgmt_jwks_publish_main.o",
        "kb_mgmt_token_roots_provision_main.o",
        "kb_mgmt_token_roots_provision.o",
        "management_action_journal.o",
        "kb_http_",
    )
    for obj in sorted(authority):
        if any(fragment in obj for fragment in forbidden_fragments):
            failures.append(f"authority closure contains forbidden object {obj}")

    text = MAKEFILE.read_text(encoding="utf-8")
    for target in ("all", "server", "kb", "install"):
        match = re.search(rf"(?ms)^{target}:(.*?)(?=^[A-Za-z0-9_.$(][^\n]*:|\Z)", text)
        if match and (
            "KB_TOKEN_AUTHORITY" in match.group(1)
            or "aimee-kb-token-authority" in match.group(1)
        ):
            failures.append(f"{target} exposes token authority")
    if "token-authority-core:" not in text:
        failures.append("explicit token-authority build target is missing")

    main = MAIN.read_text(encoding="utf-8")
    clear_at = main.find("clearenv()")
    unseal_at = main.find("vault_unseal(")
    if clear_at < 0 or unseal_at < 0 or clear_at > unseal_at:
        failures.append("authority does not sanitize its environment before custody use")
    allowed = {
        "AIMEE_VAULT_KMS_HELPER",
        "AIMEE_VAULT_KMS_KEY_ID",
        "AIMEE_VAULT_KMS_HWM_PUBKEY",
        "AIMEE_VAULT_KMS_HWM_DOMAIN",
    }
    restored = set(re.findall(r'setenv\("([A-Z0-9_]+)"', main[clear_at:unseal_at]))
    if restored != allowed:
        failures.append("post-clearenv authority environment is not the exact KMS allowlist")

    boundary = DAEMON.read_text(encoding="utf-8") + IPC.read_text(encoding="utf-8")
    for required in (
        "SO_PEERCRED",
        "PR_SET_DUMPABLE",
        "RLIMIT_CORE",
        "PR_SET_NO_NEW_PRIVS",
        "authority_uid == config->kb_uid",
        "st.st_uid != 0",
    ):
        if required not in boundary:
            failures.append(f"authority boundary omits {required}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plant-test", action="store_true")
    args = parser.parse_args()
    if args.plant_test:
        failures = check("KB_SRCS += kb/kb_mgmt_token_authority_service.c")
        expected = (
            "KB_OBJS includes token-authority private object "
            "kb/kb_mgmt_token_authority_service.o"
        )
        if expected not in failures:
            print("token authority isolation plant test: failed", file=sys.stderr)
            return 1
        print("token authority isolation plant test: ok")
        return 0
    failures = check()
    if failures:
        print("token authority isolation: failed: " + "; ".join(failures), file=sys.stderr)
        return 1
    print("token authority isolation: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
