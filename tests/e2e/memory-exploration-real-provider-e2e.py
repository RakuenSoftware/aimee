#!/usr/bin/env python3
"""Real-provider smoke driver for a retained owned activation fixture.

Run beneath benchmarks/memory/native_provider_relay.py. Arguments are the owned
activation names.json and the exact candidate image suffix. The fixture must
already have completed memory-exploration-activation-e2e.py and revoked approval.
"""
import json
from pathlib import Path
import re
import subprocess
import sys

names = json.loads(Path(sys.argv[1]).read_text())
head = sys.argv[2]
assert re.fullmatch('[a-f0-9]{9}', head)
assert re.fullmatch('aimee-e2e-server-[a-f0-9]+', names['project'])
server = names['server']
inspection = json.loads(subprocess.check_output(['docker', 'inspect', server], text=True))[0]
assert inspection['Config']['Image'] == 'aimee-pr2990:' + head
assert inspection['Config']['Labels']['com.docker.compose.project'] == names['project']
subprocess.run(['docker', 'exec', server, 'test', '!', '-e',
                '/etc/aimee/exploration-experiment.json'], check=True)
inside = Path(__file__).with_name('memory-exploration-real-provider-inside.py')
target = '/tmp/mr07-real-provider-inside.py'
subprocess.run(['docker', 'cp', str(inside), server + ':' + target],
               check=True, stdout=subprocess.DEVNULL)
raise SystemExit(subprocess.call(['docker', 'exec', '-i', '-u', '1000', server,
                                 'python3', target]))
