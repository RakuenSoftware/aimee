#!/usr/bin/env python3
"""Run in the existing Discord guest; use its private config inside the bridge.

The reply probe supplies selected-owner context from the host qualification run.
No bearer or model credential crosses this JSON transport.
"""
import json,subprocess,sys

BRIDGE_CODE='''import asyncio,json,sys
from pathlib import Path
import aiohttp
sys.path.insert(0,"/app")
from bridge import Config,ModelClient
from model_ir import ModelTurn,display_text
from dataclasses import replace
async def main():
 request=json.load(sys.stdin);c=Config.load(Path("/run/discord/config.json"))
 async with aiohttp.ClientSession() as session:
  if request["operation"]!="reply":raise ValueError("only canonical reply qualification is supported")
  model=ModelClient(replace(c,memory_profile="conversation-v2"),session)
  async def frozen_context(*args,**kwargs):return request["context"]
  model.memory_context=frozen_context
  turn=await model.reply([],request["query"],request["channel_id"])
  if not isinstance(turn,ModelTurn):raise RuntimeError("no canonical model turn")
  value={"answer":display_text(turn),"ir":turn.response}
  print(json.dumps(value))
asyncio.run(main())'''
def main():
 request=json.load(sys.stdin)
 if request.get('operation')!='reply':raise ValueError('only canonical reply qualification is supported')
 result=subprocess.run(['docker','exec','-i','aimee-discord-knowledge-discord-bridge-1','python','-c',BRIDGE_CODE],input=json.dumps(request),text=True,capture_output=True,timeout=140)
 if result.returncode:
  raise RuntimeError('bridge gateway request failed')
 sys.stdout.write(result.stdout)

if __name__=='__main__':main()
