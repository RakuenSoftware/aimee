#!/usr/bin/env python3
"""Collect provider evidence for an isolated native-runtime evaluation driver.

The driver emits ``EVAL_RELAY {id, body}`` lines and reads JSON replies on stdin.
Only explicit tool-schema completions are relayed. Native tools execute in the
driver's deployment. Pairing credentials never enter that deployment or logs.
This collector records usage, not billing cost or task-quality judgments.
"""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import time
from urllib.parse import urlsplit


class PairedProvider:
    def __init__(self, config, server_name=None):
        lines = (config / 'remote.conf').read_text().splitlines()
        url = urlsplit(lines[0])
        if url.scheme != 'https' or not url.hostname or url.username or url.password or url.path not in ('', '/') or url.query or url.fragment:
            raise ValueError('expected a paired HTTPS origin')
        if len(lines) < 2 or not lines[1]:
            raise ValueError('paired bearer is unavailable')
        self.host, self.port = url.hostname, url.port or 443
        self.server_name = server_name or self.host
        self.token = lines[1]
        ca = config / 'remote-ca.pem'
        self.pin = hashlib.sha256(ssl.PEM_cert_to_DER_cert(ca.read_text())).digest()
        self.context = ssl.create_default_context(cafile=str(ca))
        self.context.load_cert_chain(str(config / 'tls/client.crt'), str(config / 'tls/client.key'))

    def __call__(self, body):
        raw_socket = socket.create_connection((self.host, self.port), timeout=30)
        try:
            sock = self.context.wrap_socket(raw_socket, server_hostname=self.server_name)
        except BaseException:
            raw_socket.close()
            raise
        if hashlib.sha256(sock.getpeercert(binary_form=True)).digest() != self.pin:
            sock.close()
            raise ValueError('paired leaf certificate changed')
        sock.settimeout(180)
        connection = http.client.HTTPConnection(self.host, self.port, timeout=180)
        connection.sock = sock
        try:
            connection.request('POST', '/v1/chat/completions', json.dumps(body),
                               {'Authorization': 'Bearer ' + self.token, 'Content-Type': 'application/json'})
            response = connection.getresponse()
            raw = response.read(2 * 1024 * 1024 + 1)
            if len(raw) > 2 * 1024 * 1024:
                raise ValueError('provider response exceeds evidence bound')
            return response.status, json.loads(raw)
        finally:
            connection.close()


def request_body(packet, model):
    if not isinstance(packet, dict) or not isinstance(packet.get('id'), str) or not 1 <= len(packet['id']) <= 128:
        raise ValueError('invalid relay request identifier')
    body = packet.get('body')
    if not isinstance(body, dict) or len(json.dumps(body).encode()) > 131072:
        raise ValueError('evaluation request exceeds bound')
    if not isinstance(body.get('tools'), list) or not body['tools']:
        raise ValueError('explicit tools required to avoid production-native execution')
    ceiling = body.get('max_tokens', 4096)
    if type(ceiling) is not int or ceiling <= 0:
        raise ValueError('invalid output ceiling')
    return dict(body, model=model, stream=False, max_tokens=min(ceiling, 4096))


def valid_usage(response):
    usage = response.get('usage') if isinstance(response, dict) else None
    return isinstance(usage, dict) and all(
        type(usage.get(key)) is int and usage[key] >= 0
        for key in ('prompt_tokens', 'completion_tokens', 'total_tokens')
    ) and usage['total_tokens'] == usage['prompt_tokens'] + usage['completion_tokens']


def collect(command, output, model, max_calls, provider):
    if not 1 <= max_calls <= 2048:
        raise ValueError('invalid explicit call ceiling')
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    count = 0
    with (output / 'driver-stderr.log').open('w') as errors, (output / 'driver.log').open('w') as log:
        child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=errors, text=True, bufsize=1)
        try:
            for line in child.stdout:
                if not line.startswith('EVAL_RELAY '):
                    log.write(line)
                    log.flush()
                    continue
                if count >= max_calls:
                    raise ValueError('evaluation call ceiling reached')
                packet = json.loads(line[len('EVAL_RELAY '):])
                body = request_body(packet, model)
                started = time.monotonic()
                status, response = provider(body)
                elapsed = time.monotonic() - started
                count += 1
                # Keep both native and forwarded payloads: routing changes model,
                # streaming and output ceiling, so their bytes are not identical.
                evidence = dict(ordinal=count, native_request=packet['body'], request=body,
                                response=response, elapsed_seconds=elapsed, http_status=status)
                raw = json.dumps(evidence, sort_keys=True, separators=(',', ':')).encode()
                fd = os.open(output / f'call-{count:05}.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, 'wb') as file:
                    file.write(raw)
                if status != 200 or not valid_usage(response):
                    raise ValueError('provider response or complete usage unavailable; invalid cell')
                child.stdin.write(json.dumps(dict(id=packet['id'], status=status, body=response,
                                                 evidence_sha256=hashlib.sha256(raw).hexdigest(),
                                                 elapsed_seconds=elapsed)) + '\n')
                child.stdin.flush()
            code = child.wait(timeout=30)
            summary = dict(actual_exit=code, model_calls=count, billing_cost_available=False,
                           release_evidence=False)
            (output / 'collector-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
            return summary
        finally:
            child.stdin.close()
            if child.poll() is None:
                # Closing stdin lets the driver's finally restore its model roster.
                try:
                    child.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    child.terminate()
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
            child.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path.home() / '.config/aimee')
    parser.add_argument('--tls-server-name')
    parser.add_argument('--model', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--max-calls', type=int, required=True)
    parser.add_argument('driver', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.driver[1:] if args.driver[:1] == ['--'] else args.driver
    if not command:
        parser.error('an explicit isolated driver command is required')
    try:
        summary = collect(command, args.output, args.model, args.max_calls,
                          PairedProvider(args.config, args.tls_server_name))
    except Exception as error:
        # Provider/driver exceptions can contain credentials or private prompts.
        print(json.dumps(dict(collector_failed=True, error_type=type(error).__name__)))
        return 1
    print(json.dumps(summary))
    return summary['actual_exit']


if __name__ == '__main__':
    raise SystemExit(main())
