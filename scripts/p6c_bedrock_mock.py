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

The certificate SAN must contain the signed Bedrock Runtime host.  The CT gate
maps that hostname to this loopback listener while retaining it for Host, SNI,
and certificate verification.

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
import socket
import ssl
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path
from socketserver import BaseRequestHandler, TCPServer
from typing import Any, Iterable


TEST_ACCESS_KEY = "AKIDEXAMPLE"
TEST_SECRET_KEY = "secret"
TEST_SESSION_TOKEN = "token"
TEST_AMZ_DATE = "20260101T000000Z"
TEST_DATE = "20260101"
MAX_REQUEST_BODY = 16 * 1024 * 1024
MAX_REQUEST_HEAD = 64 * 1024
MAX_REQUEST_HEADERS = 32

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
    "unclean-eof",
)

AUTH_RE = re.compile(
    r"\AAWS4-HMAC-SHA256 Credential=([^/]+)/([^,]+), "
    r"SignedHeaders=([^,]+), Signature=([0-9a-f]{64})\Z"
)
HEADER_NAME_RE = re.compile(rb"\A[!#$%&'*+\-.^_`|~0-9A-Za-z]+\Z")
REQUEST_LINE_RE = re.compile(rb"\APOST ([\x21-\x7e]+) HTTP/1\.1\Z")


class WireError(ValueError):
    """A safe, body-free reason for rejecting the raw HTTP request."""


def _origin_target_valid(target: bytes) -> bool:
    if not target.startswith(b"/") or target.startswith(b"//"):
        return False
    allowed = b"/-._~!$&'()*+,;=:@"
    index = 1
    while index < len(target):
        value = target[index]
        if value == ord("%"):
            if index + 2 >= len(target) or not all(
                byte in b"0123456789abcdefABCDEF" for byte in target[index + 1 : index + 3]
            ):
                return False
            index += 3
            continue
        if (
            ord("0") <= value <= ord("9")
            or ord("A") <= value <= ord("Z")
            or ord("a") <= value <= ord("z")
            or value in allowed
        ):
            index += 1
            continue
        return False
    return True


def parse_request_head(raw: bytes) -> tuple[str, dict[str, str], int]:
    """Parse one bounded HTTP/1.1 request head without normalization."""
    if len(raw) > MAX_REQUEST_HEAD or not raw.endswith(b"\r\n\r\n"):
        raise WireError("head-framing")
    if re.search(rb"(?<!\r)\n|\r(?!\n)", raw):
        raise WireError("line-ending")
    lines = raw[:-4].split(b"\r\n")
    if not lines or len(lines) - 1 > MAX_REQUEST_HEADERS:
        raise WireError("request-lines")
    match = REQUEST_LINE_RE.fullmatch(lines[0])
    if not match or not _origin_target_valid(match.group(1)):
        raise WireError("request-line")
    target = match.group(1).decode("ascii")
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if not line or line.startswith((b" ", b"\t")):
            raise WireError("header-fold")
        colon = line.find(b":")
        if colon <= 0 or line[colon : colon + 2] != b": ":
            raise WireError("header-colon")
        name_bytes = line[:colon]
        value_bytes = line[colon + 2 :]
        if not HEADER_NAME_RE.fullmatch(name_bytes):
            raise WireError("header-name")
        if (
            not value_bytes
            or value_bytes[:1] in (b" ", b"\t")
            or value_bytes[-1:] in (b" ", b"\t")
            or any(byte < 0x20 or byte >= 0x7F for byte in value_bytes)
        ):
            raise WireError("header-whitespace")
        name = name_bytes.decode("ascii").lower()
        if name in headers:
            raise WireError("header-duplicate")
        headers[name] = value_bytes.decode("ascii")
    length_text = headers.get("content-length")
    if length_text is None or not re.fullmatch(r"[1-9][0-9]*", length_text):
        raise WireError("content-length")
    length = int(length_text, 10)
    if length > MAX_REQUEST_BODY:
        raise WireError("content-length")
    return target, headers, length


def _exact_sni(server_name: str | None, expected_host: str) -> bool:
    """Require a present, byte-for-byte exact ASCII SNI hostname."""
    return server_name is not None and server_name == expected_host


def _write_counter(path_text: str | None, value: int) -> None:
    if path_text:
        path = Path(path_text)
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_text(f"{value}\n", encoding="ascii")
        os.replace(temporary, path)


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
    amz_date: str = TEST_AMZ_DATE,
    date: str = TEST_DATE,
) -> str:
    canonical_headers = "".join(
        f"{name}:{headers[name]}\n" for name in signed_names
    )
    canonical_request = (
        f"POST\n{path}\n\n{canonical_headers}\n"
        f"{';'.join(signed_names)}\n{payload_hash}"
    )
    scope = f"{date}/{region}/bedrock/aws4_request"
    string_to_sign = (
        "AWS4-HMAC-SHA256\n"
        f"{amz_date}\n{scope}\n"
        f"{hashlib.sha256(canonical_request.encode('utf-8')).hexdigest()}"
    )
    key_date = _hmac(("AWS4" + TEST_SECRET_KEY).encode("utf-8"), date)
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


class MockServer(TCPServer):
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], args: argparse.Namespace):
        self.args = args
        self.accepted = 0
        self.observed = 0
        super().__init__(address, MockHandler)
        self.server_port = self.server_address[1]

    def mark_observed(self) -> None:
        self.observed += 1
        _write_counter(self.args.observed_file, self.observed)

    def mark_accepted(self) -> None:
        self.accepted += 1
        _write_counter(self.args.counter_file, self.accepted)


class MockHandler(BaseRequestHandler):
    server: MockServer

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

    def _send_close_notify(self) -> None:
        """Send TLS close_notify without waiting for the client's reciprocal alert."""
        try:
            self.connection.setblocking(False)
            self.connection.unwrap()
        except (ssl.SSLWantReadError, ssl.SSLWantWriteError, OSError):
            pass

    def _has_surplus_request_bytes(self, queued: bytes) -> bool:
        """Reject an immediately queued/pipelined suffix after Content-Length.

        ``queued`` covers bytes received with the declared body, and the bounded
        socket probe covers bytes immediately readable from TLS.  Together they
        reliably catch a single-write request-plus-suffix.  This
        cannot prove that a peer will never send bytes later: Content-Length ends
        the request, while this response-before-close protocol cannot wait for
        peer EOF without deadlocking a normal client.
        """
        if queued:
            return True
        previous_timeout = self.connection.gettimeout()
        try:
            self.connection.settimeout(0.1)
            return bool(self.connection.recv(1))
        except (TimeoutError, socket.timeout, ssl.SSLWantReadError):
            return False
        finally:
            self.connection.settimeout(previous_timeout)

    def _drop_without_close_notify(self) -> None:
        """Close the underlying TCP fd without emitting a TLS close_notify."""
        try:
            fd = self.connection.detach()
        except OSError:
            return
        os.close(fd)

    def _read_request(self) -> tuple[str, dict[str, str], bytes, bytes]:
        buffered = bytearray()
        marker_at = -1
        while marker_at < 0:
            if len(buffered) >= MAX_REQUEST_HEAD:
                raise WireError("head-too-large")
            chunk = self.connection.recv(min(16384, MAX_REQUEST_HEAD - len(buffered)))
            if not chunk:
                raise WireError("short-head")
            buffered.extend(chunk)
            marker_at = buffered.find(b"\r\n\r\n")
        head_end = marker_at + 4
        target, headers, length = parse_request_head(bytes(buffered[:head_end]))
        remainder = bytearray(buffered[head_end:])
        while len(remainder) < length:
            chunk = self.connection.recv(min(16384, length - len(remainder)))
            if not chunk:
                raise WireError("short-body")
            remainder.extend(chunk)
        return target, headers, bytes(remainder[:length]), bytes(remainder[length:])

    def handle(self) -> None:
        self.connection = self.request
        self.server.mark_observed()
        args = self.server.args
        self.connection.settimeout(5.0)
        try:
            path, headers, body, queued = self._read_request()
        except (OSError, WireError) as error:
            reason = error.args[0] if isinstance(error, WireError) else "request-io"
            self._reject(str(reason))
            return
        if path != args.expected_path:
            self._reject("path", len(body))
            return
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
        length = len(body)
        if self._has_surplus_request_bytes(queued):
            self._reject("surplus-request-bytes", length)
            return
        if headers["host"] != args.expected_host or headers["content-type"] != "application/json":
            self._reject("authority-media", length)
            return
        payload_hash = hashlib.sha256(body).hexdigest()
        if not hmac.compare_digest(headers["x-amz-content-sha256"], payload_hash):
            self._reject("payload-hash", length)
            return
        amz_date = headers["x-amz-date"]
        if args.dynamic_timestamp:
            try:
                signed_at = datetime.strptime(amz_date, "%Y%m%dT%H%M%SZ").replace(tzinfo=timezone.utc)
                timestamp_ok = abs((datetime.now(timezone.utc) - signed_at).total_seconds()) <= 300
            except ValueError:
                timestamp_ok = False
        else:
            timestamp_ok = amz_date == TEST_AMZ_DATE
        if not timestamp_ok:
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
        signing_date = amz_date[:8]
        expected_scope = f"{signing_date}/{args.region}/bedrock/aws4_request"
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
        wanted = expected_signature(path, headers, signed_names, payload_hash, args.region,
                                    amz_date, signing_date)
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
        if args.case == "unclean-eof":
            self._drop_without_close_notify()
        else:
            self._send_close_notify()

    def _respond(self, case: str) -> None:
        if case in ("nonstream-success", "unclean-eof"):
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

    valid_head = (
        b"POST /model/model/converse HTTP/1.1\r\n"
        b"Host: bedrock-runtime.us-east-1.amazonaws.com\r\n"
        b"Content-Type: application/json\r\n"
        b"Content-Length: 4\r\n"
        b"Connection: close\r\n\r\n"
    )
    parsed_path, parsed_headers, parsed_length = parse_request_head(valid_head)
    assert parsed_path == "/model/model/converse"
    assert parsed_headers["host"] == headers["host"]
    assert parsed_length == 4

    def rejected(raw: bytes) -> bool:
        try:
            parse_request_head(raw)
        except WireError:
            return True
        return False

    assert rejected(valid_head.replace(b"Host: ", b"Host: first\r\nHost: "))
    assert rejected(valid_head.replace(b"Host: ", b"Host : "))
    assert rejected(valid_head.replace(b"Host: ", b"Host@: "))
    assert rejected(valid_head.replace(b"Host: ", b"Host:"))
    assert rejected(valid_head.replace(b"Host: ", b"Host:  "))
    assert rejected(valid_head.replace(b".com\r\n", b".com \r\n"))
    assert rejected(valid_head.replace(b"Content-Type:", b"\tContent-Type:"))
    assert rejected(valid_head.replace(b"Host:", b"Fold: one\r\n two\r\nHost:"))
    assert rejected(valid_head.replace(b" HTTP/1.1\r\n", b" HTTP/1.1\n"))
    assert rejected(valid_head.replace(b"POST ", b"GET "))
    assert rejected(valid_head.replace(b"POST /", b"POST  /"))
    assert rejected(valid_head.replace(b"/model/model/converse", b"https://example.invalid/"))
    assert rejected(valid_head.replace(b"HTTP/1.1", b"HTTP/1.0"))
    assert _exact_sni("bedrock-runtime.us-east-1.amazonaws.com", headers["host"])
    assert not _exact_sni(None, headers["host"])
    assert not _exact_sni("wrong.invalid", headers["host"])
    assert "unclean-eof" in CASES
    assert "unclean-eof" not in (
        "stream-success",
        "fragmented-stream-success",
        "bad-crc",
        "semantic-truncation",
        "complete-frame-semantic-truncation",
    )
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
    parser.add_argument("--dynamic-timestamp", action="store_true")
    parser.add_argument("--counter-file", help="atomically updated accepted-request count")
    parser.add_argument("--observed-file", help="atomically updated observed-request count")
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
    streaming = args.case not in (
        "nonstream-success",
        "wrong-media",
        "non-2xx",
        "malformed-framing",
        "unclean-eof",
    )
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

    def require_exact_sni(
        _tls_socket: ssl.SSLSocket,
        server_name: str | None,
        _initial_context: ssl.SSLContext,
    ) -> int | None:
        if _exact_sni(server_name, args.expected_host):
            return None
        print(f"case={args.case} rejected=sni", file=sys.stderr, flush=True)
        return ssl.ALERT_DESCRIPTION_UNRECOGNIZED_NAME

    context.set_servername_callback(require_exact_sni)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    _write_counter(args.counter_file, 0)
    _write_counter(args.observed_file, 0)
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
