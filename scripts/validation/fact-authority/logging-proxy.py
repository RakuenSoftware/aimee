#!/usr/bin/env python3
"""Log the exact request aimee-server sends upstream, then forward it.

Token counts only ever suggested whether the pre-injection envelope was present;
they never showed it. This sits between the server and llama-server and writes
each outgoing body to a file, so "is the envelope in the prompt" stops being an
inference.

  aimee-server  ->  127.0.0.1:8798  (this)  ->  UPSTREAM

Usage: logging-proxy.py [UPSTREAM]     default http://192.168.1.100:8762
Writes /root/proxy-capture.jsonl (one request body per line).
"""
import json
import sys
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

UPSTREAM = (sys.argv[1] if len(sys.argv) > 1 else "http://192.168.1.100:8762").rstrip("/")
CAPTURE = "/root/proxy-capture.jsonl"


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):  # noqa: N802
        raw = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        try:
            with open(CAPTURE, "a") as fh:
                fh.write(json.dumps({"path": self.path,
                                     "body": raw.decode("utf-8", "replace")}) + "\n")
        except OSError:
            pass
        req = urllib.request.Request(UPSTREAM + self.path, data=raw,
                                     headers={"content-type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=300) as up:
                body, code = up.read(), up.status
        except Exception as exc:  # noqa: BLE001
            body = json.dumps({"error": str(exc)}).encode()
            code = 502
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        try:
            with urllib.request.urlopen(UPSTREAM + self.path, timeout=60) as up:
                body, code = up.read(), up.status
        except Exception as exc:  # noqa: BLE001
            body, code = json.dumps({"error": str(exc)}).encode(), 502
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        return


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", 8798), Handler).serve_forever()
