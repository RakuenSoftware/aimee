#!/usr/bin/env python3
"""Static/plant gate for the offline P5-C2b JWKS publisher boundary."""

from pathlib import Path
import argparse
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = ROOT / "src" / "Makefile"
MAIN = ROOT / "src" / "kb" / "kb_mgmt_jwks_publish_main.c"
PRIVATE = {
    "kb/kb_mgmt_jwks_publish_main.o",
    "kb/kb_mgmt_jwks_publication.o",
    "modules/db2/c/management_jwks_publication.o",
}
SHARED_OFFLINE = {
    "kb/kb_mgmt_offline_hardening.o",
    "kb/kb_mgmt_token_roots_provision.o",
}
REQUIRED = PRIVATE | SHARED_OFFLINE


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
    closure = make_var("JWKS_PUBLISHER_OBJS", extra)
    for obj in sorted(REQUIRED - closure):
        failures.append(f"publisher closure omits {obj}")
    for variable in (
        "KB_OBJS",
        "KB_DB2_OBJS",
        "SERVER_OBJS",
        "STATUS_AUTHORITY_OBJS",
        "STATUS_PROVISIONER_OBJS",
    ):
        for obj in sorted(REQUIRED & make_var(variable, extra)):
            failures.append(f"{variable} includes private object {obj}")
    for obj in sorted(PRIVATE & make_var("TOKEN_ROOTS_PROVISIONER_OBJS", extra)):
        failures.append(f"TOKEN_ROOTS_PROVISIONER_OBJS includes private object {obj}")

    text = MAKEFILE.read_text(encoding="utf-8")
    for target in ("all", "server", "kb", "install"):
        match = re.search(rf"(?ms)^{target}:(.*?)(?=^[A-Za-z0-9_.$(][^\n]*:|\Z)", text)
        if match and ("KB_JWKS_PUBLISHER" in match.group(1) or
                      "aimee-kb-jwks-publish" in match.group(1)):
            failures.append(f"{target} exposes JWKS publisher")
    if "jwks-publisher-core:" not in text:
        failures.append("explicit JWKS publisher build target is missing")

    if MAIN.exists():
        main = MAIN.read_text(encoding="utf-8")
        main_at = main.find("int main(")
        main_body = main[main_at:] if main_at >= 0 else ""
        clear_at = main_body.find("clearenv()")
        first_helper_at = min(
            (at for at in (main_body.find("vault_unseal("),
                           main_body.find("vault_maintenance_guard_unseal("))
             if at >= 0),
            default=-1,
        )
        if clear_at < 0 or first_helper_at < 0 or clear_at > first_helper_at:
            failures.append("publisher does not sanitize its environment before helper forks")
        allowed = {
            "AIMEE_VAULT_KMS_HELPER",
            "AIMEE_VAULT_KMS_KEY_ID",
            "AIMEE_VAULT_KMS_HWM_PUBKEY",
            "AIMEE_VAULT_KMS_HWM_DOMAIN",
        }
        restored = set(re.findall(r'setenv\("([A-Z0-9_]+)"',
                                  main_body[clear_at:first_helper_at]))
        if restored != allowed:
            failures.append("post-clearenv helper environment is not the exact KMS allowlist")
    elif not extra:
        failures.append("publisher main is missing")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plant-test", action="store_true")
    args = parser.parse_args()
    if args.plant_test:
        failures = check("KB_SRCS += kb/kb_mgmt_jwks_publication.c")
        expected = "KB_OBJS includes private object kb/kb_mgmt_jwks_publication.o"
        if expected not in failures:
            print("JWKS publisher isolation plant test: failed", file=sys.stderr)
            return 1
        print("JWKS publisher isolation plant test: ok")
        return 0
    failures = check()
    if failures:
        print("JWKS publisher isolation: failed: " + "; ".join(failures), file=sys.stderr)
        return 1
    print("JWKS publisher isolation: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
