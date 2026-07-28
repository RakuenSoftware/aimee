#!/usr/bin/python3
"""Software-backed KMS/HWM helper for the single-host managed installer.

The helper is copied into the root-owned authority volume.  It deliberately
derives every private path from its own fixed location because the offline
provisioner and publisher clear their environments before invoking it.  The
managed deployment therefore keeps the same narrow helper contract as an
operator-supplied KMS without passing private paths through argv or env.
"""

from __future__ import annotations

import fcntl
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


KEY_ID = re.compile(r"\A[A-Za-z0-9._-]{1,600}\Z")


def authority_home() -> Path:
    return Path(__file__).resolve().parent


def private_file(name: str, expected_size: int | None = None) -> Path:
    path = authority_home() / name
    st = path.stat(follow_symlinks=False)
    if not path.is_file() or path.is_symlink() or st.st_uid != 0 or st.st_nlink != 1:
        raise SystemExit(3)
    if st.st_mode & 0o077:
        raise SystemExit(3)
    if expected_size is not None and st.st_size != expected_size:
        raise SystemExit(3)
    return path


def sign(key_id: str, version: int) -> bytes:
    domain = os.environ.get("AIMEE_VAULT_KMS_HWM_DOMAIN", "")
    if not domain or any(ord(c) < 0x20 or ord(c) == 0x7F for c in domain):
        raise SystemExit(2)
    message = f"aimee-hwm-v1|{key_id}|{version}|{domain}".encode("ascii")
    key = private_file("hwm-private.pem")
    with tempfile.NamedTemporaryFile(
        prefix="aimee-managed-hwm-", dir=authority_home(), mode="wb"
    ) as source:
        source.write(message)
        source.flush()
        result = subprocess.run(
            [
                "/usr/bin/openssl",
                "pkeyutl",
                "-sign",
                "-rawin",
                "-inkey",
                str(key),
                "-in",
                source.name,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=True,
            env={"PATH": "/usr/bin:/bin"},
        ).stdout
    if len(result) != 64:
        raise SystemExit(3)
    return result


def state_paths(key_id: str) -> tuple[Path, Path]:
    return (
        authority_home() / f"hwm.{key_id}.state",
        authority_home() / f"hwm.{key_id}.lock",
    )


def open_private(path: Path) -> int:
    return os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)


def state_read_or_initialize(path: Path) -> int:
    try:
        fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    except FileNotFoundError:
        fd = os.open(
            path,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o600,
        )
        try:
            os.write(fd, b"1\n")
            os.fsync(fd)
        finally:
            os.close(fd)
        fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        st = os.fstat(fd)
        if st.st_uid != 0 or st.st_nlink != 1 or st.st_mode & 0o077 or st.st_size > 32:
            raise SystemExit(3)
        raw = os.read(fd, 33)
    finally:
        os.close(fd)
    if not re.fullmatch(rb"[1-9][0-9]*\n", raw):
        raise SystemExit(3)
    return int(raw)


def state_replace(path: Path, value: int) -> None:
    temporary = authority_home() / f".{path.name}.{os.getpid()}.tmp"
    fd = os.open(
        temporary,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
        0o600,
    )
    try:
        os.write(fd, f"{value}\n".encode("ascii"))
        os.fsync(fd)
    finally:
        os.close(fd)
    os.replace(temporary, path)


def main() -> int:
    if len(sys.argv) < 3:
        return 2
    operation, key_id = sys.argv[1], sys.argv[2]
    if not KEY_ID.fullmatch(key_id):
        return 2
    if operation == "decrypt":
        value = private_file("kek.raw", 32).read_bytes()
        if len(value) != 32:
            return 3
        sys.stdout.buffer.write(value)
        return 0

    state_path, lock_path = state_paths(key_id)
    lock_fd = open_private(lock_path)
    try:
        lock_stat = os.fstat(lock_fd)
        if lock_stat.st_uid != 0 or lock_stat.st_nlink != 1 or lock_stat.st_mode & 0o077:
            return 3
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        version = state_read_or_initialize(state_path)
        if operation == "hwm-cas":
            if len(sys.argv) != 5:
                return 2
            expected, next_version = int(sys.argv[3]), int(sys.argv[4])
            if version != expected or next_version != expected + 1:
                return 4
            state_replace(state_path, next_version)
            version = next_version
        # vault_custody_kms invokes every HWM operation with the expected/next
        # slots populated; the direct bootstrap self-check historically used
        # the shorter read-only form.  Accept those two exact shapes and reject
        # every other argv surface.
        elif operation != "hwm-read" or len(sys.argv) not in (3, 5):
            return 2
        signature = sign(key_id, version)
        sys.stdout.buffer.write(f"{version}\n".encode("ascii") + signature)
        return 0
    finally:
        os.close(lock_fd)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError):
        raise SystemExit(3)
