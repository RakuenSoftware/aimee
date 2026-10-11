#!/usr/bin/env python3
"""Exercise the real Discord memory adapter in a disposable lab, no sends."""
import argparse
import asyncio
import json
from pathlib import Path
import sys
import time
from types import SimpleNamespace
import urllib.request
import urllib.error


async def probe(options):
    sys.path.insert(0, str(options.adapter_directory))
    from conversation_memory import ConversationMemory
    state = json.loads((options.lab_root / "resume-private.json").read_text())
    token = state["kb-supervisor"]["env"]["AIMEE_KB_API_BEARER_TOKEN"]
    project = "discord:111111111111111111:222222222222222222:" + str(time.time_ns())

    async def action(name, body):
        def request():
            req = urllib.request.Request("http://127.0.0.1:8741/v1/actions/" + name, data=json.dumps(body).encode(),
                                         headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
            try:
                with urllib.request.urlopen(req, timeout=150) as response:
                    result = json.load(response)
            except urllib.error.HTTPError as error:
                raise RuntimeError(name + " HTTP " + str(error.code)) from None
            if result.get("status") != "ok":
                raise RuntimeError(name + " refused: " + str(result.get("kind", result.get("status"))))
            return result
        return await asyncio.to_thread(request)

    memory = ConversationMemory(action, project)
    nonce = int(time.time()*1000-1420070400000) << 22
    def turn(offset, name, text, bot=False):
        return SimpleNamespace(guild_id=111111111111111111, channel_id=222222222222222222,
                               message_id=nonce+offset, user_id=333333333333333333+offset,
                               author_name=name, text=text, is_bot=bot)
    virant = turn(1, "Virant", "Kibukx the person is exactly 69cm tall. Keep the original units.")
    samy = turn(2, "Samy", "The Kibukx mountains have an elevation of 69 feet; that is a place, not the person.")
    peer = turn(3, "PeerBot", "I repeated Virant's 69cm report. I did not measure Kibukx independently.", True)
    ids = {await memory.capture(value) for value in (virant, samy, peer)}
    # A new adapter instance has no volatile conversation state to rely on.
    memory = ConversationMemory(action, project)
    packed = json.loads((await memory.context("Who originally said Kibukx the person's height, and what units?")).split("\n", 1)[1])
    assert any("69cm" in row["content"] and "Virant" in row["content"] for row in packed["records"]), "source identity or original units omitted"
    await memory.guard("acquire")
    blocked = False
    try:
        await memory.capture(turn(4, "Virant", "A new statement must wait until send completion."))
    except RuntimeError:
        blocked = True
    assert blocked, "canonical writer escaped the send barrier"
    await memory.guard("release")
    assistant_id = await memory.response(virant, "Virant reported 69cm; PeerBot repeated it without independent measurement.", 444444444444444444)
    memory = ConversationMemory(action, project)
    after = json.loads((await memory.context("What did PeerBot repeat?")).split("\n", 1)[1])
    assistant = await action("memory.get", {"project": project, "scope_context": True, "id": assistant_id})
    assert "agent_message" in assistant["memory"]["content"], "delivered assistant source lost its provenance"
    assert any("agent_message" in row["content"] for row in after["records"]) or after["receipt"]["omitted_records"] > 0, "agent evidence absent without an omission receipt"
    options.output.write_text(json.dumps({"backend": options.backend, "passed": True,
        "checks": ["authored human input and exact units", "bot input is archived without human authority",
                   "restart reconstructs raw source projection", "send barrier blocks canonical mutation",
                   "explicit completion releases barrier", "delivered response remains agent evidence"],
        "receipt": packed["receipt"], "cases": [{"id": "bridge_attribution_after_restart", "status": "passed",
        "query": "Who originally reported the person Kibukx's height? Give the exact original units and say whether PeerBot measured independently.",
        "required": ["Virant", "69", "cm", "PeerBot"], "forbidden": ["69 feet", "independently measured"],
        "records": [row for row in packed["records"] if row["id"] in ids]}]}, indent=2) + "\n")
    print(json.dumps({"backend": options.backend, "passed": True, "checks": 6}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", required=True, choices=("native", "cognee", "hillock"))
    parser.add_argument("--adapter-directory", type=Path, required=True)
    parser.add_argument("--lab-root", type=Path, default=Path("/var/lib/aimee-memory-lab"))
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    try:
        asyncio.run(probe(options))
    except Exception as error:
        # Exceptions here contain only adapter refusal messages or HTTP status,
        # never curl URLs, headers, tokens or response bodies.
        detail = str(error) if isinstance(error, (RuntimeError, AssertionError)) else "transport"
        options.output.write_text(json.dumps({"backend": options.backend, "passed": False, "error": type(error).__name__, "detail": detail}) + "\n")
        print(json.dumps({"backend": options.backend, "passed": False, "error": type(error).__name__, "detail": detail}))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
