#!/usr/bin/env python3
"""Independent TLS/SigV4/AWS-eventstream mock for the P6c CT260 gate.

This server is test-only.  It accepts the fixed credentials below, verifies the
complete signed Bedrock request independently of aimee, and emits one selected
deterministic response case.  It never prints request headers or bodies.

Example:
  python3 scripts/p6c_bedrock_mock.py \
    --cert /root/tls/server.crt --key /root/tls/server.key \
    --case stream-success --port 9443 \
    --expected-path /model/model/converse-stream \
    --counter-file /run/p6c-mock.accepted --ready-file /run/p6c-mock.ready

The certificate SAN must contain the signed Bedrock Runtime host.  The client
connects to this loopback listener through its test-only socket override while
retaining that AWS hostname for Host, SNI, and certificate verification.

Run the dependency-free internal checks with:
  python3 scripts/p6c_bedrock_mock.py --self-test
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import hmac
import ipaddress
import json
import os
import re
import ssl
import struct
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from typing import Any, Iterable


TEST_ACCESS_KEY = "AKIDEXAMPLE"
TEST_SECRET_KEY = "secret"
TEST_SESSION_TOKEN = "token"
TEST_AMZ_DATE = "20260101T000000Z"
TEST_DATE = "20260101"
MAX_REQUEST_BODY = 16 * 1024 * 1024

CASES = (
    "nonstream-success",
    "stream-success",
    "fragmented-stream-success",
    "wrong-media",
    "non-2xx",
    "bad-crc",
    "semantic-truncation",
    "complete-frame-semantic-truncation",
    "malformed-framing",
)

AUTH_RE = re.compile(
    r"\AAWS4-HMAC-SHA256 Credential=([^/]+)/([^,]+), "
    r"SignedHeaders=([^,]+), Signature=([0-9a-f]{64})\Z"
)
HEADER_NAME_RE = re.compile(r"\A[a-z0-9-]+\Z")


class DuplicateKey(ValueError):
    """Raised when the request contains an ambiguous JSON object."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKey(key)
        result[key] = value
    return result


def _valid_content_block(block: Any) -> bool:
    if not isinstance(block, dict) or len(block) != 1:
        return False
    kind, value = next(iter(block.items()))
    if kind == "text":
        return isinstance(value, str) and bool(value)
    if kind == "image":
        return (
            isinstance(value, dict)
            and set(value) == {"format", "source"}
            and value.get("format") in ("png", "jpeg", "gif", "webp")
            and isinstance(value.get("source"), dict)
            and set(value["source"]) == {"bytes"}
            and isinstance(value["source"]["bytes"], str)
            and bool(value["source"]["bytes"])
        )
    if kind == "toolUse":
        return (
            isinstance(value, dict)
            and set(value) == {"toolUseId", "name", "input"}
            and isinstance(value.get("toolUseId"), str)
            and isinstance(value.get("name"), str)
            and isinstance(value.get("input"), dict)
        )
    if kind == "toolResult":
        return (
            isinstance(value, dict)
            and set(value) == {"toolUseId", "content", "status"}
            and isinstance(value.get("toolUseId"), str)
            and isinstance(value.get("content"), list)
            and bool(value["content"])
            and all(
                isinstance(part, dict)
                and len(part) == 1
                and (
                    ("text" in part and isinstance(part["text"], str))
                    or ("json" in part and isinstance(part["json"], (dict, list)))
                )
                for part in value["content"]
            )
            and value.get("status") in ("success", "error")
        )
    if kind == "reasoningContent":
        if not isinstance(value, dict) or set(value) != {"reasoningText"}:
            return False
        text = value["reasoningText"]
        return (
            isinstance(text, dict)
            and set(text).issubset({"text", "signature"})
            and set(text).issuperset({"text"})
            and isinstance(text["text"], str)
            and ("signature" not in text or isinstance(text["signature"], str))
        )
    return False


def validate_body(raw: bytes) -> bool:
    """Validate the generic Converse body without importing production fixtures."""
    try:
        body = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_unique_object,
            parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)),
        )
    except (UnicodeDecodeError, ValueError):
        return False
    if not isinstance(body, dict) or not set(body).issubset(
        {"system", "messages", "inferenceConfig", "toolConfig"}
    ):
        return False
    messages = body.get("messages")
    if not isinstance(messages, list) or not messages:
        return False
    for message in messages:
        if (
            not isinstance(message, dict)
            or set(message) != {"role", "content"}
            or message["role"] not in ("user", "assistant")
            or not isinstance(message["content"], list)
            or not message["content"]
            or not all(_valid_content_block(block) for block in message["content"])
        ):
            return False
    system = body.get("system", [])
    if not isinstance(system, list) or not all(
        isinstance(part, dict) and set(part) == {"text"} and isinstance(part["text"], str)
        for part in system
    ):
        return False
    if "inferenceConfig" in body:
        config = body["inferenceConfig"]
        if not isinstance(config, dict) or not config or not set(config).issubset(
            {"maxTokens", "temperature", "topP", "stopSequences"}
        ):
            return False
        if "maxTokens" in config and (
            isinstance(config["maxTokens"], bool)
            or not isinstance(config["maxTokens"], int)
            or config["maxTokens"] <= 0
        ):
            return False
        for key in ("temperature", "topP"):
            if key in config and (
                isinstance(config[key], bool)
                or not isinstance(config[key], (int, float))
                or not 0 <= config[key] <= 1
            ):
                return False
        if "stopSequences" in config and (
            not isinstance(config["stopSequences"], list)
            or not config["stopSequences"]
            or not all(isinstance(item, str) and item for item in config["stopSequences"])
        ):
            return False
    if "toolConfig" in body:
        config = body["toolConfig"]
        if (
            not isinstance(config, dict)
            or not config
            or not set(config).issubset({"tools", "toolChoice"})
            or "tools" not in config
        ):
            return False
        if "tools" in config:
            tools = config["tools"]
            if not isinstance(tools, list) or not tools:
                return False
            for tool in tools:
                if not isinstance(tool, dict) or set(tool) != {"toolSpec"}:
                    return False
                spec = tool["toolSpec"]
                if (
                    not isinstance(spec, dict)
                    or not set(spec).issubset({"name", "description", "inputSchema"})
                    or not set(spec).issuperset({"name", "inputSchema"})
                    or not isinstance(spec["name"], str)
                    or not spec["name"]
                    or (
                        "description" in spec and not isinstance(spec["description"], str)
                    )
                    or not isinstance(spec["inputSchema"], dict)
                    or set(spec["inputSchema"]) != {"json"}
                    or not isinstance(spec["inputSchema"]["json"], dict)
                ):
                    return False
        if "toolChoice" in config:
            choice = config["toolChoice"]
            if not isinstance(choice, dict) or len(choice) != 1:
                return False
            choice_type, choice_value = next(iter(choice.items()))
            if choice_type in ("auto", "any"):
                if choice_value != {}:
                    return False
            elif choice_type == "tool":
                if (
                    not isinstance(choice_value, dict)
                    or set(choice_value) != {"name"}
                    or not isinstance(choice_value["name"], str)
                    or not choice_value["name"]
                ):
                    return False
            else:
                return False
    return True


def _hmac(key: bytes, data: str) -> bytes:
    return hmac.new(key, data.encode("utf-8"), hashlib.sha256).digest()


def expected_signature(
    path: str,
    headers: dict[str, str],
    signed_names: list[str],
    payload_hash: str,
    region: str,
) -> str:
    canonical_headers = "".join(
        f"{name}:{' '.join(headers[name].strip().split())}\n" for name in signed_names
    )
    canonical_request = (
        f"POST\n{path}\n\n{canonical_headers}\n"
        f"{';'.join(signed_names)}\n{payload_hash}"
    )
    scope = f"{TEST_DATE}/{region}/bedrock/aws4_request"
    string_to_sign = (
        "AWS4-HMAC-SHA256\n"
        f"{TEST_AMZ_DATE}\n{scope}\n"
        f"{hashlib.sha256(canonical_request.encode('utf-8')).hexdigest()}"
    )
    key_date = _hmac(("AWS4" + TEST_SECRET_KEY).encode("utf-8"), TEST_DATE)
    key_region = _hmac(key_date, region)
    key_service = _hmac(key_region, "bedrock")
    key_signing = _hmac(key_service, "aws4_request")
    return hmac.new(key_signing, string_to_sign.encode("utf-8"), hashlib.sha256).hexdigest()


def eventstream_frame(event_type: str, payload: dict[str, Any]) -> bytes:
    """Encode an AWS eventstream frame independently using documented wire fields."""
    headers = bytearray()
    for name, value in (
        (":message-type", "event"),
        (":event-type", event_type),
        (":content-type", "application/json"),
    ):
        name_bytes = name.encode("ascii")
        value_bytes = value.encode("utf-8")
        headers += bytes((len(name_bytes),)) + name_bytes + bytes((7,))
        headers += struct.pack(">H", len(value_bytes)) + value_bytes
    body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    total_length = 16 + len(headers) + len(body)
    prelude = struct.pack(">II", total_length, len(headers))
    prefix = prelude + struct.pack(">I", binascii.crc32(prelude) & 0xFFFFFFFF)
    message = prefix + headers + body
    return message + struct.pack(">I", binascii.crc32(message) & 0xFFFFFFFF)


def stream_payload() -> bytes:
    events = (
        ("messageStart", {"role": "assistant"}),
        ("contentBlockDelta", {"contentBlockIndex": 0, "delta": {"text": "mock-completion"}}),
        ("contentBlockStop", {"contentBlockIndex": 0}),
        ("messageStop", {"stopReason": "end_turn"}),
        ("metadata", {"usage": {"inputTokens": 1, "outputTokens": 1}}),
    )
    return b"".join(eventstream_frame(name, payload) for name, payload in events)


def _send_fragmented(connection: Any, data: bytes, widths: Iterable[int]) -> None:
    offset = 0
    for width in widths:
        if offset == len(data):
            break
        end = min(len(data), offset + width)
        connection.sendall(data[offset:end])
        offset = end
    if offset < len(data):
        connection.sendall(data[offset:])


def _chunked_response(connection: Any, body: bytes) -> None:
    head = (
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: application/vnd.amazon.eventstream\r\n"
        b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
    )
    _send_fragmented(connection, head, (1, 2, 7, 3, 13))
    widths = (1, 17, 2, 31, 5, 67, 3, 127)
    offset = 0
    index = 0
    while offset < len(body):
        width = widths[index % len(widths)]
        chunk = body[offset : offset + width]
        wire = f"{len(chunk):X}\r\n".encode("ascii") + chunk + b"\r\n"
        _send_fragmented(connection, wire, (1, 2, 3, 5))
        offset += len(chunk)
        index += 1
    _send_fragmented(connection, b"0\r\n\r\n", (1, 1, 2, 1))


class MockServer(HTTPServer):
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], args: argparse.Namespace):
        self.args = args
        self.accepted = 0
        super().__init__(address, MockHandler)

    def mark_accepted(self) -> None:
        self.accepted += 1
        if self.args.counter_file:
            path = Path(self.args.counter_file)
            temporary = path.with_name(path.name + ".tmp")
            temporary.write_text(f"{self.accepted}\n", encoding="ascii")
            os.replace(temporary, path)


class MockHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: MockServer

    def log_message(self, _format: str, *args: object) -> None:
        return

    def _reject(self, reason: str, request_length: int = 0) -> None:
        print(
            f"case={self.server.args.case} rejected={reason} request_length={request_length}",
            file=sys.stderr,
            flush=True,
        )
        try:
            self.connection.sendall(
                b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
            )
        except OSError:
            pass
        self.close_connection = True

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        args = self.server.args
        if (
            self.request_version != "HTTP/1.1"
            or self.path != args.expected_path
            or not self.path.startswith("/")
            or "?" in self.path
        ):
            self._reject("path")
            return
        headers: dict[str, str] = {}
        for name in self.headers.keys():
            lower = name.lower()
            values = self.headers.get_all(name, failobj=[])
            if not HEADER_NAME_RE.fullmatch(lower) or len(values) != 1 or lower in headers:
                self._reject("headers")
                return
            headers[lower] = values[0]
        required = {
            "authorization",
            "content-length",
            "content-type",
            "host",
            "x-amz-content-sha256",
            "x-amz-date",
        }
        allowed = required | {"connection", "x-amz-security-token"}
        if (
            not required.issubset(headers)
            or not set(headers).issubset(allowed)
            or headers.get("connection") != "close"
        ):
            self._reject("headers")
            return
        try:
            length = int(headers["content-length"], 10)
        except ValueError:
            self._reject("length")
            return
        if length < 1 or length > MAX_REQUEST_BODY or str(length) != headers["content-length"]:
            self._reject("length")
            return
        body = self.rfile.read(length)
        if len(body) != length:
            self._reject("short-body", len(body))
            return
        if headers["host"] != args.expected_host or headers["content-type"] != "application/json":
            self._reject("authority-media", length)
            return
        payload_hash = hashlib.sha256(body).hexdigest()
        if not hmac.compare_digest(headers["x-amz-content-sha256"], payload_hash):
            self._reject("payload-hash", length)
            return
        if headers["x-amz-date"] != TEST_AMZ_DATE:
            self._reject("timestamp", length)
            return
        token_present = "x-amz-security-token" in headers
        if token_present != (not args.no_session_token) or (
            token_present
            and not hmac.compare_digest(headers["x-amz-security-token"], TEST_SESSION_TOKEN)
        ):
            self._reject("session-token", length)
            return
        match = AUTH_RE.fullmatch(headers["authorization"])
        if not match:
            self._reject("authorization-shape", length)
            return
        access_key, scope, signed_text, signature = match.groups()
        expected_scope = f"{TEST_DATE}/{args.region}/bedrock/aws4_request"
        signed_names = signed_text.split(";")
        expected_names = sorted(
            [
                "content-type",
                "host",
                "x-amz-content-sha256",
                "x-amz-date",
                *([] if args.no_session_token else ["x-amz-security-token"]),
            ]
        )
        if (
            access_key != TEST_ACCESS_KEY
            or scope != expected_scope
            or signed_names != expected_names
            or any(name not in headers for name in signed_names)
        ):
            self._reject("authorization-scope", length)
            return
        wanted = expected_signature(self.path, headers, signed_names, payload_hash, args.region)
        if not hmac.compare_digest(signature, wanted):
            self._reject("signature", length)
            return
        if not validate_body(body):
            self._reject("json-shape", length)
            return

        self.server.mark_accepted()
        print(
            f"case={args.case} accepted request_length={length} count={self.server.accepted}",
            file=sys.stderr,
            flush=True,
        )
        self._respond(args.case)
        self.close_connection = True

    def _respond(self, case: str) -> None:
        if case == "nonstream-success":
            body = json.dumps(
                {
                    "output": {
                        "message": {
                            "role": "assistant",
                            "content": [{"text": "mock-completion"}],
                        }
                    },
                    "stopReason": "end_turn",
                    "usage": {"inputTokens": 1, "outputTokens": 1},
                },
                separators=(",", ":"),
            ).encode("utf-8")
            self.connection.sendall(
                b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                + str(len(body)).encode("ascii")
                + b"\r\nConnection: close\r\n\r\n"
                + body
            )
            return
        if case in ("stream-success", "fragmented-stream-success"):
            _chunked_response(self.connection, stream_payload())
            return
        if case == "wrong-media":
            body = b"{}"
            self.connection.sendall(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n"
                b"Connection: close\r\n\r\n" + body
            )
            return
        if case == "non-2xx":
            body = b'{"message":"mock-provider-error"}'
            self.connection.sendall(
                b"HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\n"
                b"Content-Length: " + str(len(body)).encode("ascii")
                + b"\r\nConnection: close\r\n\r\n" + body
            )
            return
        if case == "bad-crc":
            body = bytearray(stream_payload())
            body[-1] ^= 0x01
            _chunked_response(self.connection, bytes(body))
            return
        if case in ("semantic-truncation", "complete-frame-semantic-truncation"):
            body = eventstream_frame("messageStart", {"role": "assistant"})
            body += eventstream_frame(
                "contentBlockDelta", {"contentBlockIndex": 0, "delta": {"text": "partial"}}
            )
            body += eventstream_frame("contentBlockStop", {"contentBlockIndex": 0})
            _chunked_response(self.connection, body)
            return
        if case == "malformed-framing":
            self.connection.sendall(
                b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                b"Content-Length: 2\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                b"2\r\n{}\r\n0\r\n\r\n"
            )
            return
        raise AssertionError("unreachable response case")


def self_test() -> None:
    request = b'{"messages":[{"role":"user","content":[{"text":"hello"}]}]}'
    assert validate_body(request)
    assert not validate_body(b'{"messages":[],"messages":[]}')
    frame = eventstream_frame("messageStart", {"role": "assistant"})
    total, header_length, prelude_crc = struct.unpack(">III", frame[:12])
    assert total == len(frame) and 0 < header_length < total - 16
    assert prelude_crc == (binascii.crc32(frame[:8]) & 0xFFFFFFFF)
    assert struct.unpack(">I", frame[-4:])[0] == (binascii.crc32(frame[:-4]) & 0xFFFFFFFF)
    headers = {
        "content-type": "application/json",
        "host": "bedrock-runtime.us-east-1.amazonaws.com",
        "x-amz-content-sha256": hashlib.sha256(request).hexdigest(),
        "x-amz-date": TEST_AMZ_DATE,
        "x-amz-security-token": TEST_SESSION_TOKEN,
    }
    names = sorted(headers)
    first = expected_signature(
        "/model/model/converse", headers, names, headers["x-amz-content-sha256"], "us-east-1"
    )
    second = expected_signature(
        "/model/model/converse", headers, names, headers["x-amz-content-sha256"], "us-east-1"
    )
    assert len(first) == 64 and hmac.compare_digest(first, second)
    print("p6c-bedrock-mock: self-test passed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--cert", help="PEM TLS certificate with the expected-host SAN")
    parser.add_argument("--key", help="PEM private key for --cert")
    parser.add_argument("--bind", default="127.0.0.1", help="loopback address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9443)
    parser.add_argument("--case", choices=CASES, default="nonstream-success")
    parser.add_argument("--region", default="us-east-1")
    parser.add_argument("--expected-host", help="exact signed Host (derived from region by default)")
    parser.add_argument("--expected-path", help="exact encoded origin-form request target")
    parser.add_argument("--no-session-token", action="store_true")
    parser.add_argument("--counter-file", help="atomically updated accepted-request count")
    parser.add_argument("--ready-file", help="created after the TLS listener is ready")
    args = parser.parse_args()
    if args.self_test:
        return args
    if not args.cert or not args.key:
        parser.error("--cert and --key are required")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    try:
        if not ipaddress.ip_address(args.bind).is_loopback:
            parser.error("--bind must be a numeric loopback address")
    except ValueError:
        parser.error("--bind must be a numeric loopback address")
    if not re.fullmatch(r"[a-z0-9-]+", args.region):
        parser.error("--region is invalid")
    if args.expected_host is None:
        args.expected_host = f"bedrock-runtime.{args.region}.amazonaws.com"
    streaming = args.case not in ("nonstream-success", "wrong-media", "non-2xx", "malformed-framing")
    if args.expected_path is None:
        args.expected_path = "/model/model/converse-stream" if streaming else "/model/model/converse"
    if (
        not args.expected_path.startswith("/")
        or args.expected_path.startswith("//")
        or any(char in args.expected_path for char in "?#\r\n")
    ):
        parser.error("--expected-path must be a strict origin-form target without query/fragment")
    return args


def main() -> int:
    args = parse_args()
    if args.self_test:
        self_test()
        return 0
    server = MockServer((args.bind, args.port), args)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(args.cert, args.key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    if args.counter_file:
        Path(args.counter_file).write_text("0\n", encoding="ascii")
    if args.ready_file:
        Path(args.ready_file).write_text(f"{server.server_port}\n", encoding="ascii")
    print(f"case={args.case} ready port={server.server_port}", file=sys.stderr, flush=True)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if args.ready_file:
            try:
                Path(args.ready_file).unlink()
            except FileNotFoundError:
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
