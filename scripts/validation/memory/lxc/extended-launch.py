import json,os,sys
from pathlib import Path
backend=sys.argv[1]
root=Path('/var/lib/aimee-memory-lab');run=Path((root/'last-run').read_text().strip());scratch=run/'stack';client=scratch/'client'
remote=(client/'remote.conf').read_text().splitlines()
env=os.environ.copy();env.update(json.loads(Path('/root/aimee-validation-env.json').read_text()))
env.update(REPO='/opt/aimee',RUN_ROOT=str(run),SCRATCH=str(scratch),SERVER_URL=remote[0],BEARER=remote[1],CLIENT_CERT=str(client/'tls/client.crt'),CLIENT_KEY=str(client/'tls/client.key'),AIMEE_MEMORY_BACKEND=backend)
os.execve('/opt/aimee/scripts/aimee-memory-lxc-extended.py',['aimee-memory-lxc-extended.py'],env)
