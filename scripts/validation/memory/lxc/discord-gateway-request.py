#!/usr/bin/env python3
"""Run in the existing Discord guest; use its private config inside the bridge.

The reply probe supplies selected-owner context from the host qualification run.
No bearer or model credential crosses this JSON transport.
"""
import json,subprocess,sys
request=json.load(sys.stdin)
code='''import asyncio,json,sys
from pathlib import Path
import aiohttp
sys.path.insert(0,"/app")
from bridge import Config,ModelClient
from dataclasses import replace
async def main():
 request=json.load(sys.stdin);c=Config.load(Path("/run/discord/config.json"))
 async with aiohttp.ClientSession() as session:
  if request["operation"]=="tokenizer":
   value=await ModelClient(c,session).tokenizer_request(request["route"],request.get("body"))
  elif request["operation"]=="reply":
   model=ModelClient(replace(c,memory_profile="conversation-v2"),session)
   async def frozen_context(*args,**kwargs):return request["context"]
   model.memory_context=frozen_context
   answer=await model.reply([],request["query"],request["channel_id"])
   value={"answer":answer,"budget":model.prompt_budget.receipt,"usage_verified_by_bridge":True}
  else:
   async with aiohttp.ClientSession(connector=aiohttp.UnixConnector(path=str(c.aimee_socket))) as gateway:
    async with gateway.post("http://localhost/v1/chat/completions",json=request["body"],timeout=aiohttp.ClientTimeout(total=120)) as response:value=await response.json()
  print(json.dumps(value))
asyncio.run(main())'''
result=subprocess.run(['docker','exec','-i','aimee-discord-knowledge-discord-bridge-1','python','-c',code],input=json.dumps(request),text=True,capture_output=True,timeout=140)
if result.returncode:
 print(result.stderr.splitlines()[-1] if result.stderr else 'bridge gateway request failed',file=sys.stderr)
 raise RuntimeError('bridge gateway request failed')
sys.stdout.write(result.stdout)
