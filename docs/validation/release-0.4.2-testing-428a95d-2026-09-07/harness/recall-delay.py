import importlib.util,json,pathlib,subprocess,time
from types import SimpleNamespace
p=pathlib.Path('/opt/validation042');s=json.loads((p/'private/managed-state.json').read_text())
spec=importlib.util.spec_from_file_location('placement',p/'source/tests/e2e/memory-placement-e2e.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);g=m.Gate(SimpleNamespace(server=s['application'],store_db=s['postgres']))
code,b=g.call('store',dict(key='release042-recall-delay',content='Synthetic delayed recall canary'));assert code==200 and 'id' in b;mid=b['id']
lock=subprocess.Popen(['docker','exec',s['postgres'],'psql','-U','postgres','-d','aimee_store','-X','-At','-v','ON_ERROR_STOP=1','-c',"BEGIN; LOCK TABLE user_memories IN ACCESS EXCLUSIVE MODE; SELECT pg_sleep(1.5); COMMIT;"],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
 for _ in range(100):
  locked=g.docker('exec',s['postgres'],'psql','-U','postgres','-d','aimee_store','-X','-At','-c',"SELECT count(*) FROM pg_locks WHERE relation='user_memories'::regclass AND mode='AccessExclusiveLock' AND granted")
  if locked=='1':break
  time.sleep(.01)
 else:raise RuntimeError('lock never acquired')
 start=time.monotonic();code,b=g.call('recall',dict(query='release042-recall-delay',store='user'));elapsed=time.monotonic()-start
 result=dict(http=code,message=b.get('message'),elapsed_seconds=round(elapsed,3),canary_returned=any(r.get('text')=='Synthetic delayed recall canary' for r in b.get('recall',{}).get('active_context',[])))
 print(json.dumps(result));(p/'evidence/recall-delay-published.json').write_text(json.dumps(result,indent=2)+'\n')
finally:
 lock.communicate(timeout=10);g.call('delete',dict(id=mid))
