#!/usr/bin/env python3
"""Recording proxy, run INSIDE CT 9010.

Forwards every request to the real aimee-server and records the bodies. The
point is to see what the CLIENT put on the wire, driven by whatever the real
server served it -- not what a test harness believes the client would send.

Listens on 18898, upstream 18897.
"""
import http.server
import json
import urllib.request
import urllib.error
import threading

UPSTREAM = "http://127.0.0.1:18897"
RECORD = "/tmp/wire.jsonl"
_lock = threading.Lock()


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _proxy(self, method):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""

        if not self.path.startswith("/v1/cli/manifest"):
            with _lock:
                with open(RECORD, "a") as f:
                    try:
                        parsed = json.loads(body) if body else None
                    except Exception:
                        parsed = {"_unparseable": body[:400].decode("utf-8", "replace")}
                    f.write(json.dumps({"path": self.path, "body": parsed}) + "\n")

        req = urllib.request.Request(UPSTREAM + self.path, data=body or None, method=method)
        for k, v in self.headers.items():
            if k.lower() not in ("host", "content-length", "connection", "accept-encoding"):
                req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                data = r.read()
                code = r.getcode()
                ctype = r.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as e:
            data = e.read()
            code = e.code
            ctype = e.headers.get("Content-Type", "application/json")
        except Exception as e:
            data = json.dumps({"error": str(e)}).encode()
            code = 502
            ctype = "application/json"

        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self._proxy("GET")

    def do_POST(self):
        self._proxy("POST")

    def do_PUT(self):
        self._proxy("PUT")

    def do_DELETE(self):
        self._proxy("DELETE")


http.server.ThreadingHTTPServer(("127.0.0.1", 18898), H).serve_forever()
