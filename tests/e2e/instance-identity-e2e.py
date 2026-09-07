#!/usr/bin/env python3
"""Verify immutable identities in two disposable compositions of the same image."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('server', 'kb', 'image', 'output'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    checks = []
    candidate = subprocess.check_output(['docker', 'image', 'inspect', '--format', '{{.Id}}', args.image]).decode().strip()
    for role, container in [('server', args.server), ('kb', args.kb)]:
        inspect = json.loads(subprocess.check_output(['docker', 'inspect', container]))[0]
        assert inspect['Image'] == candidate, 'roles did not use the same candidate image'
        home = next(m for m in inspect['Mounts'] if m['Destination'] == '/var/lib/aimee')
        source = home.get('Name') or home['Source']
        def read():
            return subprocess.check_output(['docker', 'exec', container, 'cat', '/var/lib/aimee/instance-identity.json'])
        before = read()
        assert json.loads(before)['role'] == role
        checks.append(dict(name=role + ' persists its first-boot identity', passed=True))
        other = 'kb' if role == 'server' else 'server'
        command = ['docker', 'run', '--rm', '-e', 'AIMEE_HOME=/var/lib/aimee', '-e', 'AIMEE_INSTANCE_ROLE=' + other, '-v', source + ':/var/lib/aimee', args.image, '/bin/true']
        result = subprocess.run(command, capture_output=True, timeout=20)
        assert result.returncode != 0 and b'forbidden' in result.stderr, 'role swap was accepted'
        assert hashlib.sha256(read()).digest() == hashlib.sha256(before).digest(), 'failed swap changed identity'
        checks.append(dict(name=role + ' rejects a role change without modifying its identity', passed=True))
        command[command.index('AIMEE_INSTANCE_ROLE=' + other)] = 'AIMEE_INSTANCE_ROLE=' + role
        result = subprocess.run(command, capture_output=True, timeout=20)
        assert result.returncode == 0, 'same-role restart refused'
        assert read() == before
        checks.append(dict(name=role + ' accepts the same image and role on restart', passed=True))
    Path(args.output).write_text(json.dumps(checks, indent=2) + '\n')
    print('PASS:', len(checks), 'instance identity checks')


if __name__ == '__main__':
    main()
