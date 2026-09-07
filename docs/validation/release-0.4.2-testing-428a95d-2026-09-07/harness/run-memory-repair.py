import json,os,pathlib,subprocess,time
p=pathlib.Path('/opt/validation042');root=p/'repair-source';images=json.loads((p/'evidence/core-images.json').read_text());published=images['AIMEE_APPLICATION_IMAGE']
s='FROM '+published+'\nCOPY --from=aimee-memory-repair-build:428a95d /src/aimee-server /usr/local/bin/aimee-server\n'
(root/'Dockerfile.memory-repair-runtime').write_text(s)
with (p/'private/memory-repair-runtime-build.log').open('w') as f:
 subprocess.run(['docker','build','-f','Dockerfile.memory-repair-runtime','-t','aimee-memory-repair:428a95d','.'],cwd=root,stdout=f,stderr=subprocess.STDOUT,check=True)
images['AIMEE_APPLICATION_IMAGE']='aimee-memory-repair:428a95d'
image=json.loads(subprocess.check_output(['docker','image','inspect',images['AIMEE_APPLICATION_IMAGE']]))[0]
(p/'evidence/memory-repair-image.json').write_text(json.dumps(dict(image_id=image['Id'],published_base=published,changed_binary='/usr/local/bin/aimee-server',published=False),indent=2)+'\n')
results=[]
for t in ('T2','T3'):
 start=time.monotonic()
 with (p/'private'/('memory-repair-'+t+'.log')).open('w') as f:
  rc=subprocess.run(['python3','tests/e2e/deployment-matrix.py','--topology',t,'--output',str(p/'evidence'/('memory-repair-'+t))],cwd=root,env=dict(os.environ,**images),stdout=f,stderr=subprocess.STDOUT).returncode
 result=dict(name=t,passed=rc==0,elapsed_seconds=round(time.monotonic()-start,1));results.append(result)
 (p/'evidence/memory-repair-runtime.json').write_text(json.dumps(results,indent=2)+'\n');print(result,flush=True)
 if rc:break
raise SystemExit(0 if all(r['passed'] for r in results) else 1)
