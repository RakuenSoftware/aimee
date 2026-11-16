"""Discord's local binding to the shared Aimee thinclient.

The C owner resolves enrolled client identity, transports HTTP, and collects
canonical chat events. Python owns only channel/session control metadata.
"""
from __future__ import annotations

import asyncio
import ctypes as C
import json
import threading
from contextlib import contextmanager


class _Header(C.Structure):
    _fields_ = [("name", C.c_char_p), ("value", C.c_char_p)]


class _Options(C.Structure):
    _fields_ = [("endpoint", C.c_char_p), ("method", C.c_char_p),
                ("path", C.c_char_p), ("bearer", C.c_void_p),
                ("headers", C.POINTER(_Header)), ("header_count", C.c_size_t),
                ("body", C.c_void_p), ("body_length", C.c_size_t),
                ("connect_timeout_ms", C.c_int), ("io_timeout_ms", C.c_int),
                ("tls_home", C.c_void_p)]


class _Profile(C.Structure):
    _fields_ = [("endpoint", C.c_void_p), ("bearer", C.c_void_p), ("tls_home", C.c_void_p)]


class ThinClient:
    def __init__(self, config):
        self.library = config.aimee_library
        self.profile = config.aimee_profile
        self.socket = config.aimee_socket
        self.timeout = 120
        self._lib = None
        self._sessions = {}
        self._locks = {}

    def _load(self):
        if self._lib is not None:
            return self._lib
        lib = C.CDLL(self.library)
        for name, args, result in (
            ("cli_http_profile_resolve", [C.c_char_p, C.c_char_p, C.POINTER(_Profile)], C.c_int),
            ("cli_http_profile_free", [C.POINTER(_Profile)], None),
            ("cli_http_stream_new", [], C.c_void_p),
            ("cli_http_stream_open", [C.c_void_p, C.POINTER(_Options)], C.c_int),
            ("cli_http_stream_status", [C.c_void_p], C.c_int),
            ("cli_http_stream_read", [C.c_void_p, C.c_void_p, C.c_size_t], C.c_long),
            ("cli_http_stream_cancel", [C.c_void_p], None),
            ("cli_http_stream_free", [C.c_void_p], None),
            ("cli_chat_request_stream_json", [C.c_void_p, C.POINTER(_Options),
                                              C.c_char_p, C.POINTER(C.c_void_p)], C.c_int),
            ("cli_chat_string_free", [C.c_void_p], None),
        ):
            fn = getattr(lib, name)
            fn.argtypes, fn.restype = args, result
        self._lib = lib
        return lib

    @staticmethod
    def _json(value):
        data = json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                          allow_nan=False).encode("utf-8")
        if len(data) > 1024 * 1024:
            raise ValueError("thin-client request exceeds input limit")
        return data

    async def request(self, method, path, body=None, *, headers=None, timeout=30):
        """Ordinary Aimee API JSON over the same enrolled C transport."""
        def run(exchange, connection):
            return exchange(connection.options(method, path, timeout, headers),
                            self._json(body) if body is not None else b"")
        return await self._execute(run, timeout)

    async def chat(self, message: dict, channel: str, agent: str) -> list[dict]:
        payload = self._json(message)
        if not isinstance(channel, str) or not channel or not isinstance(agent, str) or not agent:
            raise ValueError("chat requires a channel and registered alias")
        lock = self._locks.setdefault(channel, asyncio.Lock())
        async with lock:
            def run(exchange, connection):
                def call(method, path, value=None):
                    return exchange(connection.options(method, path, self.timeout),
                                    self._json(value) if value is not None else b"")
                binding = self._sessions.get(channel)
                if binding is None:
                    created = call("POST", "/v1/sessions/create", {"client_type": "discord"})
                    sid = created.get("session_id")
                    if not isinstance(sid, str) or not sid or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for c in sid):
                        raise RuntimeError("Aimee returned no session identity")
                    row = call("POST", "/v1/sessions/get", {"session_id": sid}).get("session")
                    if (not isinstance(row, dict) or row.get("id") != sid
                            or not isinstance(row.get("principal"), str) or not row["principal"]):
                        raise RuntimeError("Aimee returned no session owner")
                    binding = (sid, row["principal"], agent)
                    self._sessions[channel] = binding
                sid, owner, selected = binding
                if selected != agent:
                    raise RuntimeError("Discord session alias changed")
                row = call("POST", "/v1/sessions/get", {"session_id": sid}).get("session")
                if not isinstance(row, dict) or row.get("id") != sid or row.get("principal") != owner:
                    raise RuntimeError("Discord session owner changed")
                path = "/v1/sessions/" + sid + "/primary"
                primary = call("GET", path).get("agent")
                if primary == "":
                    if call("POST", path, {"agent": agent}).get("agent") != agent:
                        raise RuntimeError("Aimee refused session alias")
                elif primary != agent:
                    raise RuntimeError("Discord session alias changed")
                request = self._json({"message": json.loads(payload), "aimee_session_id": sid,
                                      "client_type": "discord"})
                result = exchange(connection.options("POST", "/v1/chat/stream", self.timeout),
                                  request, canonical=True)
                if not isinstance(result.get("messages"), list):
                    raise RuntimeError("thin client returned no canonical messages")
                return result["messages"]

            return await self._execute(run, self.timeout)

    async def _execute(self, operation, timeout):
        lib = self._load()
        guard = threading.Lock()
        active = [None]
        cancelled = threading.Event()

        def cancel():
            cancelled.set()
            with guard:
                if active[0]:
                    lib.cli_http_stream_cancel(active[0])

        def exchange(options, body, canonical=False):
            if cancelled.is_set():
                raise RuntimeError("thin-client exchange cancelled")
            handle = lib.cli_http_stream_new()
            if not handle:
                raise RuntimeError("thin-client allocation failed")
            with guard:
                active[0] = handle
                if cancelled.is_set():
                    lib.cli_http_stream_cancel(handle)
            try:
                if canonical:
                    output = C.c_void_p()
                    try:
                        if lib.cli_chat_request_stream_json(handle, C.byref(options), body,
                                                           C.byref(output)) or not output.value:
                            raise RuntimeError("Aimee chat failed")
                        raw = C.string_at(output.value)
                    finally:
                        lib.cli_chat_string_free(output)
                else:
                    options.body, options.body_length = C.cast(C.c_char_p(body), C.c_void_p), len(body)
                    if lib.cli_http_stream_open(handle, C.byref(options)):
                        raise RuntimeError("Aimee session request failed")
                    chunks, size = [], 0
                    buffer = C.create_string_buffer(65536)
                    while True:
                        n = lib.cli_http_stream_read(handle, buffer, len(buffer))
                        if n < 0:
                            raise RuntimeError("Aimee session transport failed")
                        if not n:
                            break
                        size += n
                        if size > 1024 * 1024:
                            raise RuntimeError("thin-client control response exceeds limit")
                        chunks.append(buffer.raw[:n])
                    if not 200 <= lib.cli_http_stream_status(handle) < 300:
                        raise RuntimeError("Aimee session request refused")
                    raw = b"".join(chunks)
                value = json.loads(raw)
                if not isinstance(value, dict) or value.get("status") == "error":
                    raise RuntimeError("Aimee request refused")
                return value
            except (ValueError, UnicodeError):
                raise RuntimeError("thin client returned invalid JSON") from None
            finally:
                with guard:
                    active[0] = None
                lib.cli_http_stream_free(handle)

        def run():
            with self._connection(lib) as connection:
                return operation(exchange, connection)

        worker = asyncio.create_task(asyncio.to_thread(run))
        try:
            async with asyncio.timeout(timeout):
                await asyncio.wait({worker})
                return worker.result()
        finally:
            if not worker.done():
                cancel()
            # Join and consume even a concurrently completed exception. Free only
            # after the C call unwinds; no abandoned IO thread or shield callback.
            await asyncio.gather(worker, return_exceptions=True)


    @contextmanager
    def _connection(self, lib):
        profile = _Profile()
        name = self.profile.encode() if self.profile else None
        if lib.cli_http_profile_resolve(None, name, C.byref(profile)):
            raise RuntimeError("Aimee client profile unavailable")
        endpoint = ("unix:" + str(self.socket)).encode() if self.socket else C.cast(profile.endpoint, C.c_char_p)
        if not endpoint:
            lib.cli_http_profile_free(C.byref(profile))
            raise RuntimeError("Aimee client endpoint unavailable")

        class Connection:
            @staticmethod
            def options(method, path, timeout, headers=None):
                fields = [_Header(name.encode(), value.encode()) for name, value in (headers or {}).items()]
                metadata = (_Header * len(fields))(*fields)
                return _Options(headers=metadata, header_count=len(fields), endpoint=endpoint, method=method.encode(), path=path.encode(),
                                bearer=profile.bearer, connect_timeout_ms=1000,
                                io_timeout_ms=max(1, int(timeout * 1000)), tls_home=profile.tls_home)

        try:
            yield Connection()
        finally:
            lib.cli_http_profile_free(C.byref(profile))
