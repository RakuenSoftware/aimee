"""Versioned native memory transport; memory selection and policy live remotely.

The local spool holds only opaque completion coordinates and producer-reported
transport receipts. It never stores prompts, source text, or retries an HTTP send.
"""
from __future__ import annotations

import asyncio
import hashlib
import fcntl
import json
import os
import re
from pathlib import Path
import secrets
import stat
import time
import uuid

import aiohttp

CONTRACT = "aims-conversation/v1"
MAX_RESPONSE = 1024 * 1024
_DIRECTORY_LOCKS = {}


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


def serialized(method):
    async def call(self, *args, **kwargs):
        async with self._serialization:
            return await method(self, *args, **kwargs)
    return call


class AdmissionExpired(RuntimeError):
    pass


class AdmittedPayload(aiohttp.payload.BytesPayload):
    """Check immediately before body bytes, after DNS/connect/TLS/header work."""
    def __init__(self, value, deadline, started):
        super().__init__(value, content_type="application/json")
        self.deadline, self.started = deadline, started

    def _admit(self):
        if time.monotonic() >= self.deadline:
            raise AdmissionExpired("native source admission expired before HTTP body")
        self.started[0] = True

    async def write(self, writer):
        self._admit()
        await super().write(writer)

    async def write_with_length(self, writer, content_length):
        if content_length is not None and content_length != len(self._value):
            raise RuntimeError("HTTP transport changed the admitted payload length")
        self._admit()
        await super().write_with_length(writer, content_length)


class NativeConversation:
    def __init__(self, action, project, state_directory):
        self.action, self.project = action, project
        self.state_directory = Path(state_directory)
        self.context_token = None
        self.record_owner = None
        self.caller_binding = None
        self.text = ""
        self.last_receipt = None
        self.last_recovery = None
        self.latest_source = None
        self.deliveries = []
        self._ensure_directory()

    def _ensure_directory(self):
        if not self.state_directory.is_absolute():
            raise ValueError("native transport state directory must be absolute")
        self.state_directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        info = self.state_directory.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
            raise ValueError("native transport state directory must be owner-only")
        key = str(self.state_directory.resolve())
        if key not in _DIRECTORY_LOCKS:
            fd = os.open(self.state_directory / ".owner.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
            try:
                info = os.fstat(fd)
                if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
                    raise ValueError("unsafe native transport owner lock")
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BaseException:
                os.close(fd)
                raise RuntimeError("another native conversation sender owns this state directory") from None
            _DIRECTORY_LOCKS[key] = (fd, asyncio.Lock())
        self._serialization = _DIRECTORY_LOCKS[key][1]

    async def _call(self, operation, **fields):
        result = await self.action("memory.conversation", {"contract": CONTRACT,
            "operation": operation, "project": self.project, **fields})
        if not isinstance(result, dict) or result.get("status") != "ok" or result.get("contract") != CONTRACT:
            raise RuntimeError("native conversation refused or returned an incompatible contract")
        return result

    @serialized
    async def probe(self):
        # An unresolved durable send intentionally makes writer preflight fail.
        # Authenticate and complete the retained descriptor before readiness.
        await self._recover()
        result = await self._call("probe")
        if (result.get("context") != "native_exact_sources"
                or result.get("dispatch") != "durable_host_fence"
                or result.get("outcome") != "authenticated_producer_report"
                or result.get("response_capture") != "native_input_lineage"
                or result.get("provider_operations") != ["model_template", "model_tokenize", "model", "discord"]):
            raise RuntimeError("native conversation capabilities differ")
        return result

    @staticmethod
    def episode(turn):
        return {"guild_id": str(turn.guild_id), "channel_id": str(turn.channel_id),
            "message_id": str(turn.message_id), "speaker_id": str(turn.user_id),
            "speaker_name": turn.author_name, "speaker_kind": "bot" if turn.is_bot else "human",
            "text": turn.text, "reply_to_message_id": str(turn.reply_to_message_id) if getattr(turn, "reply_to_message_id", None) else None}
    @serialized
    async def capture(self, turn):
        await self._recover()
        # These are admitted Discord event coordinates, not native grants or
        # ontological interpretations. The remote owner decides their policy.
        episode = self.episode(turn)
        result = await self._call("capture", episode=episode)
        record = result.get("id")
        if (not isinstance(record, str) or not re.fullmatch(r"[1-9][0-9]{0,18}", record)
                or int(record) > 2**63 - 1):
            raise RuntimeError("native conversation capture omitted its exact record receipt")
        reference = result.get("reference")
        if not isinstance(reference, dict) or set(reference) != {"source", "version"}:
            raise RuntimeError("native capture omitted its exact source reference")
        self.latest_source = reference
        self.context_token = None
        self.deliveries = []
        return record

    @serialized
    async def capture_response(self, turn):
        if not self.context_token or not self.deliveries:
            raise RuntimeError("delivered response lacks retained native lineage; delivery is not retried")
        result = await self._call("capture_response", episode=self.episode(turn),
            context_token=self.context_token, deliveries=self.deliveries)
        self.context_token = None
        self.deliveries = []
        return result

    @serialized
    async def context(self, query, budget=12000, *, region=None):
        fields = {"query": query, "max_context_bytes": budget}
        if region is not None:
            fields["region"] = region
        if self.latest_source is not None:
            fields["latest_source"] = self.latest_source
        result = await self._call("context", **fields)
        token, text = result.get("context_token"), result.get("text")
        if (not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{32}", token)
                or not isinstance(text, str) or len(text.encode()) > budget):
            raise RuntimeError("native conversation capture is malformed")
        owner = result.get("record_owner")
        if not isinstance(owner, str) or str(uuid.UUID(owner)) != owner or uuid.UUID(owner).int == 0:
            raise RuntimeError("native context omitted its exact database owner")
        caller = result.get("caller_binding")
        if not isinstance(caller, list) or len(caller) != 32 or not all(type(x) is int and 0 <= x <= 255 for x in caller):
            raise RuntimeError("native context omitted its exact caller constraint")
        self.context_token, self.text, self.record_owner, self.caller_binding = token, text, owner, caller
        self.deliveries = []
        return text

    def _path(self, request_id):
        if str(uuid.UUID(request_id)) != request_id:
            raise ValueError("native dispatch UUID is not canonical")
        return self.state_directory / (request_id + ".json")

    def _save(self, value):
        path = self._path(value["completion"]["request_id"])
        temporary = path.with_name(path.name + "." + secrets.token_hex(8) + ".tmp")
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            with os.fdopen(fd, "wb") as stream:
                stream.write(encode(value))
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
            self._sync_directory()
        finally:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass

    def _sync_directory(self):
        fd = os.open(self.state_directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)

    async def _complete(self, value):
        finished = await self._call("dispatch_finish", completion=value["completion"])
        if (finished.get("send_guard") == "released_administratively_unknown"
                and finished.get("dispatch_state") == "administrative_unknown"
                and finished.get("external_outcome") == "unknown" and finished.get("receipt") is None):
            # Native maintenance records UNKNOWN, never an ACK of delivery. Keep
            # the exact local transport report as a quarantined recovery hint.
            value = {**value, "administrative_unknown": True, "external_outcome":"unknown"}
            self._save(value)
            path = self._path(value["completion"]["request_id"])
            os.replace(path, path.with_suffix(".quarantine"))
            self._sync_directory()
            self.last_receipt = None
            self.last_recovery = "administrative_unknown"
            return finished
        if finished.get("send_guard") == "absent" and finished.get("dispatch_state") == "not_acquired":
            self._path(value["completion"]["request_id"]).unlink()
            self._sync_directory()
            self.last_receipt = None
            return finished
        if finished.get("send_guard") != "released" or finished.get("dispatch_state") != "finished":
            raise RuntimeError("native exact dispatch fence was not completed")
        response = await self._call("dispatch_complete", completion=value["completion"], outcome=value["outcome"])
        released = response.get("send_guard") == "released" and isinstance(response.get("receipt"), dict)
        absent = (response.get("send_guard") == "absent" and response.get("dispatch_state") == "not_acquired"
                  and response.get("receipt") is None)
        if not released and not absent:
            raise RuntimeError("native dispatch completion was not retained")
        self._path(value["completion"]["request_id"]).unlink()
        self._sync_directory()
        self.last_receipt = response.get("receipt")
        return response

    @serialized
    async def recover(self):
        await self._recover()

    async def _recover(self):
        # A prior process may have lost the acquire or HTTP acknowledgement.
        # Complete conservatively as unknown, never repeat network delivery.
        for path in sorted(self.state_directory.glob("*.json")):
            info = path.lstat()
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid()
                    or info.st_mode & 0o077 or info.st_size > 65536):
                raise RuntimeError("unsafe native transport completion spool")
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
            with os.fdopen(fd, "rb") as stream:
                value = json.load(stream)
            if value.get("completion", {}).get("project") != self.project:
                # One owned directory may contain independent channel projects.
                continue
            if self._path(value["completion"]["request_id"]) != path:
                raise RuntimeError("native completion spool filename differs")
            if not isinstance(value.get("outcome"), dict):
                value["outcome"] = {"state": "unknown"}
                self._save(value)
            await self._complete(value)

    @serialized
    async def transmit(self, session, url, body, *, purpose, headers=None, transport=None, timeout=120):
        await self._recover()
        if any(self.state_directory.glob("*.json")):
            raise RuntimeError("another project has an unresolved native delivery outcome")
        if not self.context_token:
            raise RuntimeError("native context must be captured before source-bearing delivery")
        payload = encode(body)
        completion = {"contract": CONTRACT, "project": self.project, "record_owner": self.record_owner, "caller_binding": self.caller_binding,
            "request_id": str(uuid.uuid4()), "purpose": purpose,
            "payload_digest": list(hashlib.sha256(payload).digest()), "dispatch_nonce": secrets.token_hex(16)}
        pending = {"completion": completion, "outcome": None}
        self._save(pending)  # durable opaque completion before any acquisition
        started = [False]
        try:
            before = time.monotonic()
            prepared = await self._call("dispatch_prepare", context_token=self.context_token,
                request_id=completion["request_id"], purpose=purpose, payload=payload.decode(),
                dispatch_nonce=completion["dispatch_nonce"])
            lease = prepared.get("lease_ms")
            returned = prepared.get("completion")
            exact_digest = (isinstance(returned, dict) and isinstance(returned.get("payload_digest"), list)
                and len(returned["payload_digest"]) == 32
                and all(type(x) is int and 0 <= x <= 255 for x in returned["payload_digest"]))
            if (prepared.get("send_guard") != "acquired" or returned != completion or not exact_digest
                    or type(lease) is not int or lease <= 0 or lease > 1000):
                raise RuntimeError("native dispatch did not return exact admission")
            data = AdmittedPayload(payload, before + lease / 1000, started)
            async with session.post(url, data=data, headers=headers, **(transport or {}),
                    allow_redirects=False, timeout=aiohttp.ClientTimeout(total=timeout, connect=5)) as response:
                chunks, size = [], 0
                async for chunk in response.content.iter_chunked(65536):
                    size += len(chunk)
                    if size > MAX_RESPONSE:
                        raise RuntimeError("native provider response exceeds transport bound")
                    chunks.append(chunk)
                raw = b"".join(chunks)
                external_ids = []
                try:
                    value = json.loads(raw)
                    if (purpose == "discord" and isinstance(value, dict) and isinstance(value.get("id"), str)
                            and re.fullmatch(r"[1-9][0-9]{16,19}", value["id"])):
                        external_ids = [value["id"]]
                except (ValueError, TypeError):
                    value = None
                pending["outcome"] = {"state": "received", "status_code": response.status,
                    "response_digest": list(hashlib.sha256(raw).digest()), "external_ids": external_ids}
                self._save(pending)
                await self._complete(pending)
                if purpose == "discord" and self.last_receipt is not None:
                    if len(self.deliveries) >= 8:
                        raise RuntimeError("delivery lineage exceeds bounded chunk count")
                    self.deliveries.append({"completion":completion,"outcome":pending["outcome"],
                        "payload":payload.decode(),"intent_reference":prepared.get("intent_reference"),"receipt":self.last_receipt})
                if response.status not in ((200, 201) if purpose == "discord" else (200,)) or not isinstance(value, dict):
                    raise RuntimeError("native provider refused or returned malformed JSON")
                return value
        except BaseException:
            if pending["outcome"] is None:
                pending["outcome"] = {"state": "unknown" if started[0] else "not_sent"}
                self._save(pending)
            if self._path(completion["request_id"]).exists():
                try:
                    await asyncio.shield(self._complete(pending))
                except Exception:
                    # Retained spool is intentionally fatal to startup/recovery
                    # until exact native completion can be authenticated.
                    pass
            raise
