import json,os,pathlib,subprocess,time
p=pathlib.Path('/opt/validation042');images=json.loads((p/'evidence/core-images.json').read_text());results=[]
for n in range(1,4):
 start=time.monotonic();dest=p/'evidence'/f'T2-repeat-{n}'
 with (p/'private'/f'T2-repeat-{n}.log').open('w') as f:
  rc=subprocess.run(['python3','tests/e2e/deployment-matrix.py','--topology','T2','--output',str(dest)],cwd=p/'source',env=dict(os.environ,**images),stdout=f,stderr=subprocess.STDOUT).returncode
 results.append(dict(name=f'T2-repeat-{n}',passed=rc==0,elapsed_seconds=round(time.monotonic()-start,1)))
 (p/'evidence/T2-repeats.json').write_text(json.dumps(results,indent=2)+'\n');print(results[-1],flush=True)
 if rc:break
raise SystemExit(0 if all(r['passed'] for r in results) else 1)
