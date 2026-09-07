import json,os,pathlib,subprocess,time
p=pathlib.Path('/opt/validation042');out=p/'evidence/optin';out.mkdir(exist_ok=True)
images=json.loads((p/'evidence/core-images.json').read_text());images['AIMEE_POSTGRES_IMAGE']='aimee-postgres:optin-local'
env=dict(os.environ,**images);results=[]
cases=[('plain-'+t,['tests/e2e/deployment-matrix.py','--topology',t,'--output',str(out/('plain-'+t))],{}) for t in ('T3','T1','T2')]
cases += [('plain-upgrade041',['tests/e2e/kb-upgrade-e2e.py','--output',str(out/'plain-upgrade041')],{})]
cases += [('luks-T2',['tests/e2e/deployment-matrix.py','--topology','T2','--output',str(out/'luks-T2')],{'AIMEE_POSTGRES_STORAGE':'luks','AIMEE_DEVICE_MAPPER_MAJOR':'254'})]
for legacy in (False,True):
 name='luks-upgrade' if legacy else 'luks-fresh'
 args=['tests/e2e/postgres-luks-e2e.py','--server-image',images['AIMEE_APPLICATION_IMAGE'],'--postgres-image',images['AIMEE_POSTGRES_IMAGE'],'--evidence',str(out/(name+'.json'))]
 if legacy:args.append('--legacy-upgrade')
 cases.append((name,args,{}))
for name,args,extra in cases:
 start=time.monotonic()
 with (p/'private'/('optin-'+name+'.log')).open('w') as log:rc=subprocess.run(['python3',*args],cwd=p/'source-optin',env=dict(env,**extra),stdout=log,stderr=subprocess.STDOUT).returncode
 results.append(dict(name=name,passed=rc==0,elapsed_seconds=round(time.monotonic()-start,1)))
 (out/'runtime-suite.json').write_text(json.dumps(results,indent=2)+'\n');print(name,'PASS' if rc==0 else 'FAIL',flush=True)
raise SystemExit(not all(r['passed'] for r in results))
