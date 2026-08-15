#!/usr/bin/env python3
"""Build and atomically replace module-document SSHSIG attestations.

The helper reads public keys only. Matching private keys must be available
through SSH_AUTH_SOCK, normally from a user-owned agent or hardware token.
"""

from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import errno
import fcntl
import json
import os
from pathlib import Path, PurePosixPath
import secrets
import shutil
import stat
import subprocess
import sys

import module_doc_contract as contract


REPO_ROOT = Path(__file__).resolve().parent.parent
INVENTORY_PATH = Path("tests/baselines/modules/canonical-inventory.yaml")
ATTESTATION_PATH = Path("docs/modules/attestations")
AT_FDCWD = -100
RENAME_EXCHANGE = 2
RENAME_NOREPLACE = 1


class SigningError(ValueError):
    """A signing-preparation failure that leaves repository content unchanged."""


def fail(rule: str, message: str) -> None:
    raise SigningError(f"rule={rule}: {message}")


def _regular_file(path: Path, *, max_bytes: int) -> bytes:
    try:
        info = path.lstat()
    except OSError as exc:
        fail("input", f"cannot inspect {path}: {exc}")
    if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o111:
        fail("input-mode", f"{path} must be a non-executable regular file")
    if info.st_size > max_bytes:
        fail("input-size", f"{path} exceeds {max_bytes} bytes")
    try:
        return path.read_bytes()
    except OSError as exc:
        fail("input", f"cannot read {path}: {exc}")


def _repository_file(repo: Path, relative: Path | PurePosixPath, *, max_bytes: int) -> bytes:
    pure = PurePosixPath(relative.as_posix())
    if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
        fail("repository-path", "repository path is not canonical")
    current = repo
    for index, part in enumerate(pure.parts):
        current = current / part
        try:
            info = current.lstat()
        except OSError as exc:
            fail("repository-path", f"cannot inspect {current}: {exc}")
        if stat.S_ISLNK(info.st_mode):
            fail("repository-path", f"symlink component is forbidden: {current}")
        if index + 1 < len(pure.parts) and not stat.S_ISDIR(info.st_mode):
            fail("repository-path", f"non-directory path component: {current}")
    return _regular_file(current, max_bytes=max_bytes)


def _repository_directory(repo: Path, relative: Path | PurePosixPath) -> Path:
    pure = PurePosixPath(relative.as_posix())
    if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
        fail("repository-path", "repository directory is not canonical")
    current = repo
    for part in pure.parts:
        current = current / part
        try:
            info = current.lstat()
        except OSError as exc:
            fail("repository-path", f"cannot inspect {current}: {exc}")
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
            fail("repository-path", f"directory or symlink invariant failed: {current}")
    return current


def _public_key(path: Path) -> str:
    raw = _regular_file(path, max_bytes=4096)
    try:
        text = raw.decode("ascii", "strict").strip("\n")
    except UnicodeDecodeError as exc:
        fail("public-key", f"public key is not ASCII: {exc}")
    if not contract._valid_ed25519_public_key(text):
        fail("public-key", "public key must be canonical Ed25519 without a comment")
    return text


def _inventory(repo: Path) -> tuple[set[str], set[str]]:
    value = contract.strict_json_bytes(
        _repository_file(repo, INVENTORY_PATH, max_bytes=contract.MAX_DESCRIPTOR_BYTES)
    )
    if not isinstance(value, dict) or set(value) != {
        "schema_version",
        "required",
        "optional",
        "principal_refs",
        "retired_principal_refs",
    }:
        fail("inventory", "canonical inventory has unexpected shape")
    if value["schema_version"] != 2 or type(value["schema_version"]) is not int:
        fail("inventory", "canonical inventory schema_version must equal 1")
    groups: list[set[str]] = []
    for name in ("required", "optional"):
        entries = value[name]
        if not isinstance(entries, list) or not entries or any(
            not isinstance(item, str) or not contract.MODULE_ID_RE.fullmatch(item)
            for item in entries
        ):
            fail("inventory", f"{name} must contain canonical module IDs")
        if len(entries) != len(set(entries)):
            fail("inventory", f"{name} must be unique")
        groups.append(set(entries))
    if groups[0] & groups[1]:
        fail("inventory", "required and optional inventories overlap")
    return groups[0], groups[1]


def _resolve_worktree_reference(repo: Path, module_ids: set[str], reference: str) -> None:
    match = contract.REFERENCE_RE.fullmatch(reference)
    if not match:
        fail("evidence", f"invalid reference {reference!r}")
    module_id = match.group("module")
    if module_id is not None:
        if module_id not in module_ids:
            fail("evidence", f"unknown module {module_id!r}")
        return
    relative = match.group("path")
    assert relative is not None
    pure = PurePosixPath(relative)
    if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
        fail("evidence", "path is not canonical")
    raw = _repository_file(
        repo, pure, max_bytes=contract.MAX_GOVERNED_BLOB_BYTES
    )
    if match.group("line") is not None and int(match.group("line")) > len(raw.splitlines()):
        fail("evidence", f"line exceeds {relative}")


def _existing_attestations_are_well_formed(
    path: Path,
    module_ids: set[str],
    *,
    pinned_fd: int | None = None,
    owner_identity: str,
    owner_key: str,
    reviewer_identity: str,
    reviewer_key: str,
    toolchain: contract.LockedToolchain,
) -> None:
    if pinned_fd is None and not path.exists() and not path.is_symlink():
        return
    try:
        info = os.fstat(pinned_fd) if pinned_fd is not None else path.lstat()
    except OSError as exc:
        fail("existing-attestations", str(exc))
    if not stat.S_ISDIR(info.st_mode):
        fail("existing-attestations", "existing attestation path must be a directory")
    expected = {"index.json"}
    for module_id in module_ids:
        expected.update({
            f"{module_id}.subject.json", f"{module_id}.owner.sig",
            f"{module_id}.reviewer.sig",
        })
    actual = {entry.name for entry in path.iterdir()}
    if actual != expected:
        fail("existing-attestations", "existing directory is missing, extra, or partial")
    subjects: dict[str, tuple[dict[str, object], bytes]] = {}
    for entry in path.iterdir():
        limit = (
            contract.MAX_INDEX_BYTES if entry.name == "index.json"
            else contract.MAX_SUBJECT_BYTES if entry.name.endswith(".subject.json")
            else contract.MAX_SIGNATURE_BYTES
        )
        raw = _regular_file(entry, max_bytes=limit)
        if entry.name == "index.json":
            contract.strict_json_bytes(raw)
        elif entry.name.endswith(".subject.json"):
            value = contract.strict_json_bytes(raw)
            if raw != contract.canonical_subject(value):
                fail("existing-attestations", f"{entry.name} is not canonical")
            assert isinstance(value, dict)
            module_id = entry.name.removesuffix(".subject.json")
            if value["module_id"] != module_id:
                fail("existing-attestations", f"{entry.name} has a mismatched module ID")
            if value["owner_identity"] != owner_identity or value["reviewer_identity"] != reviewer_identity:
                fail("existing-attestations", f"{entry.name} does not match the selected signers")
            subjects[module_id] = (value, raw)
        else:
            contract._validate_sshsig_envelope(raw)
    expected_index: list[dict[str, str]] = []
    for module_id in sorted(module_ids):
        _, subject_raw = subjects[module_id]
        owner_signature = _regular_file(
            path / f"{module_id}.owner.sig", max_bytes=contract.MAX_SIGNATURE_BYTES
        )
        reviewer_signature = _regular_file(
            path / f"{module_id}.reviewer.sig", max_bytes=contract.MAX_SIGNATURE_BYTES
        )
        contract.verify_sshsig(
            subject_raw, owner_signature, owner_identity, owner_key, toolchain
        )
        contract.verify_sshsig(
            subject_raw, reviewer_signature, reviewer_identity, reviewer_key, toolchain
        )
        expected_index.append({
            "module_id": module_id,
            "subject_sha256": contract.sha256(subject_raw),
        })
    contract.validate_attestation_index(
        _regular_file(path / "index.json", max_bytes=contract.MAX_INDEX_BYTES),
        expected_index,
    )


def _sign(
    subject: bytes,
    public_key: str,
    identity: str,
    toolchain: contract.LockedToolchain,
    root: Path,
    root_fd: int,
) -> bytes:
    public_path = root / f"signer-{contract.sha256(identity.encode())}.pub"
    public_path.write_text(public_key + "\n", encoding="ascii")
    os.chmod(public_path, 0o600)
    isolated_home = root / ".signing-home"
    isolated_home.mkdir(mode=0o700, exist_ok=True)
    auth_sock = os.environ.get("SSH_AUTH_SOCK")
    if not auth_sock:
        fail("ssh-agent", "SSH_AUTH_SOCK is required")
    try:
        socket_info = os.stat(auth_sock)
    except OSError as exc:
        fail("ssh-agent", f"cannot inspect SSH_AUTH_SOCK: {exc}")
    if not stat.S_ISSOCK(socket_info.st_mode):
        fail("ssh-agent", "SSH_AUTH_SOCK must name a Unix socket")
    result = subprocess.run(
        [str(toolchain.executable()), "-Y", "sign", "-f", str(public_path),
         "-n", "aimee.module-doc.v1", "-O", "hashalg=sha512"],
        input=subject,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        pass_fds=(root_fd,),
        env={
            "HOME": os.fspath(isolated_home),
            "LANG": "C",
            "LC_ALL": "C",
            "PATH": "/usr/bin:/bin",
            "SSH_AUTH_SOCK": auth_sock,
        },
    )
    if result.returncode != 0:
        fail("sign", result.stderr.decode("utf-8", "replace").strip() or "ssh-keygen rejected signing")
    # verify_sshsig parses the envelope namespace/hash/key type, then uses the
    # exact public key as an allowed-signer binding through the locked binary.
    contract.verify_sshsig(subject, result.stdout, identity, public_key, toolchain)
    return result.stdout


def _renameat2(
    old_dir_fd: int,
    old_name: str,
    new_dir_fd: int,
    new_name: str,
    flags: int,
) -> None:
    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None:
        fail("atomic-replace", "renameat2(RENAME_EXCHANGE) is unavailable")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    if renameat2(
        old_dir_fd, os.fsencode(old_name), new_dir_fd, os.fsencode(new_name), flags
    ) != 0:
        error = ctypes.get_errno()
        if error in {errno.ENOSYS, errno.EINVAL, errno.EXDEV}:
            fail("atomic-replace", "filesystem does not support the required atomic rename")
        fail("atomic-replace", os.strerror(error))


def _rename_exchange(old: Path, new: Path) -> None:
    _renameat2(AT_FDCWD, os.fspath(old), AT_FDCWD, os.fspath(new), RENAME_EXCHANGE)


def _same_inode(left: os.stat_result, right: os.stat_result) -> bool:
    return left.st_dev == right.st_dev and left.st_ino == right.st_ino


def _assert_parent_path(parent: Path, expected: os.stat_result) -> None:
    try:
        current = parent.lstat()
    except OSError as exc:
        fail("parent-race", f"cannot revalidate attestation parent: {exc}")
    if not stat.S_ISDIR(current.st_mode) or not _same_inode(current, expected):
        fail("parent-race", "attestation parent changed during signing")


def _open_pinned_directory(parent_fd: int, name: str, *, missing_ok: bool) -> int | None:
    try:
        return os.open(
            name,
            os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
            dir_fd=parent_fd,
        )
    except FileNotFoundError:
        if missing_ok:
            return None
        raise
    except OSError as exc:
        fail("directory-pin", f"cannot pin {name!r}: {exc}")


def _assert_entry_inode(
    parent_fd: int, name: str, expected: os.stat_result, *, rule: str
) -> None:
    try:
        current = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
    except FileNotFoundError:
        fail(rule, f"pinned directory {name!r} disappeared")
    if not stat.S_ISDIR(current.st_mode) or not _same_inode(current, expected):
        fail(rule, f"pinned directory {name!r} changed")


def _staging_directory(parent_fd: int) -> tuple[str, int, Path]:
    if not Path(f"/proc/self/fd/{parent_fd}").is_dir():
        fail("atomic-replace", "pinned /proc directory access is unavailable")
    for _ in range(32):
        name = f".attestations-{secrets.token_hex(12)}"
        try:
            os.mkdir(name, mode=0o700, dir_fd=parent_fd)
        except FileExistsError:
            continue
        staging_fd = _open_pinned_directory(parent_fd, name, missing_ok=False)
        assert staging_fd is not None
        return name, staging_fd, Path(f"/proc/self/fd/{staging_fd}")
    fail("staging", "cannot allocate a unique staging directory")


def build_attestations(args: argparse.Namespace) -> None:
    repo = args.repo.resolve()
    required, optional = _inventory(repo)
    module_ids = required | optional
    owner_key = _public_key(args.owner_public_key.resolve())
    reviewer_key = _public_key(args.reviewer_public_key.resolve())
    if args.owner_identity == args.reviewer_identity:
        fail("identity-separation", "owner and reviewer identities must differ")
    if owner_key == reviewer_key:
        fail("key-separation", "owner and reviewer public keys must differ")
    signed_at = contract.parse_timestamp(args.signed_at, "signed-at")
    age = (datetime.now(timezone.utc) - signed_at).total_seconds()
    if age < 0 or age > 86400:
        fail("signed-at", "signed_at must be no later than now and at most 24 hours old")
    lock = contract.strict_json_bytes(
        _regular_file(args.toolchain_lock.resolve(), max_bytes=contract.MAX_DESCRIPTOR_BYTES)
    )
    toolchain = contract.load_locked_toolchain(lock, args.ssh_keygen.resolve())
    target_parent = _repository_directory(repo, ATTESTATION_PATH.parent)
    parent_fd = os.open(
        target_parent,
        os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
    )
    staging: Path | None = None
    staging_name: str | None = None
    staging_fd: int | None = None
    prior_fd: int | None = None
    try:
        try:
            fcntl.flock(parent_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail("signing-busy", "another attestation replacement holds the parent lock")
        parent_info = os.fstat(parent_fd)
        _assert_parent_path(target_parent, parent_info)
        prior_fd = _open_pinned_directory(parent_fd, "attestations", missing_ok=True)
        if prior_fd is None:
            prior_info = None
        else:
            prior_info = os.fstat(prior_fd)
            _existing_attestations_are_well_formed(
                Path(f"/proc/self/fd/{prior_fd}"),
                module_ids,
                pinned_fd=prior_fd,
                owner_identity=args.owner_identity,
                owner_key=owner_key,
                reviewer_identity=args.reviewer_identity,
                reviewer_key=reviewer_key,
                toolchain=toolchain,
            )
            _assert_entry_inode(
                parent_fd, "attestations", prior_info, rule="target-race"
            )
        staging_name, staging_fd, staging = _staging_directory(parent_fd)
        index: list[dict[str, str]] = []
        for module_id in sorted(module_ids):
            descriptor_relative = Path("src/modules") / module_id / "module.yaml"
            descriptor_raw = _repository_file(
                repo, descriptor_relative, max_bytes=contract.MAX_DESCRIPTOR_BYTES
            )
            descriptor = contract.validate_v2_metadata(
                contract.strict_json_bytes(descriptor_raw),
                optional=module_id in optional,
                known_ids=module_ids,
            )
            document_path = Path(descriptor["docs"])
            document_raw = _repository_file(
                repo, document_path, max_bytes=contract.MAX_DOCUMENT_BYTES
            )
            contract.parse_module_document(
                document_raw,
                module_id,
                contract.DocumentProjection(
                    tuple(descriptor["sources"]), tuple(descriptor["public_headers"])
                ),
                resolve_reference=lambda reference: _resolve_worktree_reference(
                    repo, module_ids, reference
                ),
            )
            subject = {
                "descriptor_sha256": contract.sha256(descriptor_raw),
                "document_sha256": contract.sha256(document_raw),
                "module_id": module_id,
                "owner_identity": args.owner_identity,
                "reviewer_identity": args.reviewer_identity,
                "schema": "aimee.module-doc-attestation.v1",
                "signed_at": args.signed_at,
            }
            subject_raw = contract.canonical_subject(subject)
            subject_path = staging / f"{module_id}.subject.json"
            subject_path.write_bytes(subject_raw)
            os.chmod(subject_path, 0o644)
            for role, identity, public_key in (
                ("owner", args.owner_identity, owner_key),
                ("reviewer", args.reviewer_identity, reviewer_key),
            ):
                signature = _sign(
                    subject_raw, public_key, identity, toolchain, staging, staging_fd
                )
                signature_path = staging / f"{module_id}.{role}.sig"
                signature_path.write_bytes(signature)
                os.chmod(signature_path, 0o644)
            index.append({"module_id": module_id, "subject_sha256": contract.sha256(subject_raw)})
        for public_path in staging.glob("signer-*.pub"):
            public_path.unlink()
        signing_home = staging / ".signing-home"
        if signing_home.exists():
            shutil.rmtree(signing_home)
        index_path = staging / "index.json"
        index_path.write_bytes(contract.canonical_attestation_index(index))
        os.chmod(index_path, 0o644)
        # Re-read and cryptographically verify the exact bytes that will be
        # installed, including canonical subjects and the recomputed index.
        _existing_attestations_are_well_formed(
            staging,
            module_ids,
            pinned_fd=staging_fd,
            owner_identity=args.owner_identity,
            owner_key=owner_key,
            reviewer_identity=args.reviewer_identity,
            reviewer_key=reviewer_key,
            toolchain=toolchain,
        )
        _assert_parent_path(target_parent, parent_info)
        staged_info = os.fstat(staging_fd)
        _assert_entry_inode(parent_fd, staging_name, staged_info, rule="staging-race")
        if prior_info is not None:
            _assert_entry_inode(
                parent_fd, "attestations", prior_info, rule="target-race"
            )
            _assert_entry_inode(parent_fd, staging_name, staged_info, rule="staging-race")
            _renameat2(
                parent_fd, staging_name, parent_fd, "attestations", RENAME_EXCHANGE
            )
            _assert_parent_path(target_parent, parent_info)
            _assert_entry_inode(
                parent_fd, staging_name, prior_info, rule="target-race"
            )
            _assert_entry_inode(
                parent_fd, "attestations", staged_info, rule="target-race"
            )
        else:
            try:
                os.stat("attestations", dir_fd=parent_fd, follow_symlinks=False)
            except FileNotFoundError:
                pass
            else:
                fail("target-race", "attestation directory appeared during signing")
            _renameat2(
                parent_fd, staging_name, parent_fd, "attestations", RENAME_NOREPLACE
            )
            _assert_parent_path(target_parent, parent_info)
            _assert_entry_inode(
                parent_fd, "attestations", staged_info, rule="target-race"
            )
    finally:
        if prior_fd is not None:
            os.close(prior_fd)
        if staging_fd is not None:
            os.close(staging_fd)
        os.close(parent_fd)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO_ROOT)
    parser.add_argument("--owner-identity", required=True)
    parser.add_argument("--owner-public-key", type=Path, required=True)
    parser.add_argument("--reviewer-identity", required=True)
    parser.add_argument("--reviewer-public-key", type=Path, required=True)
    parser.add_argument("--signed-at", required=True, help="UTC timestamp: YYYY-MM-DDTHH:MM:SSZ")
    parser.add_argument("--toolchain-lock", type=Path, required=True)
    parser.add_argument("--ssh-keygen", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        build_attestations(parse_args(sys.argv[1:] if argv is None else argv))
    except (SigningError, contract.ContractError, OSError, ValueError) as exc:
        print(f"sign_module_docs: error: {exc}", file=sys.stderr)
        return 1
    print("sign_module_docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
