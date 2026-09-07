#!/usr/bin/env python3
"""Real LUKS + PostgreSQL + core Vault test, on a disposable Linux Docker VM.

Requires root, dm-crypt and loop devices. Creates only its named containers and
its own temporary storage. No LUKS credential is accepted from flags or env.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile
import time


def run(*args, timeout=90, check=True):
    return subprocess.run(args, check=check, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=timeout)


def wait(predicate, seconds=60):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.3)
    raise AssertionError("condition did not become ready")


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while block := f.read(1024 * 1024):
            h.update(block)
    return h.hexdigest()


def contains(path, marker):
    tail = b''
    with path.open('rb') as f:
        while block := f.read(1024 * 1024):
            if marker in tail + block:
                return True
            tail = block[-len(marker):]
    return False


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--server-image', required=True)
    p.add_argument('--postgres-image', required=True)
    p.add_argument('--evidence', required=True)
    p.add_argument('--keep', action='store_true')
    p.add_argument('--legacy-upgrade', action='store_true')
    args = p.parse_args()
    if os.geteuid() != 0:
        p.error('run in a disposable Linux Docker VM as root')
    for device in ['/dev/mapper/control', '/dev/loop-control']:
        if not Path(device).exists():
            p.error(f'missing required device: {device}')
    root = Path(tempfile.mkdtemp(prefix='aimee-luks-e2e-'))
    prefix = root.name
    pg, owner, rival = prefix + '-pg', prefix + '-owner', prefix + '-rival'
    legacy = prefix + '-legacy'
    results = []

    def passed(name):
        results.append({'check': name, 'passed': True})
        print('PASS:', name, flush=True)

    def mount(source, target, readonly=False):
        return ['--mount', f'type=bind,src={source},dst={target}' + (',readonly' if readonly else '')]

    for name in ['ciphertext', 'vault', 'control', 'tls', 'legacy']:
        (root / name).mkdir(mode=0o700)
    os.chown(root / 'vault', 1000, 1000)
    (root / 'control').chmod(0o755)
    (root / 'legacy').chmod(0o755)
    passwords = {k: secrets.token_hex(24) for k in [
        'POSTGRES_PASSWORD', 'AIMEE_STORE_MIGRATOR_PASSWORD', 'AIMEE_STORE_RUNTIME_PASSWORD']}
    dm_major = next(line.split()[0] for line in Path('/proc/devices').read_text().splitlines()
                    if line.split()[-1:] == ['device-mapper'])
    env = {'POSTGRES_USER': 'postgres', 'POSTGRES_DB': 'aimee_store',
           'AIMEE_POSTGRES_VOLUME_MIB': '256', **passwords}
    pg_args = ['docker', 'run', '-d', '--name', pg, '--network', 'none',
               '--cap-add', 'SYS_ADMIN', '--security-opt', 'apparmor=unconfined',
               '--device', '/dev/mapper/control', '--device', '/dev/loop-control',
               '--device-cgroup-rule', 'b 7:* rmw', '--device-cgroup-rule', f'b {dm_major}:* rmw',
               '--ulimit', 'core=0', '--ulimit', 'memlock=1073741824:1073741824']
    pg_args += mount(root/'ciphertext', '/var/lib/aimee-postgres')
    pg_args += mount(root/'control', '/run/aimee-postgres')
    pg_args += mount(root/'tls', '/run/aimee-store-tls')
    for key, value in env.items():
        pg_args += ['--env', key+'='+value]
    if args.legacy_upgrade:
        pg_args += mount(root/'legacy', '/mnt/aimee-postgres-legacy', readonly=True)
    pg_args += [args.postgres_image]

    def unlock(timeout=45, expect=True):
        run('docker', 'rm', '-f', owner, check=False)
        cmd = ['docker', 'run', '--rm', '--name', owner, '--network', 'none',
               '--user', '1000:1000', '--ulimit', 'core=0',
               '--ulimit', 'memlock=1073741824:1073741824', '--env', 'AIMEE_HOME=/var/lib/aimee']
        cmd += mount(root/'vault', '/var/lib/aimee')
        cmd += mount(root/'control', '/run/aimee-postgres', readonly=True)
        cmd += ['--entrypoint', '/usr/local/libexec/aimee-modules/aimee-module-postgres',
                args.server_image, '__aimee_postgres_unlock', '/run/aimee-postgres/storage.sock']
        try:
            result = run(*cmd, timeout=timeout, check=False)
            if expect and result.returncode != 0:
                raise AssertionError('Vault-backed unlock failed: '+result.stderr.decode(errors='replace'))
            if not expect and result.returncode == 0:
                raise AssertionError('unlock unexpectedly succeeded')
        except subprocess.TimeoutExpired:
            if expect:
                raise AssertionError('Vault-backed unlock timed out') from None
        finally:
            run('docker', 'rm', '-f', owner, check=False)

    def sql(query, check=True):
        return run('docker', 'exec', pg, 'psql', '-U', 'postgres', '-d', 'aimee_store',
                   '-Atqc', query, check=check)

    def db_ready():
        return sql('SELECT 1', check=False).returncode == 0

    def locked():
        return run('docker', 'exec', pg, 'test', '!', '-e',
                   '/var/lib/postgresql/data/pgdata/PG_VERSION', check=False).returncode == 0

    def legacy_digest():
        h = hashlib.sha256()
        for f in sorted((root/'legacy').rglob('*')):
            if f.is_file():
                h.update(str(f.relative_to(root/'legacy')).encode()+b'\0')
                h.update(bytes.fromhex(digest(f)))
        return h.hexdigest()

    try:
        upgrade_marker = 'Offline migration preserves complete memory 🦊'
        if args.legacy_upgrade:
            legacy_args = ['docker', 'run', '-d', '--name', legacy, '--network', 'none',
                           '--env', 'POSTGRES_USER=postgres', '--env', 'POSTGRES_DB=aimee_store',
                           '--env', 'POSTGRES_PASSWORD='+passwords['POSTGRES_PASSWORD'],
                           '--env', 'PGDATA=/var/lib/postgresql/data/pgdata']
            legacy_args += mount(root/'legacy', '/var/lib/postgresql/data')
            run(*legacy_args, 'postgres:18-bookworm')
            wait(lambda: run('docker', 'exec', legacy, 'psql', '-U', 'postgres', '-d',
                             'aimee_store', '-Atqc', 'SELECT 1', check=False).returncode == 0)
            run('docker', 'exec', legacy, 'psql', '-U', 'postgres', '-d', 'aimee_store', '-Atqc',
                "CREATE TABLE upgrade_regression(note text); INSERT INTO upgrade_regression VALUES ('"+upgrade_marker+"')")
            run('docker', 'stop', '--time', '60', legacy)
            legacy_before = legacy_digest()
        run(*pg_args)
        wait(lambda: (root/'control/storage.sock').exists())
        assert locked() and not (root/'ciphertext/postgres.luks').exists()
        passed('no initdb or database files before Vault unlock')
        unlock()
        wait(db_ready)
        passed('first boot unlocks from Vault and starts PostgreSQL')
        if args.legacy_upgrade:
            assert sql('SELECT note FROM upgrade_regression').stdout.decode().strip() == upgrade_marker
            assert legacy_digest() == legacy_before
            assert json.loads((root/'ciphertext/volume.json').read_text())['legacy_migrated']
            passed('offline plaintext cluster migrates intact into encrypted PostgreSQL')
            passed('verified legacy source remains unchanged for rollback')
        image = root/'ciphertext/postgres.luks'
        public = json.loads((root/'ciphertext/volume.json').read_text())
        assert run('cryptsetup', 'luksUUID', str(image)).stdout.decode().strip() == public['uuid']
        meta = json.loads(run('cryptsetup', 'luksDump', '--dump-json-metadata', str(image)).stdout)
        assert len(meta['keyslots']) == 1 and not meta['tokens']
        passed('LUKS2 header has one keyslot and no TPM or enrollment tokens')
        marker = ('LUKS-DATA-CANARY-'+secrets.token_hex(16)).encode()
        sql("CREATE TABLE luks_regression(id integer PRIMARY KEY, note text); "
            "INSERT INTO luks_regression VALUES (1, '"+marker.decode()+"')")
        assert sql('SELECT note FROM luks_regression WHERE id=1').stdout.strip() == marker
        assert not contains(image, marker)
        passed('database reads work and raw storage contains no plaintext canary')
        unlock()
        passed('application restart reuses the already unlocked database')
        rival_args = list(pg_args)
        rival_args[rival_args.index('--name')+1] = rival
        run(*rival_args)
        wait(lambda: run('docker', 'inspect', '-f', '{{.State.Running}}', rival).stdout.strip() == b'false')
        assert sql('SELECT note FROM luks_regression WHERE id=1').stdout.strip() == marker
        passed('concurrent container cannot take ownership of the same storage')
        run('docker', 'stop', '--time', '60', pg)
        run('docker', 'start', pg)
        wait(lambda: (root/'control/storage.sock').exists())
        unlock()
        wait(db_ready)
        assert sql('SELECT note FROM luks_regression WHERE id=1').stdout.strip() == marker
        passed('clean restart preserves the encrypted database and Vault key')
        run('docker', 'stop', '--time', '60', pg)
        before = digest(image)
        records = list((root/'vault/.vault').glob('*.json'))
        assert records
        for f in records:
            f.rename(f.with_suffix('.unavailable'))
        run('docker', 'start', pg)
        unlock(timeout=7, expect=False)
        assert locked() and digest(image) == before
        passed('missing Vault key leaves existing ciphertext untouched and PostgreSQL locked')
        for f in records:
            f.with_suffix('.unavailable').rename(f)
        unlock()
        wait(db_ready)
        assert sql('SELECT note FROM luks_regression WHERE id=1').stdout.strip() == marker
        passed('restoring Vault recovers the original encrypted database')
        run('docker', 'kill', '--signal', 'KILL', pg)
        run('docker', 'start', pg)
        unlock()
        wait(db_ready)
        assert sql('SELECT note FROM luks_regression WHERE id=1').stdout.strip() == marker
        passed('container crash recovers the encrypted mapping and PostgreSQL WAL')
        run('docker', 'stop', '--time', '60', pg)
        for file in records:
            original = file.read_bytes()
            file.write_bytes(b'{corrupt')
            before = digest(image)
            run('docker', 'start', pg)
            unlock(timeout=7, expect=False)
            assert locked() and digest(image) == before and file.read_bytes() == b'{corrupt'
            run('docker', 'stop', '--time', '60', pg)
            file.write_bytes(original)
        passed('corrupt Vault fails closed without replacing credentials or formatting storage')
    finally:
        Path(args.evidence).write_text(json.dumps({'checks': results, 'workspace': str(root)}, indent=2)+'\n')
        if args.keep:
            print('Retained disposable test workspace:', root, flush=True)
        else:
            for name in [owner, rival, pg, legacy]:
                run('docker', 'rm', '-f', name, check=False)
            shutil.rmtree(root)


if __name__ == '__main__':
    main()
