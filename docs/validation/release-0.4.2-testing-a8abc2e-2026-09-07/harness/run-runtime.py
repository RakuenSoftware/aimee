import json,os,pathlib,subprocess,time
p=pathlib.Path('/opt/validation042');out=p/'evidence';images=json.loads((out/'images.json').read_text());env=dict(os.environ,**images);results=[]
(out/'core-images.json').write_text(json.dumps({k:v for k,v in images.items() if k in ('AIMEE_APPLICATION_IMAGE','AIMEE_POSTGRES_IMAGE','AIMEE_EMBEDDER_IMAGE')},indent=2)+'\n')
def run(name,args,extra=None):
 start=time.monotonic()
 with (p/'private'/(name+'.log')).open('w') as log:rc=subprocess.run(['python3',*args],cwd=p/'source',env=dict(env,**(extra or {})),stdout=log,stderr=subprocess.STDOUT).returncode
 results.append(dict(name=name,passed=rc==0,elapsed_seconds=round(time.monotonic()-start,1)));(out/'runtime-suite.json').write_text(json.dumps(results,indent=2)+'\n');print(name,'PASS' if rc==0 else 'FAIL',flush=True)
 if rc:raise SystemExit(rc)
for topology in ('T1','T3','T2'):run(topology,['tests/e2e/deployment-matrix.py','--topology',topology,'--output',str(out/topology)])
run('plain-storage',['tests/e2e/postgres-plain-e2e.py','--postgres-image',images['AIMEE_POSTGRES_IMAGE'],'--evidence',str(out/'plain-storage.json')])
run('upgrade041',['tests/e2e/kb-upgrade-e2e.py','--output',str(out/'upgrade041')])
(out/'plain-completed.json').write_text(json.dumps(results,indent=2)+'\n')
# Optional encryption prerequisites are installed only after ordinary deployment
# qualification finishes. No kernel preparation is part of default startup.
with (p/'private/luks-host-setup.log').open('w') as log:
 subprocess.run(['apt-get','install','-y','cryptsetup-bin'],stdout=log,stderr=subprocess.STDOUT,check=True)
 for driver in ('loop','dm_mod','dm_crypt'):subprocess.run(['modprobe',driver],stdout=log,stderr=subprocess.STDOUT,check=True)
major=next(line.split()[0] for line in pathlib.Path('/proc/devices').read_text().splitlines() if line.split()[-1:]==['device-mapper'])
(out/'luks-host-setup.json').write_text(json.dumps(dict(after_plain_qualification=True,device_mapper_major=major,drivers=['loop','dm_mod','dm_crypt']),indent=2)+'\n')
run('luks-T2',['tests/e2e/deployment-matrix.py','--topology','T2','--output',str(out/'luks-T2')],dict(AIMEE_POSTGRES_STORAGE='luks',AIMEE_DEVICE_MAPPER_MAJOR=major))
run('luks-upgrade041',['tests/e2e/kb-upgrade-e2e.py','--storage','luks','--output',str(out/'luks-upgrade041')])
for legacy in (False,True):
 name='luks-upgrade' if legacy else 'luks-fresh';args=['tests/e2e/postgres-luks-e2e.py','--server-image',images['AIMEE_APPLICATION_IMAGE'],'--postgres-image',images['AIMEE_POSTGRES_IMAGE'],'--evidence',str(out/(name+'.json'))]
 if legacy:args.append('--legacy-upgrade')
 run(name,args)
(out/'runtime-completed.json').write_text(json.dumps(results,indent=2)+'\n')
