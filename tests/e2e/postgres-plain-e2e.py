#!/usr/bin/env python3
"""Ordinary Docker PostgreSQL persistence and explicit storage-mode boundaries."""
import argparse
import json
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile
import time


def run(*args, check=True):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    if check and result.returncode:
        raise RuntimeError('storage fixture command failed (arguments withheld)')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--postgres-image', required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix='aimee-plain-e2e-'))
    name = root.name
    checks = []
    for directory in ('data', 'tls', 'encrypted'):
        (root / directory).mkdir(mode=0o755)
    passwords = {key: secrets.token_hex(24) for key in ('POSTGRES_PASSWORD', 'AIMEE_STORE_MIGRATOR_PASSWORD', 'AIMEE_STORE_RUNTIME_PASSWORD')}

    def check(label, passed):
        checks.append(dict(name=label, passed=bool(passed)))
        if not passed:
            raise RuntimeError(label)

    def start(mode=None, directory='data'):
        run('docker', 'rm', '-f', name, check=False)
        command = ['docker', 'run', '-d', '--name', name, '--network', 'none',
                   '--mount', f'type=bind,src={root / directory},dst=/var/lib/aimee-postgres',
                   '--mount', f'type=bind,src={root / "tls"},dst=/run/aimee-store-tls']
        env = dict(POSTGRES_USER='postgres', POSTGRES_DB='aimee_store', **passwords)
        if mode is not None:
            env['AIMEE_POSTGRES_STORAGE'] = mode
        for key, value in env.items():
            command += ['--env', key + '=' + value]
        run(*command, args.postgres_image)

    def sql(query, admin=False, tls=True):
        role = 'postgres' if admin else 'aimee_store_runtime'
        password = '$POSTGRES_PASSWORD' if admin else '$AIMEE_STORE_RUNTIME_PASSWORD'
        return run('docker', 'exec', name, 'sh', '-c',
                   'PGPASSWORD="' + password + '" PGSSLMODE=' + ('require' if tls else 'disable') +
                   ' psql -h 127.0.0.1 -U ' + role + ' -d aimee_store -Atqc "$1"', 'sh', query, check=False)

    def ready():
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if sql('SELECT 1').returncode == 0:
                return
            time.sleep(1)
        raise RuntimeError('ordinary PostgreSQL did not become ready')

    def refused():
        # No restart policy: an invalid mode must exit rather than initialize.
        result = run('docker', 'wait', name)
        return result.stdout.strip() != b'0'

    try:
        start()
        ready()
        host = json.loads(run('docker', 'inspect', '--format', '{{json .HostConfig}}', name).stdout)
        check('default starts without elevated capabilities or host devices',
              not any(host.get(key) for key in ('Privileged', 'CapAdd', 'Devices', 'DeviceCgroupRules')))
        check('default persists an ordinary cluster without a LUKS image',
              (root / 'data/plain/pgdata/PG_VERSION').exists() and not (root / 'data/postgres.luks').exists())
        check('unencrypted SQL transport remains refused', sql('SELECT 1', tls=False).returncode != 0)
        check('runtime role remains non-superuser', sql('SELECT rolsuper FROM pg_roles WHERE rolname=current_user').stdout.strip() == b'f')
        check('create persistent fixture', sql('CREATE TABLE public.storage_canary(value text); INSERT INTO public.storage_canary VALUES (\'persisted\'); GRANT SELECT ON public.storage_canary TO aimee_store_runtime', admin=True).returncode == 0)
        run('docker', 'restart', name)
        ready()
        check('ordinary store retains data across PostgreSQL restart', sql('SELECT value FROM public.storage_canary').stdout.strip() == b'persisted')
        start('plain')
        ready()
        check('explicit plain mode retains data across container replacement', sql('SELECT value FROM public.storage_canary').stdout.strip() == b'persisted')
        start('luks')
        check('LUKS refuses implicit conversion of an existing plain store', refused() and not (root / 'data/postgres.luks').exists())
        start('unsupported')
        check('unknown storage mode is refused', refused())
        marker = root / 'encrypted/postgres.luks'
        marker.write_bytes(b'preserve encrypted storage fixture')
        start(directory='encrypted')
        check('default refuses encrypted storage without creating an empty database', refused() and not (root / 'encrypted/plain').exists() and marker.read_bytes() == b'preserve encrypted storage fixture')
        start()
        ready()
        check('refused mode switches leave the original database usable', sql('SELECT value FROM public.storage_canary').stdout.strip() == b'persisted')
    finally:
        run('docker', 'rm', '-f', name, check=False)
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps(checks, indent=2) + '\n')
        shutil.rmtree(root)
    print('postgres-plain-e2e: all checks passed')


if __name__ == '__main__':
    main()
