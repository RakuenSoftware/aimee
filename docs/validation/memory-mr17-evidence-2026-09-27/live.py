import json,subprocess,sys,re,time
from pathlib import Path
head=sys.argv[1];assert re.fullmatch('[a-f0-9]{9}',head)
n=json.loads(Path('/opt/pr2990-evidence/mr07-managed-be46915db/names.json').read_text())
out=Path('/opt/pr2990-evidence/mr17-retry-'+head+'-v3');out.mkdir(exist_ok=False)
db=n['kb_project']+'-aimee-store-db-1';signal=None;proposal=None;session=None;before=None

def sql(q,store=None):
 p=subprocess.run(['docker','exec',store or db,'psql','-X','-qAt','-U','postgres','-d','aimee_store','-v','ON_ERROR_STOP=1','-c',q],capture_output=True,text=True)
 if p.returncode:raise RuntimeError('controlled retry SQL failed')
 return p.stdout.strip()
def lit(x):return "'"+x.replace("'","''")+"'"
def journal():
 raw=sql("SELECT retry_journal FROM governed_action_roots WHERE principal='uid:1000' AND root_id='session:"+session+"'",n['postgres']);d=json.loads(raw)
 for a in d['attempts']:a.pop('retained_input',None)
 return d
policy_path='/var/lib/aimee/policy.json'
old_policy=subprocess.check_output(['docker','exec',n['server'],'cat',policy_path])
settings=json.loads(old_policy)
settings['clean_retry']=dict(enabled=True,max_attempts=3,max_repeated_failures=2,max_wall_seconds=1200,max_provider_bytes=1000000,nanodollars_per_byte=1,max_nanodollars=1000000,retain_input_seconds=1200)
def write_policy(raw):
 subprocess.run(['docker','exec','-i','-u','0',n['server'],'python3','-c','import sys;from pathlib import Path;Path(sys.argv[1]).write_bytes(sys.stdin.buffer.read())',policy_path],input=raw,check=True,stdout=subprocess.DEVNULL)
try:
 write_policy(json.dumps(settings).encode())
 signal=int(sql("INSERT INTO learning_signals(signal_type,title) VALUES('explicit','MR17 controlled retry evidence') RETURNING id"))
 action=dict(scope_kind='global',scope_id='',text='MR17_OLD_PROCEDURE: verify mr07activation_state before reporting completion')
 proposal=int(sql("INSERT INTO learning_proposals(signal_id,sink,state,target_key,action_json) VALUES("+str(signal)+",'artifact','committed','mr07activation',"+lit(json.dumps(action))+") RETURNING id"))
 subprocess.run(['docker','cp','/opt/pr2990-mr17-live-inside.py',n['server']+':/tmp/mr17-live.py'],check=True,stdout=subprocess.DEVNULL)
 for phase in ['before','after']:
  if phase=='after':
   action['text']='MR17_NEW_PROCEDURE: inspect existing verified work; do not repeat a completed write'
   sql('UPDATE learning_proposals SET action_json='+lit(json.dumps(action))+' WHERE id='+str(proposal))
   subprocess.run(['docker','restart',n['server']],check=True,stdout=subprocess.DEVNULL)
   deadline=time.monotonic()+180
   while time.monotonic()<deadline:
    status=subprocess.check_output(['docker','inspect','--format','{{.State.Health.Status}}',n['server']],text=True).strip()
    if status=='healthy':break
    time.sleep(2)
   else:raise RuntimeError('candidate restart health timeout')
   print('actual server restart healthy; authoritative procedure correction committed',flush=True)
  process=subprocess.Popen(['docker','exec','-u','1000',n['server'],'python3','/tmp/mr17-live.py',head,phase,str(proposal)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  with (out/(phase+'.log')).open('w') as log:
   for line in process.stdout:
    log.write(line);log.flush();print(line,end='',flush=True)
    if line.startswith('MR17_SESSION '):session=line.strip().split()[1]
  if process.wait():raise RuntimeError('MR17 '+phase+' acceptance failed')
  j=journal();(out/(phase+'-journal.json')).write_text(json.dumps(j,indent=2)+'\n')
  if phase=='before':
   assert len(j['attempts'])==1 and j['attempts'][0]['state']=='failed' and j['reserved_bytes']>0
   before=j
  else:
   assert len(j['attempts'])==2 and j['attempts'][1]['state']=='completed' and j['reserved_bytes']>before['reserved_bytes'] and j['reserved_nanodollars']>before['reserved_nanodollars']
   assert j['attempts'][1]['parent_attempt']==j['attempts'][0]['attempt_id'] and j['attempts'][1]['plan_revision']!=j['attempts'][0]['plan_revision']
   print('PASS durable shared reservations and new plan survive restart',flush=True)
 (out/'fixture.json').write_text(json.dumps(dict(signal_id=signal,proposal_id=proposal,session=session,provider='controlled functional fixture; no task-quality or billing claim'))+'\n')
finally:
 write_policy(old_policy)
 for file in ['checks.json','captures.json','checkpoint.json','before-response.json','after-response.json']:
  subprocess.run(['docker','cp',n['server']+':/var/lib/aimee/mr17-'+head+'-v3/'+file,str(out/file)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 if signal is not None:sql('DELETE FROM learning_signals WHERE id='+str(signal))
