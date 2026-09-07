#!/usr/bin/env python3
"""Prepare fixed database/KB credentials in Vault before Compose creates services.

Called by compose-local.sh. Inspecting the Compose model is read-only; secret
values stay in memory and enter the disposable Vault bootstrap only over stdin.
Long-lived application definitions contain no SQL or enrollment credentials.
"""
import json
import subprocess
import sys

VALUE_OPTIONS = {'-f', '--file', '-p', '--project-name', '--env-file', '--project-directory',
                 '--profile', '--ansi', '--progress', '--parallel'}
FLAG_OPTIONS = {'--compatibility', '--all-resources'}


def up_prefix(args):
    prefix, i = [], 0
    while i < len(args):
        value = args[i]
        if value == '--dry-run':
            return None
        if value in VALUE_OPTIONS:
            if i + 1 >= len(args):
                return None
            prefix.extend(args[i:i + 2]); i += 2
        elif value in FLAG_OPTIONS or (value.startswith('--') and '=' in value and value.split('=')[0] in VALUE_OPTIONS):
            prefix.append(value); i += 1
        else:
            return prefix if value == 'up' else None
    return None


def sql_value(value):
    # libpq keyword/value quoting, including apostrophes and backslashes in
    # operator-chosen passwords. Reject NUL before constructing the stdin frame.
    if not isinstance(value, str) or not value or '\0' in value:
        raise ValueError('missing or invalid database bootstrap credential')
    return "'" + value.replace('\\', '\\\\').replace("'", "\\'") + "'"


def payloads(model):
    bootstrap_env = model.get('x-aimee-vault', {})
    services = model.get('services', {})
    owners = [name for name in ('aimee-server', 'aimee-kb') if name in services]
    if not owners:
        return []  # model-only compositions have no Vault to bootstrap
    if len(owners) != 1:
        raise ValueError('a Compose project must select one application identity')
    pg = services.get('aimee-store-db', {}).get('environment', {})
    def dsn(role, password):
        return ('host=aimee-store-db port=5432 dbname=aimee_store user=' + role +
                ' password=' + sql_value(pg.get(password)) +
                ' sslmode=verify-full sslrootcert=/run/aimee-store-tls/server.crt')
    values = {
        'AIMEE_STORE_URL': dsn('aimee_store_runtime', 'AIMEE_STORE_RUNTIME_PASSWORD'),
        'AIMEE_STORE_MIGRATION_URL': dsn('aimee_store_migrator', 'AIMEE_STORE_MIGRATOR_PASSWORD'),
    }
    owner = owners[0]
    if owner == 'aimee-kb':
        values['AIMEE_DB2_URL'] = values['AIMEE_STORE_MIGRATION_URL']
        bearer = bootstrap_env.get('AIMEE_KB_API_BEARER_TOKEN')
        if not isinstance(bearer, str) or not bearer or '\0' in bearer:
            raise ValueError('KB authority bootstrap credential is required')
        values['AIMEE_KB_API_BEARER_TOKEN'] = bearer
        identity = bootstrap_env.get('AIMEE_KB_SERVICE_IDENTITY_TOKEN')
        if identity:
            values['AIMEE_KB_SERVICE_IDENTITY_TOKEN'] = identity
    return [(owner, ''.join(key + '=' + value + '\0' for key, value in values.items()))]


def run(argv, **kwargs):
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120, **kwargs)
    if result.returncode:
        # Compose output and bootstrap diagnostics may include secret values.
        raise RuntimeError('Compose Vault preparation failed; check image availability, private volumes, and required bootstrap settings')
    return result.stdout


def main(args):
    prefix = up_prefix(args)
    if prefix is None:
        return 0
    compose = ['docker', 'compose', *prefix]
    model = json.loads(run([*compose, 'config', '--format', 'json']))
    # Root extension fields survive Compose normalization and are never placed
    # in Config.Env. They preserve Compose's env-file interpolation semantics.
    for owner, data in payloads(model):
        binary = 'aimee-kb' if owner == 'aimee-kb' else 'aimee-server'
        run([*compose, 'run', '--rm', '--no-deps', '-T', '--entrypoint',
             '/usr/sbin/runuser', owner, '-u', 'aimee', '--', binary, '--bootstrap-vault-stdin'],
            input=data.encode())
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError):
        print('compose-vault-init: failed to seal startup credentials; verify required settings, image availability, and Vault ownership', file=sys.stderr)
        raise SystemExit(1)
