#!/usr/bin/env python3
"""Preserve bounded redacted test logs, never configuration or credentials."""
import os
from pathlib import Path
import re
import stat
import sys
import uuid

LIMIT = 512 * 1024
CREDENTIAL = re.compile(r"authorization|bearer|token|api[_-]?key|password|secret|PRIVATE KEY", re.I)
DSN = re.compile(r"(?:postgres(?:ql)?://|[a-z][a-z0-9+.-]{0,15}://[^\s/@:]+:[^\s/@]+@)[^\s\"']*", re.I)


def redact(data):
    return "\n".join("[redacted credential-bearing log line]" if CREDENTIAL.search(line)
                     else DSN.sub("[redacted connection string]", line)
                     for line in data.decode("utf-8", "replace").splitlines()) + "\n"


def open_destination(destination):
    destination = Path(destination)
    if not destination.is_absolute() or destination.resolve() != destination:
        raise ValueError("artifact directory must be an absolute real directory")
    root_fd = os.open(destination, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    info = os.fstat(root_fd)
    if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) & 0o077:
        os.close(root_fd)
        raise ValueError("artifact directory must be owned and private")
    return root_fd


def preserve(destination, home, server_stdout):
    destination = Path(destination)
    root_fd = open_destination(destination)
    try:
        run = "native-recovery-" + uuid.uuid4().hex
        os.mkdir(run, mode=0o700, dir_fd=root_fd)
        run_fd = os.open(run, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=root_fd)
        try:
            home = Path(home)
            logs = {"server.log": home / ".config/aimee/server.log",
                    "native-ir-live.log": home / "native-ir-live.log",
                    "native-ir-crash-live.log": home / "native-ir-crash-live.log"}
            # Fixed module stdout allowlist: missing capability diagnoses must
            # survive the same disposable HOME cleanup as the daemon log.
            for module in ("config", "providers", "delegates", "memory", "policy", "tools",
                           "git", "learning", "economizer", "routing", "wfe"):
                logs["aimee-" + module + ".log"] = home / ("aimee-" + module + ".log")
            for module in ("pg", "db1"):
                logs[module + "-module.log"] = home / ".config/aimee" / (module + "-module.log")
            if server_stdout:
                logs["server-stdout.log"] = Path(server_stdout)
                logs["server-before-restart.log"] = Path(server_stdout + ".before-grade-restart")
            for name, source in logs.items():
                try:
                    source_fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW)
                except FileNotFoundError:
                    continue
                try:
                    source_info = os.fstat(source_fd)
                    if not stat.S_ISREG(source_info.st_mode):
                        raise ValueError("refusing nonregular diagnostic source")
                    os.lseek(source_fd, max(0, source_info.st_size - LIMIT), os.SEEK_SET)
                    data = os.read(source_fd, LIMIT)
                finally:
                    os.close(source_fd)
                output_fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                                    0o600, dir_fd=run_fd)
                with os.fdopen(output_fd, "wb") as output:
                    output.write(redact(data).encode("utf-8")[-LIMIT:])
        finally:
            os.close(run_fd)
        return destination / run
    finally:
        os.close(root_fd)


if __name__ == "__main__":
    try:
        if len(sys.argv) == 3 and sys.argv[1] == "--check":
            os.close(open_destination(sys.argv[2]))
            sys.exit(0)
        result = preserve(*sys.argv[1:])
    except (OSError, ValueError, TypeError):
        sys.exit("integration: diagnostic preservation refused; no configuration exported")
    print("integration: bounded redacted native diagnostics preserved at", result)
