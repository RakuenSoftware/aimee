import hashlib,http.client,json,os,socket,subprocess,sys,time,statistics
from pathlib import Path
head,phase=sys.argv[1:];prefix='mr16-'+head
root=Path('/var/lib/aimee/'+prefix);root.mkdir(exist_ok=True)
checks=json.loads((root/'checks.json').read_text()) if (root/'checks.json').exists() else []
def check(name,ok):
 checks.append(dict(name=name,passed=bool(ok)));(root/'checks.json').write_text(json.dumps(checks,indent=2)+'\n');print(('PASS ' if ok else 'FAIL ')+name,flush=True)
 if not ok:raise RuntimeError(name)
class Local(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(90);self.sock.connect('/var/lib/aimee/aimee-http.sock')
def api(path,body,request=None):
 c=Local('localhost');headers={'Content-Type':'application/json'}
 if request:headers['X-Request-ID']=request
 started=time.monotonic();c.request('POST',path,json.dumps(body),headers);r=c.getresponse();raw=r.read();status=r.status;c.close()
 try:d=json.loads(raw)
 except ValueError:d={'unparsed':raw.decode()[:500]}
 return status,d,(time.monotonic()-started)*1000
repo=root/'workspace';repo.mkdir(exist_ok=True)
target=repo/(prefix+'.txt');other=repo/(prefix+'-other.txt')
def tool(session,name,args,request):
 status,d,elapsed=api('/v1/tools/execute',dict(tool=name,arguments=json.dumps(args),cwd=str(repo),session_id=session,timeout_ms=10000),request)
 raw=d.get('result','');
 try:result=json.loads(raw)
 except (ValueError,TypeError):result={'raw':raw}
 (root/'last-response.json').write_text(json.dumps(dict(status=status,response=d),indent=2)+'\n')
 return result,elapsed
def receipt(session,action,op='inspect',args=None):
 body=dict(operation=op,session_id=session,action_id=action)
 if args is not None:body.update(directory=str(repo),arguments_json=json.dumps(args))
 return api('/v1/action/receipt',body)[1]
if phase=='before':
 status,d,_=api('/v1/sessions/create',dict(client_type=prefix));session=d.get('session_id');check('owned action session created',status==200 and bool(session))
 target.write_text('before');args=dict(path=str(target),content='approved-'+head)
 r,_=tool(session,'read_file',dict(path=str(target)),prefix+'-read');check('native read receives durable acknowledgement',r.get('completion_claim')=='provider_acknowledged')
 r,ms=tool(session,'write_file',args,prefix+'-write');check('native write verifies exact file effect',r.get('completion_claim')=='effect_confirmed' and target.read_text()==args['content'])
 action=r['action_id'];owned=receipt(session,action);check('principal receipt inspection reports verified effect',owned.get('completion_claim')=='effect_confirmed' and owned['receipt']['intent']['destination']=='file:'+str(target))
 target.write_text('sentinel-after-effect')
 r,_=tool(session,'write_file',args,prefix+'-write');check('duplicate request returns receipt without repeating effect',target.read_text()=='sentinel-after-effect' and r.get('completion_claim')=='effect_confirmed')
 r,_=tool(session,'write_file',dict(args,content='different'),prefix+'-write');check('changed payload cannot reuse the committed action',target.read_text()=='sentinel-after-effect' and r.get('completion_claim')!='effect_confirmed')
 r,_=tool(session,'write_file',dict(args,path=str(other)),prefix+'-write');check('changed destination cannot reuse committed action',not other.exists() and r.get('completion_claim')!='effect_confirmed')
 r,_=tool(session,'example:publish',dict(path=str(target),side_effect='read_only'),prefix+'-forged');check('unresolved external publisher cannot self-label as read',r.get('completion_claim')!='effect_confirmed' and target.read_text()=='sentinel-after-effect')
 p=subprocess.run(['aimee','action','receipt','inspect','--session_id',session,'--action_id',action,'--json'],env=dict(os.environ,AIMEE_API_ENDPOINT='unix:/var/lib/aimee/aimee-http.sock'),capture_output=True,text=True,timeout=30)
 check('compiled thin-client command reaches owned receipt',p.returncode==0 and 'effect_confirmed' in p.stdout)
 (root/'checkpoint.json').write_text(json.dumps(dict(session=session,action=action,args=args,initial_write_ms=ms))+'\n')
 print('MR16_RESTART_READY',flush=True)
elif phase=='after':
 saved=json.loads((root/'checkpoint.json').read_text());session=saved['session'];args=saved['args'];action=saved['action']
 check('receipt survives actual server restart',receipt(session,action).get('completion_claim')=='effect_confirmed')
 r,_=tool(session,'write_file',args,prefix+'-write');check('restart cannot replay a committed effect',target.read_text()=='sentinel-after-effect' and r.get('completion_claim')=='effect_confirmed')
 # A write-only file permits mutation but refuses readback. This exercises a
 # real verification failure, not a caller-supplied unknown outcome assertion.
 target.chmod(0o200)
 unknown_args=dict(args,content='unverified-'+head)
 try:r,_=tool(session,'write_file',unknown_args,prefix+'-unknown')
 finally:target.chmod(0o600)
 check('failed postcondition cannot claim confirmed effect',r.get('completion_claim')=='outcome_unknown')
 unknown_id=r['action_id'];check('unverified mutation really occurred',target.read_text()==unknown_args['content'])
 wrong=receipt(session,unknown_id,'reconcile',dict(unknown_args,content='wrong'))
 check('wrong material cannot reconcile unknown outcome',wrong.get('completion_claim')=='outcome_unknown' and 'unverified' in wrong.get('reconciliation',''))
 r,_=tool(session,'write_file',dict(args,content='blind-retry'),prefix+'-blind-retry')
 check('new attempt cannot bypass unresolved destination',target.read_text()==unknown_args['content'] and r.get('completion_claim')!='effect_confirmed')
 proved=receipt(session,unknown_id,'reconcile',unknown_args);check('owner verifies original object during reconciliation',proved.get('completion_claim')=='effect_confirmed')
 denied=receipt('non-owned-session',unknown_id);check('unknown or foreign session cannot inspect receipt',denied.get('completion_claim')!='effect_confirmed')
 cancelled=receipt(session,unknown_id,'cancel');check('started effect cannot be relabeled as cancelled',cancelled.get('allowed') is False and cancelled.get('completion_claim')=='effect_confirmed')
 elapsed=[]
 for i in range(12):
  r,ms=tool(session,'read_file',dict(path=str(target)),prefix+'-timing-'+str(i));check('measured governed read '+str(i),r.get('completion_claim')=='provider_acknowledged');elapsed.append(ms)
 (root/'timing.json').write_text(json.dumps(dict(kind='synthetic local tool API end-to-end wall time, not isolated admission overhead',samples=len(elapsed),median_ms=statistics.median(elapsed),max_ms=max(elapsed),initial_write_ms=saved['initial_write_ms']),indent=2)+'\n')
 target.unlink(missing_ok=True);other.unlink(missing_ok=True)
else:raise RuntimeError('unknown phase')
