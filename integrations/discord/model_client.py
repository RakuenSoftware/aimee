"""Canonical model RPC through the installed Aimee thin client."""
from __future__ import annotations

import asyncio
import json
import os


class ThinClient:
    def __init__(self, config):
        self.executable = config.aimee_cli
        self.profile = config.aimee_profile
        self.socket = config.aimee_socket
        self.timeout = 120

    async def generate(self, request: dict, agent: str) -> dict:
        payload = json.dumps({"ir": request, "agent": agent}, ensure_ascii=False,
                             separators=(",", ":"), allow_nan=False).encode("utf-8")
        if len(payload) > 1024 * 1024:
            raise ValueError("canonical model request exceeds thin-client input limit")
        argv = [self.executable, "--json"]
        if self.profile:
            argv.append("--profile=" + self.profile)
        argv.extend(("model", "generate"))
        env = os.environ.copy()
        if self.socket:
            env["AIMEE_API_ENDPOINT"] = "unix:" + str(self.socket)
        proc = await asyncio.create_subprocess_exec(
            *argv, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL, env=env)

        async def read_output():
            chunks, size = [], 0
            while chunk := await proc.stdout.read(65536):
                size += len(chunk)
                if size > 8 * 1024 * 1024:
                    raise RuntimeError("thin-client response exceeds output limit")
                chunks.append(chunk)
            return b"".join(chunks)

        async def exchange():
            reader = asyncio.create_task(read_output())
            try:
                proc.stdin.write(payload)
                await proc.stdin.drain()
                proc.stdin.close()
                output = await reader
                await proc.wait()
                return output
            finally:
                if not reader.done():
                    reader.cancel()
                await asyncio.gather(reader, return_exceptions=True)

        try:
            async with asyncio.timeout(self.timeout):
                output = await exchange()
            if proc.returncode:
                raise RuntimeError("Aimee model generation failed")
            try:
                value = json.loads(output)
            except (ValueError, UnicodeError):
                raise RuntimeError("thin client returned invalid JSON") from None
            if (not isinstance(value, dict) or value.get("status") == "error"
                    or not isinstance(value.get("ir"), dict)):
                raise RuntimeError("thin client returned no canonical response")
            return value["ir"]
        finally:
            if proc.returncode is None:
                proc.kill()
                await proc.wait()
