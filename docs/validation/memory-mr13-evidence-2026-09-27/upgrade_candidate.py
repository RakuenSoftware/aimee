import http.client,socket,json,sys,time,re,hashlib,subprocess,os
from pathlib import Path
head=sys.argv[1];assert re.fullmatch('[a-f0-9]{9}',head)
names=json.loads(Path('/opt/pr2990-evidence/mr07-managed-be46915db/names.json').read_text())
out=Path('/opt/pr2990-evidence/mr13-upgrade-'+head);out.mkdir(exist_ok=False,parents=True)
class Docker(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(90);self.sock.connect('/var/run/docker.sock')
def api(method,path,body=None):
 c=Docker('localhost');c.request(method,path,None if body is None else json.dumps(body),{'Content-Type':'application/json'});r=c.getresponse();raw=r.read();c.close()
 if r.status>=400:raise RuntimeError('Docker '+method+' '+path.split('?')[0]+' status '+str(r.status))
 return json.loads(raw) if raw else None
def recipe(info,image):
 config=dict(info['Config']);config['Image']=image
 if image=='aimee-pr2990:'+head:
  config['Env']=[v for v in config['Env'] if not v.startswith(('AIMEE_MEMORY_HEALTH_ENABLED=','AIMEE_MEMORY_SELECTION_POLICY=','AIMEE_MEMORY_UTILITY_HORIZON_POLICY='))]
 config['HostConfig']=info['HostConfig']
 config['NetworkingConfig']={'EndpointsConfig':{k:{'Aliases':v.get('Aliases',[]),'IPAMConfig':v.get('IPAMConfig'),'DriverOpts':v.get('DriverOpts')} for k,v in info['NetworkSettings']['Networks'].items()}}
 return config
prior={key:api('GET','/containers/'+names[key]+'/json') for key in ['server','kb']}
for info in prior.values():
 assert info['Config']['Labels']['com.docker.compose.project'] in [names['project'],names['kb_project']]
 assert info['Config']['Image']=='aimee-pr2990:5d4bcab87'
 assert not info['HostConfig'].get('PortBindings')
# Keep both private PostgreSQL stores, workspaces, Vaults and enrolled identities.
try:
 for key in ['server','kb']:api('POST','/containers/'+names[key]+'/stop?t=30')
 # Quiesce owners before schema change. Dumps contain private test data and remain
 # mode 0600 on CT109; only their hashes enter the public evidence record.
 backups={}
 for key in ['server','kb']:
  dbname=names['postgres'] if key=='server' else names['kb_project']+'-aimee-store-db-1'
  target=out/(key+'-before-upgrade.dump')
  fd=os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
  with os.fdopen(fd,'wb') as stream:
   result=subprocess.run(['docker','exec',dbname,'pg_dump','-U','postgres','-d','aimee_store','-Fc'],stdout=stream,stderr=subprocess.PIPE)
  if result.returncode:raise RuntimeError('owned database snapshot failed: '+key)
  backups[key]=dict(container=dbname,path=str(target),sha256=hashlib.sha256(target.read_bytes()).hexdigest())
 (out/'snapshot-identities.json').write_text(json.dumps({k:dict(sha256=v['sha256'],restoration_scope='exact owned CT109 store') for k,v in backups.items()},indent=2)+'\n')
except Exception:
 for key in ['kb','server']:
  api('POST','/containers/'+names[key]+'/start')
 raise

for key in ['server','kb']:api('DELETE','/containers/'+names[key])
try:
 for key in ['kb','server']:
  api('POST','/containers/create?name='+names[key],recipe(prior[key],'aimee-pr2990:'+head));api('POST','/containers/'+names[key]+'/start')
  deadline=time.monotonic()+180
  while time.monotonic()<deadline:
   current=api('GET','/containers/'+names[key]+'/json')
   if current['State'].get('Health',{}).get('Status')=='healthy':break
   time.sleep(2)
  else:raise RuntimeError(key+' candidate health timeout')
  print(key,'upgraded and healthy',flush=True)
except Exception:
 for key in ['server','kb']:
  try:api('DELETE','/containers/'+names[key]+'?force=1')
  except RuntimeError:pass
 for key,backup in backups.items():
  with open(backup['path'],'rb') as stream:
   result=subprocess.run(['docker','exec','-i',backup['container'],'pg_restore','--clean','--if-exists','--single-transaction','-U','postgres','-d','aimee_store'],stdin=stream,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
  if result.returncode:raise RuntimeError('owned database rollback requires reconciliation: '+key)
 for key in ['kb','server']:
  api('POST','/containers/create?name='+names[key],recipe(prior[key],prior[key]['Config']['Image']));api('POST','/containers/'+names[key]+'/start')
 print('owned topology rolled back',flush=True);raise
safe=[]
for key in ['server','kb']:
 current=api('GET','/containers/'+names[key]+'/json');safe.append(dict(role=key,previous_image=prior[key]['Image'],image=current['Image'],configured_image=current['Config']['Image'],healthy=current['State']['Health']['Status']=='healthy'))
(out/'image-identities.json').write_text(json.dumps(safe,indent=2)+'\n');(out/'names.json').write_text(json.dumps(names)+'\n')
