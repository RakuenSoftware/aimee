#!/usr/bin/env python3
"""Prove llm-chat.py presents a client certificate, against a server that demands one.

The synthesis sidecar terminates mTLS and rejects an anonymous client. Before this,
llm-chat.py called urlopen with no TLS context at all, so it could not have
connected -- and the symptom would have been a handshake error at the first curation
call, pointing at TLS rather than at the missing capability.

A REAL HANDSHAKE, not a mock. The server below sets verify_mode=CERT_REQUIRED, so
the anonymous case fails for exactly the reason the sidecar would fail it. Mocking
ssl here would test that the code calls the functions it calls, which is worth
nothing.

stunnel is what terminates in production; this covers the client half only. The two
meet for the first time when an aimee-llm image runs.
"""
import http.server
import json
import os
import ssl
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LLM_CHAT = ROOT / "scripts" / "llm-chat.py"

failures = 0


def check(ok: bool, what: str, detail: str = "") -> None:
    global failures
    if ok:
        print(f"  ok    {what}")
        return
    print(f"  FAIL  {what}{(': ' + detail) if detail else ''}")
    failures += 1


def openssl(*args: str) -> None:
    subprocess.run(["openssl", *args], check=True, capture_output=True)


def make_pki(d: Path) -> dict[str, Path]:
    """A CA, a server certificate for localhost, and a client certificate."""
    ca_key, ca_crt = d / "ca.key", d / "ca.pem"
    # basicConstraints and keyUsage are not optional decoration: without them
    # OpenSSL refuses the chain with "CA cert does not include key usage ext", and
    # the test fixture rather than the client under test is what fails.
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-keyout", str(ca_key), "-out", str(ca_crt), "-subj", "/CN=test-ca",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")

    out: dict[str, Path] = {"ca": ca_crt}
    for name, cn in (("server", "localhost"), ("client", "aimee-kb-synthesis")):
        key, csr, crt = d / f"{name}.key", d / f"{name}.csr", d / f"{name}.pem"
        openssl("req", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(key), "-out", str(csr), "-subj", f"/CN={cn}")
        ext = d / f"{name}.ext"
        purpose = "serverAuth" if name == "server" else "clientAuth"
        ext.write_text(
            f"basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n"
            f"extendedKeyUsage={purpose}\n"
            + (f"subjectAltName=DNS:{cn}\n" if name == "server" else "")
        )
        openssl("x509", "-req", "-in", str(csr), "-CA", str(ca_crt), "-CAkey", str(ca_key),
                "-CAcreateserial", "-days", "1", "-out", str(crt),
                "-extfile", str(ext))
        out[f"{name}_key"], out[f"{name}_crt"] = key, crt
    return out


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self) -> None:  # noqa: N802
        self.rfile.read(int(self.headers.get("content-length", 0) or 0))
        payload = json.dumps({"choices": [{"message": {"content": "pong"}}]}).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_args) -> None:
        pass


def serve(pki: dict[str, Path]) -> tuple[http.server.HTTPServer, int]:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=pki["server_crt"], keyfile=pki["server_key"])
    ctx.load_verify_locations(cafile=pki["ca"])
    # The whole point: an anonymous client is refused, exactly as stunnel's
    # verifyPeer would refuse it.
    ctx.verify_mode = ssl.CERT_REQUIRED

    httpd = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, httpd.server_address[1]


def run_client(port: int, env_extra: dict[str, str]) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env.update({
        "SYNTHESIS_ENDPOINT": f"https://localhost:{port}/v1",
        "SYNTHESIS_MODEL": "test",
        "LLM_RETRIES": "0",
        "LLM_TIMEOUT": "15",
    })
    env.update(env_extra)
    return subprocess.run([sys.executable, str(LLM_CHAT), "--prompt", "ping"],
                          env=env, capture_output=True, text=True, timeout=60)


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        pki = make_pki(d)
        httpd, port = serve(pki)
        try:
            full = {
                "SYNTHESIS_CA_FILE": str(pki["ca"]),
                "SYNTHESIS_CERT_FILE": str(pki["client_crt"]),
                "SYNTHESIS_KEY_FILE": str(pki["client_key"]),
            }

            print("with a client certificate")
            r = run_client(port, full)
            check(r.returncode == 0 and "pong" in r.stdout,
                  "the handshake completes and the reply is read",
                  (r.stderr or "").strip()[:200])

            print("without one")
            # The regression guard. Before client-cert support this was the ONLY
            # behaviour available, so it must be seen to fail.
            r = run_client(port, {})
            check(r.returncode != 0, "an anonymous client is refused")

            print("partially configured")
            # Two of three is always a mistake. Silently downgrading to anonymous
            # would surface as a handshake error pointing nowhere near the cause.
            r = run_client(port, {k: v for k, v in full.items() if k != "SYNTHESIS_KEY_FILE"})
            check(r.returncode != 0 and "SYNTHESIS_KEY_FILE" in (r.stderr or ""),
                  "missing SYNTHESIS_KEY_FILE is named, not ignored",
                  (r.stderr or "").strip()[:200])
        finally:
            httpd.shutdown()

    if failures:
        print(f"\nllm-chat mTLS: {failures} check(s) failed")
        return 1
    print("\nllm-chat mTLS: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
