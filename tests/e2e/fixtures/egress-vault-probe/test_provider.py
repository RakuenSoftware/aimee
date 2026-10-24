"""Offline protocol qualification for the installed-helper gate's SSE fixture."""
import http.client
from http.server import ThreadingHTTPServer
import importlib.util
from pathlib import Path
import threading
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'egress-vault-e2e.py'
spec = importlib.util.spec_from_file_location('egress_vault_gate', SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class ProviderTest(unittest.TestCase):
    def setUp(self):
        self.state = dict(token='fixture-secret', accepted=0, rejected=0)
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), gate.provider_handler(self.state))
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)

    def connect(self):
        return http.client.HTTPConnection(*self.server.server_address, timeout=2)

    def test_authenticated_sse_exchange_echoes_frame_without_bearer(self):
        stream = self.connect()
        post = self.connect()
        self.addCleanup(stream.close)
        self.addCleanup(post.close)
        auth = {'Authorization': 'Bearer fixture-secret'}
        stream.request('GET', '/sse', headers=auth)
        reply = stream.getresponse()
        self.assertEqual(reply.status, 200)
        self.assertEqual(reply.readline(), b'event: endpoint\n')
        endpoint = reply.readline().decode().removeprefix('data: ').strip()
        self.assertEqual(reply.readline(), b'\n')
        frame = b'{"jsonrpc":"2.0","id":1,"method":"fixture/echo"}'
        post.request('POST', endpoint, body=frame, headers=auth)
        accepted = post.getresponse()
        self.assertEqual(accepted.status, 202)
        self.assertEqual(accepted.read(), b'')
        self.assertEqual(reply.readline(), b'event: message\n')
        self.assertEqual(reply.readline(), b'data: ' + frame + b'\n')
        self.assertEqual(reply.readline(), b'\n')
        self.assertEqual(self.state['accepted'], 2)
        self.assertEqual(self.state['rejected'], 0)

    def test_stale_bearer_is_refused_without_response_leak(self):
        connection = self.connect()
        self.addCleanup(connection.close)
        connection.request('GET', '/sse', headers={'Authorization': 'Bearer stale-secret'})
        response = connection.getresponse()
        self.assertEqual(response.status, 401)
        self.assertEqual(response.read(), b'')
        self.assertEqual(self.state['accepted'], 0)
        self.assertEqual(self.state['rejected'], 1)

    def test_unbound_post_is_rejected_without_response_leak(self):
        connection = self.connect()
        self.addCleanup(connection.close)
        connection.request('POST', '/messages/unknown', body=b'{}',
                           headers={'Authorization': 'Bearer fixture-secret'})
        response = connection.getresponse()
        self.assertEqual(response.status, 400)
        self.assertEqual(response.read(), b'')


if __name__ == '__main__':
    unittest.main()
