#!/usr/bin/env python3
"""Run one bounded action on a disposable lab's loopback owner over SSH stdin."""
import json
from pathlib import Path
import sys
import urllib.request
state=json.loads(Path('/var/lib/aimee-memory-lab/resume-private.json').read_text())
token=state['kb-supervisor']['env']['AIMEE_KB_API_BEARER_TOKEN']
request=json.loads(sys.stdin.buffer.read(1048577))
name=request['name']
if not name.startswith('memory.') or not name.replace('.', '').replace('_','').isalnum():
    raise SystemExit('memory action required')
req=urllib.request.Request('http://127.0.0.1:8741/v1/actions/'+name,data=json.dumps(request['body']).encode(),
    headers={'Authorization':'Bearer '+token,'Content-Type':'application/json'})
with urllib.request.urlopen(req,timeout=30) as response:
    sys.stdout.write(response.read(1048576).decode())
