#!/usr/bin/python3 -I
"""Activate one existing managed PAM login in the installed private memory policy.

Root writes a bounded roster, never a request-provided native grant. The native
owner derives only the account's User scope from its installed templates.
"""
import argparse
import fcntl
import grp
import json
import os
from pathlib import Path
import pwd
import re
import secrets
import stat
import uuid

ACCOUNT = re.compile(r'[a-z_][a-z0-9_-]{0,31}\Z')
MAX_BYTES = 16 * 1024


def read_roster(fd, node):
    meta = os.fstat(fd)
    if not stat.S_ISREG(meta.st_mode) or meta.st_uid != 0 or meta.st_mode & 0o022 or meta.st_size > MAX_BYTES:
        raise ValueError('untrusted account registry')
    with os.fdopen(os.dup(fd), 'rb') as stream:
        raw = stream.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError('account registry exceeds bound')
    value = json.loads(raw)
    names = value.get('accounts')
    if (set(value) != {'schema_version', 'node', 'accounts'} or value['schema_version'] != 1
            or value['node'] != node or not isinstance(names, list) or len(names) > 32
            or any(not isinstance(n, str) or not ACCOUNT.fullmatch(n) for n in names)
            or len(set(names)) != len(names)):
        raise ValueError('account registry differs from this installation')
    return set(names)


def activate(username, home=Path('/var/lib/aimee'), configuration=Path('/opt/aims-memory/server/etc/database.json')):
    if os.geteuid() != 0 or not ACCOUNT.fullmatch(username):
        raise ValueError('activation requires the root account manager and a PAM login name')
    user, managed = pwd.getpwnam(username), grp.getgrnam('aimee-webchat')
    if user.pw_uid < 1000 or (user.pw_gid != managed.gr_gid and username not in managed.gr_mem):
        raise ValueError('account is outside the managed login group')
    config = json.loads(configuration.read_text())
    expected = home / 'private-memory' / 'pam-accounts.json'
    registry = config.get('pam_account_registry')
    if registry != dict(path=str(expected), user_template='aimee', maintenance_template='native-maintenance:aimee'):
        raise ValueError('installed memory has no supported PAM activation policy')
    templates = config['host_subject_grants']
    node = str(uuid.UUID(templates['aimee']['access']['local_node']))
    parent = os.open(expected.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    temporary = None
    try:
        lock = os.open('.pam-accounts.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=parent)
        try:
            meta = os.fstat(lock)
            if not stat.S_ISREG(meta.st_mode) or meta.st_uid != 0 or meta.st_mode & 0o022:
                raise ValueError('untrusted activation lock')
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                fd = os.open(expected.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
            except FileNotFoundError:
                names = set()
            else:
                try:
                    names = read_roster(fd, node)
                finally:
                    os.close(fd)
            names.add(username)
            installed = {n for n, g in templates.items() if g.get('access', {}).get('principal', {}).get('kind') == 'pam'
                         and ACCOUNT.fullmatch(n)}
            if len(names | installed) > 32:
                raise ValueError('memory account capacity reached')
            raw = (json.dumps(dict(schema_version=1, node=node, accounts=sorted(names)), sort_keys=True) + '\n').encode()
            temporary = '.pam-accounts-' + secrets.token_hex(16)
            fd = os.open(temporary, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600, dir_fd=parent)
            try:
                with os.fdopen(os.dup(fd), 'wb') as stream:
                    stream.write(raw)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.fchown(fd, 0, pwd.getpwnam('aimee').pw_gid)
                os.fchmod(fd, 0o640)
                os.fsync(fd)
            finally:
                os.close(fd)
            os.replace(temporary, expected.name, src_dir_fd=parent, dst_dir_fd=parent)
            temporary = None
            os.fsync(parent)
        finally:
            os.close(lock)
    finally:
        if temporary:
            os.unlink(temporary, dir_fd=parent)
        os.close(parent)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('username')
    args = parser.parse_args()
    try:
        activate(args.username, Path(os.environ.get('AIMEE_HOME', '/var/lib/aimee')))
    except (OSError, ValueError, KeyError, TypeError):
        # No account roster, grant configuration or shadow data reaches stdout.
        parser.exit(1, 'private memory account activation failed\n')
    print('{"schema_version":1,"active":true}')


if __name__ == '__main__':
    main()
