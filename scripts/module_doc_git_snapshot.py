#!/usr/bin/env python3
"""Provider-neutral, bounded acquisition of immutable Git snapshots.

Production acquisition is Linux-only and accepts HTTPS remotes.  Provider
adapters supply already-authorized refs and expected commit IDs; this module
does not interpret CI identity, pull requests, or provider APIs.
"""

from __future__ import annotations

from contextlib import suppress
from dataclasses import dataclass
import errno
import fcntl
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import stat
import subprocess
import tempfile
import time
from typing import Callable, Sequence
from urllib.parse import urlsplit

import module_doc_contract as contract


MAX_HEADER_BYTES = 8_192
MAX_FETCH_SPECS = 8
MAX_REF_BYTES = 255
MAX_ORIGIN_BYTES = 512
MAX_ORIGINS_BYTES = 8_192
MAX_OUTPUT_BYTES = 65_536
MAX_TREE_ENTRIES = 100_000
# A stuck-filesystem backstop, NOT a performance budget -- see _tree_bytes. Sized so
# only a filesystem that has stopped answering can reach it; MAX_TREE_ENTRIES is what
# actually bounds a large tree, and it does so deterministically.
MAX_TREE_SCAN_SECONDS = 30.0
# Read the clock once per this many entries. Per-file, the syscall was a measurable
# share of the scan it was supposed to be policing.
TREE_SCAN_CLOCK_INTERVAL = 1024
MIN_DISK_BYTES = 1_048_576
MAX_DISK_BYTES = 8_589_934_592
DEFAULT_DISK_BYTES = 536_870_912
MIN_TIMEOUT_SECONDS = 1
MAX_TIMEOUT_SECONDS = 300
GIT = Path("/usr/bin/git")
LOCAL_REF_RE = re.compile(r"^refs/aimee-snapshot/[a-z][a-z0-9-]{0,63}$")
HEX_40_RE = re.compile(r"^[0-9a-f]{40}$")
TOKEN68_RE = re.compile(
    rb"^Authorization: [A-Za-z][A-Za-z0-9._~-]{0,31} "
    rb"[A-Za-z0-9._~+/-]+={0,2}$"
)


@dataclass(frozen=True)
class FetchSpec:
    source_ref: str
    local_ref: str
    expected_commit: str


class RepositoryHandle:
    """Own a read-only snapshot until explicit or context-managed close."""

    def __init__(self, root: Path, repository: Path) -> None:
        self._root = root
        self.repository = repository
        self._closed = False

    def __enter__(self) -> Path:
        if self._closed:
            contract.fail("snapshot-lifetime", "snapshot handle is closed")
        return self.repository

    def close(self) -> None:
        if self._closed:
            return
        _remove_root(self._root)
        self._closed = True

    def __exit__(self, exc_type, exc, traceback) -> None:
        if exc is None:
            self.close()
            return
        try:
            self.close()
        except BaseException as cleanup_error:
            exc.add_note(f"snapshot cleanup also failed: {cleanup_error}")


def _wipe(value: bytearray) -> None:
    value[:] = b"\0" * len(value)


def _sanitize(raw: bytes) -> str:
    printable = bytes(byte for byte in raw if byte in (9, 10, 13) or 32 <= byte <= 126)
    return printable.decode("ascii")[:MAX_OUTPUT_BYTES]


def _tree_bytes(root: Path) -> int:
    """Total size of every regular file under `root`, with the scan itself bounded.

    MAX_TREE_ENTRIES is the real bound and it is DETERMINISTIC: the same tree gives
    the same verdict on every machine. The clock is only a backstop for a filesystem
    that has stopped answering, where the entry count would never advance.

    Which is why the clock must not be tight. At 0.1s, checked once per file, a
    contended CI runner failed this on a fixture tree of a few dozen files -- and it
    surfaces as `rule=snapshot-disk`, a CONTRACT violation, so a scheduling hiccup on
    a shared runner reads as "this snapshot broke the rules". Observed twice on
    2026-08-09, on two unrelated branches, while the same tree passed locally and on
    the integration tip. A budget that a healthy tree can miss is not measuring the
    tree.

    So the clock is now generous enough that only a genuinely stuck filesystem trips
    it, and it is read once per block of entries rather than once per file: at 0.1s
    the per-file time.monotonic() was itself part of what made the scan slow.
    """
    total = 0
    entries = 0
    deadline = time.monotonic() + MAX_TREE_SCAN_SECONDS
    for directory, _, files in os.walk(root):
        for name in files:
            entries += 1
            if entries > MAX_TREE_ENTRIES:
                contract.fail("snapshot-disk", "snapshot tree scan exceeds its budget")
            if entries % TREE_SCAN_CLOCK_INTERVAL == 0 and time.monotonic() >= deadline:
                contract.fail("snapshot-disk", "snapshot tree scan exceeds its budget")
            with suppress(OSError):
                total += (Path(directory) / name).stat().st_size
    return total


def _terminate(process: subprocess.Popen[bytes]) -> None:
    with suppress(ProcessLookupError):
        os.killpg(process.pid, signal.SIGTERM)
    if process.poll() is None:
        try:
            process.wait(timeout=0.2)
        except subprocess.TimeoutExpired:
            pass
    with suppress(ProcessLookupError):
        os.killpg(process.pid, 0)
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
    if process.poll() is None:
        process.wait()


def _run(
    arguments: Sequence[str],
    *,
    cwd: Path,
    env: dict[str, str],
    timeout_seconds: int,
    pass_fds: tuple[int, ...] = (),
    monitor_root: Path | None = None,
    max_disk_bytes: int = DEFAULT_DISK_BYTES,
    sensitive_output: bool = False,
) -> bytes:
    process = subprocess.Popen(
        list(arguments),
        cwd=cwd,
        env=env,
        shell=False,
        bufsize=0,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
        pass_fds=pass_fds,
    )
    assert process.stdout is not None and process.stderr is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ, "stdout")
    selector.register(process.stderr, selectors.EVENT_READ, "stderr")
    output = bytearray()
    errors = bytearray()
    scratch = bytearray(MAX_OUTPUT_BYTES)
    counts = {"stdout": 0, "stderr": 0}
    deadline = time.monotonic() + timeout_seconds
    next_disk_check = 0.0
    try:
        while selector.get_map():
            now = time.monotonic()
            if now >= deadline:
                _terminate(process)
                contract.fail("snapshot-timeout", "Git operation timed out")
            if monitor_root is not None and now >= next_disk_check:
                if _tree_bytes(monitor_root) > max_disk_bytes:
                    _terminate(process)
                    contract.fail("snapshot-disk", "snapshot exceeds its disk ceiling")
                next_disk_check = now + 0.05
            events = selector.select(min(0.05, deadline - now))
            for key, _ in events:
                count = key.fileobj.readinto(scratch)
                if not count:
                    selector.unregister(key.fileobj)
                    continue
                counts[key.data] += count
                if counts[key.data] > MAX_OUTPUT_BYTES:
                    _terminate(process)
                    contract.fail(
                        "snapshot-output", f"Git {key.data} exceeds its output ceiling"
                    )
                if not sensitive_output:
                    sink = output if key.data == "stdout" else errors
                    sink.extend(memoryview(scratch)[:count])
                scratch[:count] = b"\0" * count
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            _terminate(process)
            contract.fail("snapshot-timeout", "Git operation timed out")
        try:
            returncode = process.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            _terminate(process)
            contract.fail("snapshot-timeout", "Git operation timed out")
        if monitor_root is not None and _tree_bytes(monitor_root) > max_disk_bytes:
            contract.fail("snapshot-disk", "snapshot exceeds its disk ceiling")
        if returncode != 0:
            detail = (
                "authenticated Git operation failed"
                if sensitive_output else _sanitize(bytes(errors)) or "Git operation failed"
            )
            contract.fail("snapshot-git", detail)
        return b"" if sensitive_output else bytes(output)
    finally:
        _wipe(output)
        _wipe(errors)
        _wipe(scratch)
        selector.close()
        if process.poll() is None:
            _terminate(process)
        process.stdout.close()
        process.stderr.close()


def _base_env(home: Path, *, config: str = "/dev/null") -> dict[str, str]:
    return {
        "GIT_CONFIG_GLOBAL": config,
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_TERMINAL_PROMPT": "0",
        "HOME": str(home),
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
    }


def _validate_common(
    specs: Sequence[FetchSpec], authorization: bytearray, timeout_seconds: int,
    max_disk_bytes: int,
) -> tuple[FetchSpec, ...]:
    if not isinstance(authorization, bytearray):
        contract.fail("snapshot-credential", "authorization must be an owned bytearray")
    if not 1 <= len(authorization) <= MAX_HEADER_BYTES:
        contract.fail("snapshot-credential", "authorization size is outside the allowed range")
    if not TOKEN68_RE.fullmatch(authorization):
        contract.fail(
            "snapshot-credential",
            "authorization must be one canonical token68 header",
        )
    if (
        type(timeout_seconds) is not int
        or not MIN_TIMEOUT_SECONDS <= timeout_seconds <= MAX_TIMEOUT_SECONDS
    ):
        contract.fail("snapshot-timeout", "timeout is outside the allowed range")
    if type(max_disk_bytes) is not int or not MIN_DISK_BYTES <= max_disk_bytes <= MAX_DISK_BYTES:
        contract.fail("snapshot-disk", "disk ceiling is outside the allowed range")
    if not isinstance(specs, Sequence) or not 1 <= len(specs) <= MAX_FETCH_SPECS:
        contract.fail("snapshot-specs", "snapshot requires one to eight fetch specs")
    result = tuple(specs)
    if not all(isinstance(item, FetchSpec) for item in result):
        contract.fail("snapshot-specs", "every fetch spec must use the canonical type")
    if len({item.source_ref for item in result}) != len(result) or len(
        {item.local_ref for item in result}
    ) != len(result):
        contract.fail("snapshot-specs", "source and local refs must be unique")
    for item in result:
        if not all(
            isinstance(value, str)
            for value in (item.source_ref, item.local_ref, item.expected_commit)
        ):
            contract.fail("snapshot-specs", "fetch spec fields must be strings")
        for ref in (item.source_ref, item.local_ref):
            if (
                not ref.startswith("refs/") or not ref.isascii()
                or not 1 <= len(ref) <= MAX_REF_BYTES
            ):
                contract.fail("snapshot-ref", "refs must be bounded full ASCII refs")
        if not LOCAL_REF_RE.fullmatch(item.local_ref):
            contract.fail("snapshot-ref", "local ref is outside refs/aimee-snapshot")
        if not HEX_40_RE.fullmatch(item.expected_commit):
            contract.fail("snapshot-commit", "expected commit must be a full lowercase SHA-1")
    return result


def _validate_https_url(value: str) -> tuple[str, str]:
    if (
        not isinstance(value, str) or not value.isascii()
        or any(ord(character) < 33 or ord(character) > 126 for character in value)
    ):
        contract.fail("snapshot-url", "repository URL must be visible ASCII")
    parsed = urlsplit(value)
    try:
        port = parsed.port
    except ValueError:
        contract.fail("snapshot-url", "repository URL port is invalid")
    if (
        parsed.scheme != "https" or not parsed.hostname or parsed.username is not None
        or parsed.password is not None or parsed.query or parsed.fragment
        or "@" in parsed.netloc
    ):
        contract.fail("snapshot-url", "production repository URL must be canonical HTTPS")
    if (
        parsed.hostname != parsed.hostname.lower() or parsed.hostname.endswith(".")
        or "%" in value or "\\" in value or '"' in value or "//" in parsed.path
        or any(component in (".", "..") for component in parsed.path.split("/"))
        or port in (0, 443)
    ):
        contract.fail("snapshot-url", "repository URL is ambiguous")
    host = parsed.hostname
    rendered_host = f"[{host}]" if ":" in host else host
    origin = f"https://{rendered_host}" + (f":{port}" if port is not None else "")
    if parsed.netloc != origin.removeprefix("https://"):
        contract.fail("snapshot-url", "repository URL is not canonical")
    return value, origin


def _validate_allowed_origins(origin: str, allowed_origins: Sequence[str]) -> None:
    if not isinstance(allowed_origins, Sequence) or isinstance(
        allowed_origins, (str, bytes, bytearray)
    ):
        contract.fail("snapshot-origin", "allowed origins must be a sequence")
    allowed = tuple(allowed_origins)
    if not 1 <= len(allowed) <= 32 or not all(isinstance(item, str) for item in allowed):
        contract.fail("snapshot-origin", "one to 32 canonical allowed origins are required")
    if any(len(item.encode("utf-8")) > MAX_ORIGIN_BYTES for item in allowed) or sum(
        len(item.encode("utf-8")) for item in allowed
    ) > MAX_ORIGINS_BYTES:
        contract.fail("snapshot-origin", "allowed origins exceed their byte ceiling")
    if len(set(allowed)) != len(allowed) or any(
        _validate_https_url(f"{item}/repository")[1] != item for item in allowed
    ):
        contract.fail("snapshot-origin", "allowed origins must be unique and canonical")
    if origin not in allowed:
        contract.fail("snapshot-origin", "repository origin is not allowed")


def _validate_fixture_remote(remote: Path, fixture_root: Path) -> str:
    root_input = fixture_root.absolute()
    if root_input.is_symlink() or not root_input.is_dir():
        contract.fail("snapshot-test-remote", "fixture root must be a real directory")
    root = root_input.resolve(strict=True)
    try:
        relative = remote.absolute().relative_to(root_input)
    except ValueError:
        contract.fail("snapshot-test-remote", "test remote escapes fixture root")
    current = root_input
    for component in relative.parts:
        current = current / component
        if current.is_symlink():
            contract.fail("snapshot-test-remote", "test remote contains a symlink")
    candidate = current.resolve(strict=True)
    try:
        candidate.relative_to(root)
    except ValueError:
        contract.fail("snapshot-test-remote", "test remote escapes fixture root")
    return candidate.as_uri()


def _validate_refs(specs: Sequence[FetchSpec], *, home: Path, timeout: int) -> None:
    for item in specs:
        for ref in (item.source_ref, item.local_ref):
            _run(
                [str(GIT), "check-ref-format", ref],
                cwd=home,
                env=_base_env(home),
                timeout_seconds=timeout,
            )


def _credential_config(scope_url: str, authorization: bytearray) -> tuple[int, bytearray]:
    if not callable(getattr(os, "memfd_create", None)) or not Path("/proc/self/fd").is_dir():
        contract.fail("snapshot-platform", "anonymous Git configuration requires Linux memfd")
    fd = os.memfd_create(
        "aimee-git-credential", flags=os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING
    )
    config = bytearray()
    try:
        os.fchmod(fd, stat.S_IRUSR | stat.S_IWUSR)
        config.extend(b'[http "')
        config.extend(scope_url.encode("ascii"))
        config.extend(b'"]\n\textraHeader = "')
        config.extend(authorization)
        config.extend(
            b"\"\n\tfollowRedirects = false\n[credential]\n\thelper =\n"
        )
        view = memoryview(config)
        try:
            written = 0
            while written < len(view):
                try:
                    count = os.write(fd, view[written:])
                except InterruptedError:
                    continue
                if count <= 0:
                    contract.fail(
                        "snapshot-credential", "credential config write was incomplete"
                    )
                written += count
        finally:
            view.release()
        os.fsync(fd)
        seals = fcntl.F_SEAL_SEAL | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW | fcntl.F_SEAL_WRITE
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, seals)
        actual_seals = fcntl.fcntl(fd, fcntl.F_GET_SEALS)
        if seals & ~actual_seals:
            contract.fail("snapshot-credential", "credential config sealing failed")
        os.lseek(fd, 0, os.SEEK_SET)
        return fd, config
    except BaseException:
        os.close(fd)
        _wipe(config)
        raise


def _make_read_only(root: Path) -> None:
    for directory, directories, files in os.walk(root):
        path = Path(directory)
        path.chmod(stat.S_IRUSR | stat.S_IXUSR)
        for name in directories:
            (path / name).chmod(stat.S_IRUSR | stat.S_IXUSR)
        for name in files:
            (path / name).chmod(stat.S_IRUSR)


def _make_owner_writable(root: Path) -> None:
    if not root.exists():
        return
    for directory, directories, files in os.walk(root):
        path = Path(directory)
        with suppress(OSError):
            path.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        for name in directories:
            with suppress(OSError):
                (path / name).chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        for name in files:
            with suppress(OSError):
                (path / name).chmod(stat.S_IRUSR | stat.S_IWUSR)


def _remove_root(root: Path) -> None:
    last_error: OSError | None = None
    for _ in range(2):
        _make_owner_writable(root)
        try:
            shutil.rmtree(root)
            return
        except OSError as error:
            last_error = error
    contract.fail("snapshot-cleanup", f"snapshot cleanup failed: {last_error}")


def _acquire(
    remote_url: str,
    specs: Sequence[FetchSpec],
    authorization: bytearray,
    *,
    timeout_seconds: int,
    max_disk_bytes: int,
    temp_parent: Path | None,
    observer: Callable[[Sequence[str], dict[str, str], tuple[int, ...]], None] | None = None,
) -> RepositoryHandle:
    validated = _validate_common(specs, authorization, timeout_seconds, max_disk_bytes)
    parent = temp_parent.resolve() if temp_parent is not None else None
    root = Path(tempfile.mkdtemp(prefix="aimee-snapshot-", dir=parent))
    try:
        root.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        repository = root / "repository.git"
        home = root / "home"
        home.mkdir(mode=0o700)
        _validate_refs(validated, home=home, timeout=timeout_seconds)
        _run(
            [str(GIT), "init", "--bare", str(repository)],
            cwd=root,
            env=_base_env(home),
            timeout_seconds=timeout_seconds,
            monitor_root=root,
            max_disk_bytes=max_disk_bytes,
        )
        fd = -1
        config = bytearray()
        try:
            fd, config = _credential_config(remote_url, authorization)
            fd_path = f"/proc/self/fd/{fd}"
            env = _base_env(home, config=fd_path)
            arguments = [
                str(GIT), "-c", "protocol.version=2", "-c", "fetch.fsckObjects=true",
                "-c", "transfer.fsckObjects=true", "fetch", "--atomic", "--no-tags",
                "--no-force", "--no-write-fetch-head", remote_url,
                *(f"{item.source_ref}:{item.local_ref}" for item in validated),
            ]
            if observer is not None:
                observer(arguments, env, (fd,))
            _run(
                arguments,
                cwd=repository,
                env=env,
                timeout_seconds=timeout_seconds,
                pass_fds=(fd,),
                monitor_root=root,
                max_disk_bytes=max_disk_bytes,
                sensitive_output=True,
            )
        finally:
            if fd >= 0:
                os.close(fd)
                try:
                    os.fstat(fd)
                except OSError as error:
                    if error.errno != errno.EBADF:
                        raise
                else:
                    contract.fail("snapshot-credential", "credential descriptor remained open")
            _wipe(config)
            _wipe(authorization)

        env = _base_env(home)
        for item in validated:
            resolved = _run(
                [str(GIT), "rev-parse", "--verify", f"{item.local_ref}^{{commit}}"],
                cwd=repository, env=env, timeout_seconds=timeout_seconds,
            ).decode("ascii").strip()
            kind = _run(
                [str(GIT), "cat-file", "-t", resolved],
                cwd=repository, env=env, timeout_seconds=timeout_seconds,
            ).decode("ascii").strip()
            if kind != "commit" or resolved != item.expected_commit:
                contract.fail("snapshot-commit", "fetched ref does not match expected commit")
        unreachable = _run(
            [
                str(GIT), "fsck", "--full", "--strict", "--no-reflogs",
                "--unreachable",
            ],
            cwd=repository,
            env=env,
            timeout_seconds=timeout_seconds,
            monitor_root=root,
            max_disk_bytes=max_disk_bytes,
        )
        if unreachable:
            contract.fail("snapshot-object-closure", "snapshot contains unreachable objects")
        if _tree_bytes(root) > max_disk_bytes:
            contract.fail("snapshot-disk", "snapshot exceeds its disk ceiling")
        _make_read_only(root)
        return RepositoryHandle(root, repository)
    except BaseException as active_error:
        try:
            _remove_root(root)
        except BaseException as cleanup_error:
            active_error.add_note(f"snapshot cleanup also failed: {cleanup_error}")
        raise


def acquire_snapshot(
    repository_url: str,
    specs: Sequence[FetchSpec],
    authorization: bytearray,
    *,
    allowed_origins: Sequence[str],
    timeout_seconds: int = 60,
    max_disk_bytes: int = DEFAULT_DISK_BYTES,
    temp_parent: Path | None = None,
) -> RepositoryHandle:
    """Consume authorization and acquire one bounded HTTPS Git snapshot."""
    try:
        canonical_url, origin = _validate_https_url(repository_url)
        _validate_allowed_origins(origin, allowed_origins)
        return _acquire(
            canonical_url, specs, authorization,
            timeout_seconds=timeout_seconds, max_disk_bytes=max_disk_bytes,
            temp_parent=temp_parent,
        )
    finally:
        if isinstance(authorization, bytearray):
            _wipe(authorization)


def _acquire_test_snapshot(
    remote: Path,
    fixture_root: Path,
    specs: Sequence[FetchSpec],
    authorization: bytearray,
    *,
    timeout_seconds: int = 60,
    max_disk_bytes: int = DEFAULT_DISK_BYTES,
    temp_parent: Path | None = None,
    observer: Callable[[Sequence[str], dict[str, str], tuple[int, ...]], None] | None = None,
) -> RepositoryHandle:
    try:
        return _acquire(
            _validate_fixture_remote(remote, fixture_root), specs, authorization,
            timeout_seconds=timeout_seconds, max_disk_bytes=max_disk_bytes,
            temp_parent=temp_parent, observer=observer,
        )
    finally:
        if isinstance(authorization, bytearray):
            _wipe(authorization)
