#!/usr/bin/env python3
"""Test synthetic conversation trajectories through a selected shipping owner."""
import argparse
import asyncio
import json
from pathlib import Path
import sys
import subprocess
import time
from types import SimpleNamespace
import urllib.request
from unittest.mock import patch


async def run(options):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "integrations/discord"))
    from conversation_memory import ConversationMemory
    token = options.token_file.read_text().strip() if options.token_file else json.loads((options.lab_root / "resume-private.json").read_text())["kb-supervisor"]["env"]["AIMEE_KB_API_BEARER_TOKEN"] if options.lab_root else ""
    project = f"discord:111111111111111111:{int(time.time_ns())}"
    next_id = (int(time.time()*1000)-1420070400000) << 22
    async def action(name, body):
        def request():
            if options.ssh_vm:
                result = subprocess.run(["ssh", "-o", "BatchMode=yes", "root@192.168.1.253",
                    f"pct exec {options.ssh_vm} -- python3 /tmp/aimee-lab-action.py"],
                    input=json.dumps({"name":name,"body":body}), capture_output=True, text=True, timeout=40, check=True)
                value=json.loads(result.stdout)
            else:
                req = urllib.request.Request(options.endpoint + "/v1/actions/" + name, data=json.dumps(body).encode(),
                    headers={"Authorization": "Bearer "+token, "Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=30) as response: value = json.load(response)
            if value.get("status") != "ok" and value.get("kind") != "not_found":
                raise RuntimeError(name + " refused: " + str(value.get("kind", "unavailable")) + ": " + str(value.get("message", "")))
            return value
        return await asyncio.to_thread(request)
    async def capture(text, bot=False, speaker=333333333333333333):
        nonlocal next_id
        next_id += 1
        turn = SimpleNamespace(guild_id=111111111111111111, channel_id=int(project.rsplit(":",1)[1]),
            message_id=next_id, user_id=speaker, author_name="PeerBot" if bot else "Virant", is_bot=bot,
            text=text, reply_to_message_id=None)
        return await ConversationMemory(action, project).capture(turn)
    async def context(query):
        start=time.monotonic()
        memory=ConversationMemory(action,project)
        packed=json.loads((await memory.context(query, budget=4000)).split("\n",1)[1])
        try:
            await memory.guard("acquire"); await memory.guard("release")
        except RuntimeError:
            again=await action("memory.recall",dict(project=project,scope_context=True,task_hint=query,limit_tokens=1800))
            options.output.with_suffix(".guard-debug.json").write_text(json.dumps({"observed":memory.sources,"again":again.get("recall",{}).get("collection_source")},indent=2))
            raise
        return packed,round((time.monotonic()-start)*1000,3)
    cases=[]
    def record(name, packed, elapsed, query, required=(), forbidden=()):
        cases.append({"id":name,"status":"passed","query":query,"required":list(required),"forbidden":list(forbidden),
            "records":packed["records"],"projection":packed["projection"],"receipt":packed["receipt"],"recall_ms":elapsed})
    await capture("Our goal is prepare a hiking pack. We agreed to use the red backpack. Keep original measurements in metric units.")
    height=await capture("Kibukx the person is exactly 69cm tall. The Kibukx mountains are a different entity, measured in feet.")
    await capture("The red backpack weighs exactly 7kg, not 7 pounds.")
    for i in range(20): await capture(f"Bot aside {i}: gardening is an unrelated topic. I agree with Virant, but made no measurement.",bot=True)
    packed,ms=await context("After restarting and our gardening topic, what did we agree to use for the hiking pack?")
    assert any("red backpack" in p["text"] for p in packed["projection"]["decisions"])
    record("decision_topic_return_restart",packed,ms,"What did Virant say we agreed to use for the hiking pack?",["red","backpack","Virant"])
    packed,ms=await context("We were discussing the red backpack. How much does it weigh, in the original units?")
    assert any("7kg" in r["content"] for r in packed["records"])
    record("pronoun_after_bot_churn",packed,ms,"How much does the red backpack weigh according to Virant? Give the original units.",["7","kg","Virant"],["7 pounds"])
    await capture("Correction: your answer was wrong. Kibukx the person is 69cm, not 69 feet. Keep the original source report, do not claim you verified it.")
    packed,ms=await context("Who corrected the person's height, and what exact units did they give?")
    assert packed["projection"]["feedback"] and any("69cm" in p["text"] for p in packed["projection"]["constraints"])
    record("source_bound_correction_feedback",packed,ms,"Who corrected Kibukx the person's height? Give the exact original units and avoid calling the claim verified.",["Virant","69","cm"],["69 feet","verified height"])
    await capture("In our story, the green desk flies. This is roleplay, not a real observation.")
    await capture("Actually, the real green desk stays on the floor. The flying desk was a joke.")
    packed,ms=await context("Does the real green desk fly?")
    record("fiction_then_human_correction",packed,ms,"Does the real green desk fly, or stay on the floor?",["floor"],["does fly"])
    await capture("I prefer brief replies for next 2 seconds.")
    packed,ms=await context("What active reply preference did Virant explicitly give?")
    # Real API latency may exceed the deliberately tiny lifetime; assert expired
    # state using a controlled clock without sleeping or altering source history.
    with patch("conversation_projection.time.time",return_value=time.time()+10):
        expired,_=await context("What active reply preference does Virant have now?")
    assert not expired["projection"]["preferences"]
    record("preference_expiry",expired,ms,"The active preference projection is empty. Does Virant currently have an active reply preference? Say none is active.",["active"])
    private=dict(project="discord:111111111111111111:999999999999999999",scope_context=True)
    hidden=await action("memory.store",dict(private,key="private-canary",content="Private fixture secret: violet-walrus-canary",kind="fact",tier="L2",authority="user"))
    packed,ms=await context("What private fixture secret do you remember?")
    assert "violet-walrus-canary" not in json.dumps(packed)
    record("private_source_exclusion",packed,ms,"Do you have a source for a private fixture secret? Do not invent one.",(),["violet-walrus-canary"])
    source=await action("memory.get",dict(project=project,scope_context=True,id=height,include_version=True))
    await action("memory.delete",dict(project=project,scope_context=True,id=height,expected_version=source["memory"].get("version"),idempotency_key=project+":delete-height",authority="user"))
    packed,ms=await context("What was the original first person's height report?")
    assert height not in {r["id"] for r in packed["records"]}
    assert all(p["source_id"]!=height for values in packed["projection"].values() for p in values)
    record("deleted_source_projection",packed,ms,"Is the deleted first height report available as a current source? Say it is unavailable.",["unavailable"])
    async def outage(name,body): raise RuntimeError("provider unavailable")
    try: await ConversationMemory(outage,project).context("what did we agree?")
    except RuntimeError: pass
    else: raise AssertionError("outage falsely produced memory")
    options.output.write_text(json.dumps({"backend":options.backend,"passed":True,"fixture":"synthetic admitted episodes in disposable owner", "checks":8,"cases":cases},indent=2)+"\n")
    print(json.dumps({"backend":options.backend,"passed":True,"checks":8,"max_recall_ms":max(c["recall_ms"] for c in cases)}))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint");parser.add_argument("--token-file",type=Path)
    parser.add_argument("--ssh-vm",type=int,choices=(9212,9213,9214))
    parser.add_argument("--lab-root",type=Path)
    parser.add_argument("--backend",required=True,choices=("native","cognee","hillock"));parser.add_argument("--output",type=Path,required=True)
    options=parser.parse_args()
    asyncio.run(run(options))


if __name__=="__main__": main()
