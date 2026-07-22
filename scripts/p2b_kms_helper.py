#!/usr/bin/env python3
"""CT-only signed-HWM/KMS helper for the P2b composition gate."""
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def required(name: str) -> Path:
    value = os.environ.get(name, "")
    if not value:
        raise SystemExit(2)
    return Path(value)


def sign(key_id: str, version: int) -> bytes:
    domain = os.environ.get("AIMEE_VAULT_KMS_HWM_DOMAIN", "")
    private_key = required("AIMEE_P2B_KMS_PRIVATE_KEY")
    message = f"aimee-hwm-v1|{key_id}|{version}|{domain}".encode("ascii")
    # OpenSSL's Ed25519 provider requires a seekable input for its one-shot
    # operation; a subprocess pipe cannot supply the size up front.
    with tempfile.NamedTemporaryFile(prefix="aimee-p2b-hwm-", mode="wb") as source:
        source.write(message)
        source.flush()
        result = subprocess.run(
            ["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(private_key),
             "-in", source.name],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=True,
        ).stdout
    if len(result) != 64:
        raise SystemExit(3)
    if os.environ.get("AIMEE_TEST_HWM_FORGE") == "1":
        result = bytes([result[0] ^ 1]) + result[1:]
    return result


def main() -> int:
    if len(sys.argv) < 3:
        return 2
    operation, key_id = sys.argv[1], sys.argv[2]
    if operation == "decrypt":
        value = required("AIMEE_P2B_KMS_KEK").read_bytes()
        if len(value) != 32:
            return 3
        sys.stdout.buffer.write(value)
        return 0

    state_path = required("AIMEE_P2B_KMS_HWM_STATE")
    lock_path = required("AIMEE_P2B_KMS_HWM_LOCK")
    with lock_path.open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        version = int(state_path.read_text(encoding="ascii").strip())
        if operation == "hwm-cas":
            if len(sys.argv) != 5:
                return 2
            expected, next_version = int(sys.argv[3]), int(sys.argv[4])
            if version != expected or next_version != expected + 1:
                return 4
            temporary = state_path.with_suffix(".tmp")
            temporary.write_text(f"{next_version}\n", encoding="ascii")
            os.replace(temporary, state_path)
            version = next_version
        elif operation != "hwm-read":
            return 2
        signature = sign(key_id, version)
        sys.stdout.buffer.write(f"{version}\n".encode("ascii") + signature)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
