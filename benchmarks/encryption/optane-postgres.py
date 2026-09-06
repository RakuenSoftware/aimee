#!/usr/bin/env python3
"""Read existing synthetic databases through Docker on the verified Optane images."""
import json
import os
import random
import subprocess
import time
from pathlib import Path

import psycopg2

ROOT = Path('/opt/aimee-encryption-bench')
DOCKER = str(ROOT/'tools/docker/docker')
CRYPT = str(ROOT/'tools/cryptsetup/usr/sbin/cryptsetup')
SOCKET = 'unix:///run/aimee-encryption-bench/docker.sock'
KEY = Path('/run/aimee-encryption-bench/fixture-drive.key').read_bytes()
LOOPS = json.loads((ROOT/'results/optane-diagnostic-loops.json').read_text())
MAPPER = 'aimee-bench-luks'
FLAGS = {'default': [], 'no_queues': ['--perf-no_read_workqueue', '--perf-no_write_workqueue']}
SQL = 'SELECT count(*),sum(length(md5(ciphertext))) FROM encrypted WHERE id <= 100000'


def command(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, **kwargs)


def docker(*args):
    return command([DOCKER, '--host', SOCKET, *args], text=True).stdout


def sectors():
    return int(Path('/sys/block/nvme0n1/stat').read_text().split()[2])


assert not Path('/run/aimee-encryption-bench/dockerd.pid').exists()
for name, loop in LOOPS.items():
    assert command(['losetup', '-n', '-O', 'BACK-FILE', loop], text=True).stdout.strip() == str(ROOT/'storage-optane'/f'{name}.img')
env = dict(os.environ, PATH=str(ROOT/'tools/docker')+':'+os.environ['PATH'])
with (ROOT/'docker-optane.log').open('ab') as log:
    daemon = subprocess.Popen([str(ROOT/'tools/docker/dockerd'), '--host', SOCKET,
        '--data-root', str(ROOT/'docker-data'), '--exec-root', '/run/aimee-encryption-bench/docker-exec',
        '--pidfile', '/run/aimee-encryption-bench/dockerd.pid', '--bridge', 'none',
        '--iptables=false', '--ip6tables=false', '--ip-forward=false', '--ip-masq=false',
        '--storage-driver', 'vfs'], stdout=log, stderr=log, start_new_session=True, env=env)
for _ in range(100):
    try:
        docker('info')
        break
    except subprocess.CalledProcessError:
        time.sleep(.1)
else:
    raise RuntimeError('Docker daemon did not become ready')
result = {'sql': SQL, 'cache_reset': 'container stop; inner ext4 unmount/mount; PostgreSQL start',
          'outer_cache': 'primarycache=metadata, secondarycache=none, direct=always',
          'samples': [], 'containers': json.loads(docker('inspect', 'aimee-encryption-ordinary', 'aimee-encryption-luks'))}
rng = random.Random(254)
for trial in range(3):
    modes = ['ordinary', *FLAGS]
    rng.shuffle(modes)
    for mode in modes:
        name = 'ordinary' if mode == 'ordinary' else 'luks'
        mount = str(ROOT/'mounts'/name)
        command(['umount', mount])
        if name == 'luks':
            command([CRYPT, 'close', MAPPER])
            command([CRYPT, 'open', '--key-file', '-', *FLAGS[mode], LOOPS[name], MAPPER], input=KEY)
        device = LOOPS[name] if name == 'ordinary' else '/dev/mapper/'+MAPPER
        command(['mount', '-o', 'noatime', device, mount])
        container = 'aimee-encryption-'+name
        docker('start', container)
        for _ in range(200):
            try:
                conn = psycopg2.connect(host=str(ROOT/'sockets'/name), dbname='bench', user='postgres')
                break
            except psycopg2.OperationalError:
                time.sleep(.1)
        else:
            raise RuntimeError('PostgreSQL did not become ready')
        conn.autocommit = True
        with conn.cursor() as cur:
            cur.execute("SET jit=off; SET max_parallel_workers_per_gather=0; SET statement_timeout='60s'")
            for cache in ('cold', 'warm'):
                before = sectors()
                start = time.perf_counter()
                cur.execute(SQL)
                rows = cur.fetchall()
                elapsed = time.perf_counter()-start
                after = sectors()
                assert rows == [(100000, 3200000)], rows
                sample = dict(trial=trial, mode=mode, cache=cache, milliseconds=elapsed*1000,
                              device_read_bytes=(after-before)*512, result=[list(row) for row in rows],
                              host_loadavg=Path('/proc/loadavg').read_text().strip())
                result['samples'].append(sample)
                print(json.dumps(sample), flush=True)
            if trial == 0:
                cur.execute('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) '+SQL)
                result.setdefault('plans', {})[mode] = cur.fetchone()[0]
        conn.close()
        if trial == 0:
            cpu = docker('exec', container, 'cat', '/proc/cpuinfo')
            result.setdefault('cpu_flags', {})[mode] = next(line for line in cpu.splitlines() if line.startswith('flags'))
        docker('stop', '-t', '30', container)
        (ROOT/'results/optane-postgres.json').write_text(json.dumps(result, indent=2)+'\n')
print('PostgreSQL comparison complete; containers stopped, daemon retained for verified cleanup', flush=True)
