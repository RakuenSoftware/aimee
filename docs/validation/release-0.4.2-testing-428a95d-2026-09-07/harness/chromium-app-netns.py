#!/usr/bin/env python3
"""Keep browser requests isolated from unrelated Docker interface churn on the VM."""
import json,os,pathlib,subprocess,sys
state=json.loads(pathlib.Path('/opt/validation042/private/managed-state.json').read_text())
pid=subprocess.check_output(['docker','inspect','--format','{{.State.Pid}}',state['application']],text=True).strip()
assert pid.isdigit() and int(pid)>0
os.execvp('nsenter',['nsenter','--net=/proc/'+pid+'/ns/net','/usr/bin/chromium',*sys.argv[1:]])
