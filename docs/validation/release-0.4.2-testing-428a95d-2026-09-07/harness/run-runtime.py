import json,os,pathlib,subprocess,time
p=pathlib.Path('/opt/validation042');out=p/'evidence';images={}
for key,name in [('AIMEE_APPLICATION_IMAGE','aimee'),('AIMEE_POSTGRES_IMAGE','aimee-postgres'),('AIMEE_EMBEDDER_IMAGE','aimee-embedder-a25m')]:
 tag='ghcr.io/rakuensoftware/'+name+':testing'
 current=json.loads(subprocess.check_output(['docker','image','inspect',tag]))[0]
 if name in ('aimee','aimee-postgres'):
  pinned=tag+'-428a95d'
  subprocess.run(['docker','pull',pinned],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
  assert current['Id']==json.loads(subprocess.check_output(['docker','image','inspect',pinned]))[0]['Id']
 images[key]=current['RepoDigests'][0]
(out/'core-images.json').write_text(json.dumps(images,indent=2)+'\n')
(p/'core-images.env').write_text(''.join(f'export {k}={v}\n' for k,v in images.items()))
cases=[(t,['tests/e2e/deployment-matrix.py','--topology',t,'--output',str(out/t)]) for t in ('T1','T3','T2')]
cases.append(('upgrade041',['tests/e2e/kb-upgrade-e2e.py','--output',str(out/'upgrade041')]))
for legacy in (False,True):
 name='luks-upgrade' if legacy else 'luks-fresh'
 args=['tests/e2e/postgres-luks-e2e.py','--server-image',images['AIMEE_APPLICATION_IMAGE'],'--postgres-image',images['AIMEE_POSTGRES_IMAGE'],'--evidence',str(out/(name+'.json'))]
 if legacy:args.append('--legacy-upgrade')
 cases.append((name,args))
results=[]
for name,args in cases:
 start=time.monotonic()
 with (p/'private'/(name+'.log')).open('w') as log:
  rc=subprocess.run(['python3',*args],cwd=p/'source',env=dict(os.environ,**images),stdout=log,stderr=subprocess.STDOUT).returncode
 results.append({'name':name,'passed':rc==0,'elapsed_seconds':round(time.monotonic()-start,1)})
 (out/'runtime-suite.json').write_text(json.dumps(results,indent=2)+'\n')
 print(name,'PASS' if rc==0 else 'FAIL',flush=True)
raise SystemExit(0 if all(r['passed'] for r in results) else 1)
