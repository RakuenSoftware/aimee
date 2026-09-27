import json,subprocess,sys,re
from pathlib import Path
head=sys.argv[1];assert re.fullmatch('[a-f0-9]{9}',head)
n=json.loads(Path('/opt/pr2990-evidence/mr07-managed-be46915db/names.json').read_text())
out=Path('/opt/pr2990-evidence/mr16-evidence-'+head);out.mkdir(exist_ok=False)
db=n['kb_project']+'-aimee-store-db-1';signal=None;proposal=None;session=None

def sql(q):
 p=subprocess.run(['docker','exec',db,'psql','-X','-qAt','-U','postgres','-d','aimee_store','-v','ON_ERROR_STOP=1','-c',q],capture_output=True,text=True)
 if p.returncode:raise RuntimeError('controlled procedure SQL failed')
 return p.stdout.strip()
def lit(x):return "'"+x.replace("'","''")+"'"
try:
 signal=int(sql("INSERT INTO learning_signals(signal_type,title) VALUES('explicit','MR16 controlled action evidence fixture') RETURNING id"))
 action=dict(scope_kind='global',scope_id='',text='MR16_PROCEDURE_MARKER: verify the exact approved file before reporting completion')
 proposal=int(sql("INSERT INTO learning_proposals(signal_id,sink,state,target_key,action_json) VALUES("+str(signal)+",'artifact','committed','mr07activation',"+lit(json.dumps(action))+") RETURNING id"))
 subprocess.run(['docker','cp','/opt/pr2990-mr16-evidence-inside.py',n['server']+':/tmp/mr16-evidence.py'],check=True,stdout=subprocess.DEVNULL)
 process=subprocess.Popen(['docker','exec','-i','-u','1000',n['server'],'python3','/tmp/mr16-evidence.py',head,str(proposal)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
 with (out/'acceptance.log').open('w') as log:
  for line in process.stdout:
   log.write(line);log.flush();print(line,end='',flush=True)
   if line.startswith('MR16_SESSION '):session=line.strip().split()[1]
   if line.strip()=='MR16_MUTATE_PROCEDURE':
    action['revision_note']='authoritative correction before action admission'
    sql('UPDATE learning_proposals SET action_json='+lit(json.dumps(action))+' WHERE id='+str(proposal))
    print('controlled procedure correction committed before tool response',flush=True);process.stdin.write('continue\n');process.stdin.flush()
 rc=process.wait()
 if rc:raise RuntimeError('MR16 evidence acceptance failed')
 (out/'fixture.json').write_text(json.dumps(dict(signal_id=signal,proposal_id=proposal,session=session,provider='controlled functional fixture; no measured task quality claim'))+'\n')
finally:
 for file in ['checks.json','captures.json','action-receipt.json','fresh-response.json','stale-response.json']:
  subprocess.run(['docker','cp',n['server']+':/var/lib/aimee/mr16-evidence-'+head+'/'+file,str(out/file)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 if signal is not None:sql('DELETE FROM learning_signals WHERE id='+str(signal))
