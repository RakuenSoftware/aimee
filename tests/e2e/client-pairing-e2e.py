#!/usr/bin/env python3
"""Exercise independent thin clients against an explicitly disposable Docker stack.

Start the shipped compose.yaml with a candidate image, dashboard enabled, and
loopback ports 8443/8743. The Compose project MUST start aimee-pairing-e2e-.
This test replaces its bootstrap login, pairs devices, expires invitations,
revokes credentials, and restarts the application. Never target a user stack.
No credentials are printed. Uses the supplied release Linux client unchanged.
"""
import argparse
import concurrent.futures
import http.cookiejar
import json
import os
import re
from pathlib import Path
import secrets
import ssl
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def run(*args, data=None, env=None, timeout=120):
    result = subprocess.run(args, input=data, text=True, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if result.returncode:
        detail = re.sub(r'[0-9a-fA-F]{32,}', '[redacted]', result.stderr[-2000:]) if Path(args[0]).name == 'node' else ''
        raise RuntimeError('fixture command failed: ' + Path(args[0]).name + ' ' + detail)
    return result.stdout + result.stderr


def check(name, ok):
    if not ok:
        raise RuntimeError(name)
    print('PASS ' + name, flush=True)


class Browser:
    def __init__(self, url):
        self.url = url
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPSHandler(context=ssl._create_unverified_context()),
            urllib.request.HTTPCookieProcessor(self.jar))

    def request(self, path, body=None, token=None, api_key=None):
        headers = {'Content-Type': 'application/json', 'Origin': self.url}
        if token:
            headers['Authorization'] = 'Bearer ' + token
        if api_key:
            headers['x-api-key'] = api_key
        csrf = next((c.value for c in self.jar if 'csrf' in c.name.lower()), '')
        if csrf:
            headers['X-CSRF-Token'] = csrf
        req = urllib.request.Request(self.url + path,
            data=None if body is None else json.dumps(body).encode(), headers=headers)
        try:
            response = self.opener.open(req, timeout=45)
        except urllib.error.HTTPError as error:
            response = error
        data = response.read()
        try:
            data = json.loads(data)
        except ValueError:
            data = {}
        return response.status, data, response.headers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    parser.add_argument('--store-db', required=True)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--browser-script', type=Path)
    parser.add_argument('--web-url', default='https://127.0.0.1:8443')
    parser.add_argument('--api-url', default='https://127.0.0.1:8743')
    args = parser.parse_args()
    for container in (args.server, args.store_db):
        project = run('docker', 'inspect', '--format',
                      '{{index .Config.Labels "com.docker.compose.project"}}', container).strip()
        if not project.startswith('aimee-pairing-e2e-'):
            parser.error('refusing a container outside the disposable pairing project')
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        if run('docker', 'inspect', '--format', '{{.State.Health.Status}}', args.server).strip() == 'healthy':
            break
        time.sleep(2)
    else:
        raise RuntimeError('application did not become healthy')
    browser = Browser(args.web_url)
    check('client management requires login', browser.request('/api/clients')[0] == 401)
    raw = run('docker', 'logs', args.server)
    creds = {key: re.findall(r'\b' + key + r': ([a-z0-9-]+)', raw)[-1]
             for key in ('username', 'password')}
    check('bootstrap login works', browser.request('/api/auth/login', creds)[0] == 200)
    check('temporary bootstrap account cannot own devices', browser.request('/api/clients', {'name': 'Too early'})[0] == 409)
    password = secrets.token_hex(24)
    check('permanent owner account created', browser.request('/api/setup/account', dict(
        username='pairingowner', password=password, password_confirmation=password))[0] == 200)
    check('owner login works', browser.request('/api/auth/login', dict(username='pairingowner', password=password))[0] == 200)

    def create(name):
        code, body, headers = browser.request('/api/clients', {'name': name})
        if code != 200:
            raise RuntimeError('invitation status ' + str(code) + ': ' + str(body))
        check('create invitation for ' + name, len(body.get('bearer_token', '')) == 64)
        check('invitation response is not cached', 'no-store' in headers.get('Cache-Control', ''))
        return body

    def roster():
        code, body, _ = browser.request('/api/clients')
        check('owner can list clients', code == 200 and 'bearer_token' not in json.dumps(body))
        return body['clients']

    with tempfile.TemporaryDirectory(prefix='aimee-pairing-clients-') as directory:
        root = Path(directory)

        def client(device, *command, success=True):
            home = root / device
            home.mkdir(exist_ok=True, mode=0o700)
            env = dict(os.environ, AIMEE_HOME=str(home), XDG_CONFIG_HOME=str(home / 'xdg'))
            result = subprocess.run([str(args.client.resolve()), *command], env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=90)
            if (result.returncode == 0) != success:
                detail = re.sub(r'[0-9a-fA-F]{64}', '[redacted]', result.stdout + result.stderr)
                raise RuntimeError(device + ' ' + ' '.join(command[:2]) + ': ' + detail[-2000:])
            check(device + ' ' + ' '.join(command[:2]), (result.returncode == 0) == success)
            return result.stdout + result.stderr

        first, second = create('Workstation one'), create('Workstation two')
        api = Browser(args.api_url)
        check('device bearer also works through x-api-key', api.request(
            '/v1/health', token='separate-caller-proof', api_key=first['bearer_token'])[0] == 200)
        extra = [create('Additional device ' + str(i)) for i in range(8)]
        for invitation in extra:
            check('pending invitation cancellation preserves other devices', browser.request(
                '/api/clients/revoke', {'id': invitation['id']})[0] == 200)
        check('pending invitation cannot mint more tokens', api.request(
            '/v1/api/enroll_bearer', {}, token=first['bearer_token'])[0] == 401)
        run('docker', 'exec', args.server, 'useradd', '-m', '-G', 'aimee-webchat', 'pairingother')
        other_password = secrets.token_hex(24)
        run('docker', 'exec', '-i', args.server, 'chpasswd', data='pairingother:' + other_password + '\n')
        other = Browser(args.web_url)
        check('second dashboard user can log in', other.request('/api/auth/login',
            dict(username='pairingother', password=other_password))[0] == 200)
        for path, body in [('/api/clients', None), ('/api/clients', {'name': 'Unauthorized'}),
                           ('/api/clients/revoke', {'id': first['id']})]:
            check('another user cannot manage owner clients', other.request(path, body)[0] == 403)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            tasks = [pool.submit(client, device, 'remote', 'set', args.api_url, invitation['bearer_token'])
                     for device, invitation in [('first', first), ('second', second)]]
            for task in tasks:
                task.result()
        rows = roster()
        paired = [r for r in rows if r['state'] == 'paired']
        check('two devices have distinct paired certificates', len(paired) == 2 and
              len({r['serial'] for r in paired}) == 2)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            tasks = [pool.submit(client, device, 'memory', 'store', 'pairing-' + device,
                                  'Independent client pairing fixture ' + device)
                     for device in ('first', 'second')]
            for task in tasks:
                task.result()
        for device in ('first', 'second'):
            found = client(device, 'memory', 'search', 'Independent client pairing fixture')
            check(device + ' can recall shared owner memories', 'Independent client pairing fixture' in found)
        client('replay', 'remote', 'set', args.api_url, first['bearer_token'], success=False)
        expired = create('Expired invitation')
        sql = "UPDATE remote_client_grants SET expires_at=1 WHERE bearer_sha256='" + expired['id'] + "';"
        run('docker', 'exec', '-i', args.store_db, 'sh', '-c',
            'PGPASSWORD="$POSTGRES_PASSWORD" psql -h 127.0.0.1 -U postgres -d aimee_store -v ON_ERROR_STOP=1', data=sql)
        client('expired', 'remote', 'set', args.api_url, expired['bearer_token'], success=False)
        check('expiry appears in roster', any(r['id'] == expired['id'] and r['state'] == 'expired' for r in roster()))
        check('one client revocation succeeds', browser.request('/api/clients/revoke', {'id': first['id']})[0] == 200)
        check('revocation is retryable', browser.request('/api/clients/revoke', {'id': first['id']})[0] == 200)
        client('first', 'memory', 'store', 'revoked', 'Must be rejected', success=False)
        client('second', 'memory', 'store', 'survivor', 'Other client remains connected')
        run('docker', 'restart', args.server)
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            if run('docker', 'inspect', '--format', '{{.State.Health.Status}}', args.server).strip() == 'healthy':
                break
            time.sleep(2)
        else:
            raise RuntimeError('application did not recover after restart')
        client('second', 'memory', 'store', 'after-restart', 'Client pairing survives restart')
        client('first', 'memory', 'store', 'revoked-restart', 'Still rejected', success=False)
        check('owner login survives restart', browser.request('/api/auth/login', dict(username='pairingowner', password=password))[0] == 200)
        rows = roster()
        check('individual revocation persists', any(r['id'] == first['id'] and r['state'] == 'revoked' for r in rows))
        check('other device remains paired', any(r['id'] == second['id'] and r['state'] == 'paired' for r in rows))
        if args.browser_script:
            print(run('node', str(args.browser_script), data=json.dumps(dict(
                url=args.web_url, username='pairingowner', password=password))), end='')
        rotated = json.loads(run('docker', 'exec', args.server, 'curl', '-sS', '--max-time', '15',
            '--unix-socket', '/var/lib/aimee/aimee-http.sock', '-H', 'Content-Type: application/json',
            '-d', '{}', 'http://localhost/v1/api/rotate_bearer'))
        check('explicit global rotation succeeds', rotated.get('status') == 'ok')
        client('second', 'memory', 'store', 'after-global-revoke', 'Must be rejected', success=False)
    print('PASS independent client pairing end-to-end', flush=True)


if __name__ == '__main__':
    main()
