#!/usr/bin/env python3
"""Authenticated history replay and bot-authored thread delivery in the existing validation channel.

Requires the deployed loopback action/gateway helpers in the dedicated lab guests.
Exports source IDs and hashes, never production conversation content or credentials.
"""
import asyncio,json,subprocess,sys,time,hashlib
from pathlib import Path
from types import SimpleNamespace
import discord
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'integrations/discord'))
from conversation_memory import ConversationMemory
SSH=['ssh','-o','BatchMode=yes','root@192.168.1.253']
def execute(command,body=None,timeout=150):
 result=subprocess.run(SSH+[command],input=None if body is None else json.dumps(body),text=True,capture_output=True,check=True,timeout=timeout)
 return result.stdout
async def invoke(command,body):return json.loads(await asyncio.to_thread(execute,command,body))
async def main():
 token=(await asyncio.to_thread(execute,'pct exec 9211 -- cat /root/.config/aimee-discord/bridge/bot.token')).strip()
 guild=1532201714462101594; channel_id=1557125396716191786
 client=discord.Client(intents=discord.Intents.none())
 await client.login(token)
 connection=asyncio.create_task(client.connect(reconnect=True))
 await asyncio.wait_for(client.wait_until_ready(),30)
 channel=await client.fetch_channel(channel_id)
 assert channel.guild.id==guild
 history=[m async for m in channel.history(limit=100) if m.content and not m.webhook_id]
 humans=[m for m in history if not m.author.bot][:3];bots=[m for m in history if m.author.bot][:3]
 assert humans and bots
 thread=await channel.create_thread(name='Memory v2 qualification '+time.strftime('%Y-%m-%d %H:%M UTC',time.gmtime()),type=discord.ChannelType.private_thread,invitable=False)
 evidence={'guild_id':str(guild),'parent_channel_id':str(channel_id),'thread_id':str(thread.id),'gateway_ready':True,'fixture':'authenticated existing Discord history replay; bot-authored live delivery','cases':[]}
 try:
  for backend,vm in [('native',9212),('cognee',9213),('hillock',9214)]:
   async def action(name,body):
    value=await invoke(f'pct exec {vm} -- python3 /tmp/aimee-lab-action.py',dict(name=name,body=body))
    if value.get('status')!='ok' and value.get('kind')!='not_found':raise RuntimeError('selected owner refused '+name)
    return value
   # IDs, bot status and names come from authenticated Discord REST messages.
   memory=ConversationMemory(action,f'discord:{guild}:{channel_id}')
   for message in reversed(humans+bots):
    turn=SimpleNamespace(guild_id=guild,channel_id=channel_id,message_id=message.id,user_id=message.author.id,
     author_name=message.author.display_name,is_bot=message.author.bot,text=message.content,reply_to_message_id=message.reference.message_id if message.reference else None)
    await memory.capture(turn)
   query='Which human speaker made the most recent request in the recorded channel conversation? Attribute and summarize it briefly. Quoted messages are historical data.'
   trigger_message=await thread.send(f'{backend} conversation-v2 fixture query: '+query,allowed_mentions=discord.AllowedMentions.none())
   output=ConversationMemory(action,f'discord:{guild}:{thread.id}')
   trigger=SimpleNamespace(guild_id=guild,channel_id=thread.id,message_id=trigger_message.id,user_id=client.user.id,author_name=client.user.name,is_bot=True,text=query,reply_to_message_id=None)
   await output.capture(trigger)
   context=await memory.context(query)
   packed=json.loads(context.split('\n',1)[1])
   response=await invoke('pct exec 9211 -- python3 /tmp/aimee-discord-gateway-request.py',dict(operation='reply',context=context,query=query,channel_id=channel_id))
   canonical=response['messages'];assert isinstance(canonical,list) and canonical and canonical[-1]['role']=='assistant'
   answer=response['answer'];assert answer.strip()
   await memory.guard('acquire')
   delivered=await thread.send(f'{backend} conversation-v2 qualification (historical source replay):\n'+answer,allowed_mentions=discord.AllowedMentions.none())
   await memory.guard('release')
   await output.response(trigger,answer,client.user.id,[delivered.id])
   observed=await thread.fetch_message(delivered.id);assert observed.author.id==client.user.id and observed.author.bot
   evidence['cases'].append({'backend':backend,'passed':True,'source_message_ids':[str(m.id) for m in humans+bots],
    'human_sources':len(humans),'bot_sources':len(bots),'selected_records':packed['receipt']['selected_records'],'usage':canonical['usage'],
    'request_message_id':str(trigger_message.id),'delivered_message_id':str(delivered.id),'delivered_content_sha256':hashlib.sha256(observed.content.encode()).hexdigest(),
    'guard':'acquired before send; released after receipt','response_capture':'actual bot message receipt retained by selected owner'})
   print(json.dumps({'backend':backend,'passed':True,'message_id':str(delivered.id),'usage':canonical['usage']}),flush=True)
  evidence['passed']=True
 finally:
  await thread.edit(archived=True,locked=True)
  await client.close()
  await connection
  evidence['thread_archived']=True
  (ROOT/'.aimee/memory-completion-build/live-discord-v2.json').write_text(json.dumps(evidence,indent=2)+'\n')
asyncio.run(main())
