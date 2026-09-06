#!/usr/bin/env python3
"""Compare dm-crypt scheduling on the existing disposable disk images."""
import argparse
import hashlib
import json
import os
import random
import subprocess
from pathlib import Path

ROOT = Path('/opt/aimee-encryption-bench')
CRYPT = str(ROOT/'tools/cryptsetup/usr/sbin/cryptsetup')
parser = argparse.ArgumentParser()
parser.add_argument('--storage', choices=['storage', 'storage-optane'], default='storage')
parser.add_argument('--output', default='luks-diagnostic')
parser.add_argument('--direct-loop', action='store_true')
args = parser.parse_args()
KEY = Path('/run/aimee-encryption-bench/fixture-drive.key').read_bytes()
MAPPER = 'aimee-bench-luks'
FLAGS = {'default': [],
         'no_queues': ['--perf-no_read_workqueue', '--perf-no_write_workqueue'],
         'same_cpu': ['--perf-same_cpu_crypt']}


def command(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, **kwargs)


def drivers():
    found = []
    for chunk in Path('/proc/crypto').read_text().split('\n\n'):
        fields = dict(line.split(':', 1) for line in chunk.splitlines() if ':' in line)
        fields = {k.strip(): v.strip() for k, v in fields.items()}
        if fields.get('name') == 'xts(aes)':
            found.append(fields)
    return found


def luks_open(loop, flags):
    command([CRYPT, 'open', '--key-file', '-', *flags, loop, MAPPER], input=KEY)
    command(['mount', '-o', 'noatime', '/dev/mapper/'+MAPPER, str(ROOT/'mounts/luks')])


def luks_close():
    command(['umount', str(ROOT/'mounts/luks')])
    command([CRYPT, 'close', MAPPER])


assert not Path('/dev/mapper'/Path(MAPPER)).exists()
assert not Path('/run/aimee-encryption-bench/dockerd.pid').exists()
loops = {}
for mode in ('ordinary', 'luks'):
    image = str(ROOT/args.storage/f'{mode}.img')
    assert not command(['losetup', '-j', image], text=True).stdout.strip()
    extra = ['--offset', '16777216'] if mode == 'ordinary' else []
    if args.direct_loop:
        extra.append('--direct-io=on')
    loops[mode] = command(['losetup', '--find', '--show', *extra, image], text=True).stdout.strip()
command(['mount', '-o', 'noatime', loops['ordinary'], str(ROOT/'mounts/ordinary')])
before = drivers()
luks_open(loops['luks'], [])
after = drivers()
existing = ROOT/'mounts/ordinary/aimee-diagnostic-io.bin'
if existing.exists():
    with existing.open('rb') as handle:
        fixture = handle.read(1024*1024)
else:
    fixture = os.urandom(1024*1024)
expected = hashlib.sha256(fixture).hexdigest()
for mode in ('ordinary', 'luks'):
    path = ROOT/'mounts'/mode/'aimee-diagnostic-io.bin'
    if not path.exists():
        with path.open('xb', buffering=0) as handle:
            for _ in range(512):
                assert handle.write(fixture) == len(fixture)
            os.fsync(handle.fileno())
    assert path.stat().st_size == 512*1024*1024
    with path.open('rb') as handle:
        assert hashlib.sha256(handle.read(1024*1024)).hexdigest() == expected
luks_close()
result = {'storage': args.storage, 'direct_loop': args.direct_loop,
          'loop_setup': command(['losetup', '--list', '--output', 'NAME,BACK-FILE,DIO,LOG-SEC'], text=True).stdout,
          'crypto_before_mapping': before, 'crypto_with_mapping': after,
          'fixture_first_mib_sha256': expected, 'samples': [], 'configurations': {}}
rng = random.Random(253)
for trial in range(3):
    modes = ['ordinary', *FLAGS]
    rng.shuffle(modes)
    for mode in modes:
        mount = 'ordinary' if mode == 'ordinary' else 'luks'
        if mode != 'ordinary':
            luks_open(loops['luks'], FLAGS[mode])
            result['configurations'][mode] = command([CRYPT, 'status', MAPPER], text=True).stdout
        path = ROOT/'mounts'/mount/'aimee-diagnostic-io.bin'
        with path.open('rb') as handle:
            assert hashlib.sha256(handle.read(1024*1024)).hexdigest() == expected
        cases = [(1048576, 1, 0), (1048576, 8, 0), (4096, 1, 1), (4096, 8, 1)]
        rng.shuffle(cases)
        for block, workers, random_io in cases:
            device_before = int(Path('/sys/block/nvme0n1/stat').read_text().split()[2])
            output = command([str(ROOT/'io-read'), str(path), str(block), str(workers),
                              '1.5', str(random_io)], text=True).stdout
            device_after = int(Path('/sys/block/nvme0n1/stat').read_text().split()[2])
            sample = json.loads(output)
            if args.storage == 'storage-optane':
                sample['device_read_bytes'] = (device_after-device_before)*512
                sample['logical_read_bytes'] = sample['operations']*block
                assert sample['device_read_bytes'] >= sample['logical_read_bytes']*.95, sample
            sample.update(mode=mode, trial=trial, host_loadavg=Path('/proc/loadavg').read_text().strip())
            result['samples'].append(sample)
            print(json.dumps(sample), flush=True)
        if mode != 'ordinary':
            luks_close()
        (ROOT/'results'/f'{args.output}.json').write_text(json.dumps(result, indent=2)+'\n')
# Leave mounts available for the subsequent PostgreSQL comparison; retain fixture data.
luks_open(loops['luks'], FLAGS['no_queues'])
(ROOT/'results'/f'{args.output}-loops.json').write_text(json.dumps(loops, indent=2)+'\n')
print('Diagnostic complete; LUKS reopened with workqueue bypass for PostgreSQL verification', flush=True)
