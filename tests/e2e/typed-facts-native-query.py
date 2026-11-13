#!/usr/bin/env python3
"""Run one graph tool through a real native worker and its next provider send.

Only for the disposable typed-facts stack. The synthetic provider requests the
named tool and records its exact result on the subsequent request: this tests
canonical receipt custody and current source revalidation, not just HTTP data.
"""
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
import uuid


class Local(http.client.HTTPConnection):
    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(self.timeout)
        self.sock.connect('/var/lib/aimee/aimee-http.sock')


def api(path, body=None):
    conn = Local('localhost', timeout=90)
    try:
        conn.request('GET' if body is None else 'POST', path,
                     None if body is None else json.dumps(body), {'Content-Type': 'application/json'})
        response = conn.getresponse()
        return response.status, json.loads(response.read())
    finally:
        conn.close()


def main():
    query = json.load(sys.stdin)
    prefix = 'typed-graph-' + uuid.uuid4().hex[:10]
    outputs, requests = [], []

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            requests.append(body)
            for message in body.get('messages', []):
                if message.get('role') == 'tool':
                    outputs.append(message.get('content', ''))
            message, reason = dict(role='assistant', content='GRAPH_QUERY_OK'), 'stop'
            if len(requests) == 1:
                message = dict(role='assistant', content=None, tool_calls=[dict(
                    id=prefix, type='function', function=dict(name=query['tool'],
                    arguments=json.dumps(query['arguments'])))])
                reason = 'tool_calls'
            raw = json.dumps(dict(id=prefix, object='chat.completion', model=body['model'],
                choices=[dict(index=0, message=message, finish_reason=reason)],
                usage=dict(prompt_tokens=11, completion_tokens=2, total_tokens=13))).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

    provider = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    roster = Path('/var/lib/aimee/models.json')
    previous = roster.read_bytes() if roster.exists() else None
    env = dict(os.environ, AIMEE_API_ENDPOINT='unix:/var/lib/aimee/aimee-http.sock')

    def config(*args):
        return json.loads(subprocess.check_output(['aimee', '--json', 'config', *args], env=env, text=True))

    previous_mode, run_id = None, None
    try:
        previous_mode = config('get', 'kb_mode')['value']
        config('set', 'kb_mode', 'none')
        roster.write_text(json.dumps(dict(default_agent=prefix, default_delegate=prefix, fallback_chain=[],
            models=[dict(name=prefix, model=prefix, provider='openai', auth_type='none',
                endpoint=f'http://127.0.0.1:{provider.server_port}/v1', roles=['all'], enabled=True,
                tools_enabled=True, context_window=32768, max_tokens=4096, max_output=4096,
                max_parallel=1, max_turns=4)])))
        status, result = api('/v1/runs', dict(model=prefix, input='Use the graph tool for the synthetic typed fact.',
                                            max_output_tokens=32))
        if status != 200 or not result.get('id'):
            raise RuntimeError('native graph run was not queued')
        run_id = result['id']
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            status, result = api('/v1/runs/' + run_id)
            if status == 200 and result.get('status') in ('completed', 'failed', 'cancelled'):
                break
            time.sleep(0.2)
        else:
            api('/v1/runs/' + run_id + '/stop', {})
            raise RuntimeError('native graph run timed out')
        print(json.dumps(dict(status=result.get('status'), provider_requests=len(requests), outputs=outputs, error=result.get('error'))))
    finally:
        if previous is None:
            roster.unlink(missing_ok=True)
        else:
            roster.write_bytes(previous)
        if previous_mode is not None:
            config('set', 'kb_mode', str(previous_mode))
        provider.shutdown()
        provider.server_close()


if __name__ == '__main__':
    main()
