import json,pathlib,subprocess,time,shutil
p=pathlib.Path('/opt/validation042');out=p/'evidence';images=json.loads((out/'core-images.json').read_text());results=json.loads((out/'runtime-suite.json').read_text());archive=p/'private/host-setup-cryptsetup';archive.mkdir(exist_ok=True)
for legacy in (False,True):
 name='luks-upgrade' if legacy else 'luks-fresh'
 shutil.copy2(p/'private'/(name+'.log'),archive/(name+'.log'))
 args=['python3','tests/e2e/postgres-luks-e2e.py','--server-image',images['AIMEE_APPLICATION_IMAGE'],'--postgres-image',images['AIMEE_POSTGRES_IMAGE'],'--evidence',str(out/(name+'.json'))]
 if legacy:args.append('--legacy-upgrade')
 start=time.monotonic()
 with (p/'private'/(name+'.log')).open('w') as log:rc=subprocess.run(args,cwd=p/'source',stdout=log,stderr=subprocess.STDOUT).returncode
 result=dict(name=name,passed=rc==0,elapsed_seconds=round(time.monotonic()-start,1));results=[result if r['name']==name else r for r in results]
 (out/'runtime-suite.json').write_text(json.dumps(results,indent=2)+'\n');print(name,'PASS' if rc==0 else 'FAIL',flush=True)
(out/'runtime-completed.json').write_text(json.dumps(results,indent=2)+'\n')
raise SystemExit(not all(r['passed'] for r in results))
