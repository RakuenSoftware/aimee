"""Controlled MR15 acceptance; synthetic rows in the existing isolated CT109 KB."""
import json,subprocess,sys,re
from pathlib import Path
head=sys.argv[1];assert re.fullmatch('[a-f0-9]{9}',head)
n=json.loads(Path('/opt/pr2990-evidence/mr07-managed-be46915db/names.json').read_text())
out=Path('/opt/pr2990-evidence/mr15-experience-'+head);out.mkdir(exist_ok=False)
db=n['kb_project']+'-aimee-store-db-1';signal=None;proposal=None

def sql(q):
 p=subprocess.run(['docker','exec',db,'psql','-X','-qAt','-U','postgres','-d','aimee_store','-v','ON_ERROR_STOP=1','-c',q],capture_output=True,text=True)
 if p.returncode:
  (out/'fixture-error.txt').write_text(p.stderr);raise RuntimeError('fixture SQL failed')
 return p.stdout.strip()
def literal(x):return "'"+x.replace("'","''")+"'"
try:
 signal=int(sql("INSERT INTO learning_signals(signal_type,title) VALUES('explicit','MR15 controlled protocol fixture') RETURNING id"))
 action=dict(scope_kind='global',scope_id='',text="MR15_PROCEDURE_MARKER: inspect the task's principal-owned receipt before reporting an outcome")
 proposal=int(sql("INSERT INTO learning_proposals(signal_id,sink,state,target_key,action_json) VALUES("+str(signal)+",'artifact','committed','mr07activation',"+literal(json.dumps(action))+") RETURNING id"))
 subprocess.run(['docker','cp','/opt/pr2990-mr15-live-inside.py',n['server']+':/tmp/mr15-live.py'],check=True,stdout=subprocess.DEVNULL)
 process=subprocess.Popen(['docker','exec','-i','-u','1000',n['server'],'python3','/tmp/mr15-live.py',head,str(proposal)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
 with (out/'acceptance.log').open('w') as log:
  for line in process.stdout:
   log.write(line);log.flush();print(line,end='',flush=True)
   if line.strip()=='MR15_CHANGE_VERSION':
    action['revision_note']='second controlled version'
    sql('UPDATE learning_proposals SET action_json='+literal(json.dumps(action))+' WHERE id='+str(proposal))
    process.stdin.write('continue\n');process.stdin.flush()
 rc=process.wait()
 for file in ['checks.json','captures.json','admission-diagnostic.json']:
  subprocess.run(['docker','cp',n['server']+':/var/lib/aimee/mr15-live-'+head+'/'+file,str(out/file)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 if rc:raise RuntimeError('MR15 acceptance failed')
 (out/'fixture.json').write_text(json.dumps(dict(signal_id=signal,proposal_id=proposal,provider='controlled functional fixture, not a task-quality benchmark'))+'\n')
finally:
 if proposal is not None:
  sql("DELETE FROM learning_application_events WHERE governed_event->'procedure'->>'procedure_id'="+literal(str(proposal))+" AND governed_event->>'task_id'="+literal('mr15-'+head+'-task'))
 if signal is not None:sql('DELETE FROM learning_signals WHERE id='+str(signal))
