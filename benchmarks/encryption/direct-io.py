#!/usr/bin/env python3
"""O_DIRECT I/O on task-owned files. Does not evict host or application caches."""
import json
import mmap
import os
import random
import statistics
import time
from pathlib import Path

ROOT = Path('/opt/aimee-encryption-bench')
SIZE = 512 * 1024 * 1024
BLOCK = 1024 * 1024
rng = random.Random(253)
buf = mmap.mmap(-1, BLOCK)
buf.write(os.urandom(BLOCK)); buf.seek(0)
view = memoryview(buf)
results = []
for trial in range(3):
    modes = ['ordinary', 'luks']; rng.shuffle(modes)
    for mode in modes:
        path = ROOT/'mounts'/mode/'aimee-benchmark-io.bin'
        fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_DIRECT, 0o600)
        try:
            start = time.perf_counter()
            for offset in range(0, SIZE, BLOCK):
                assert os.pwrite(fd, view, offset) == BLOCK
            os.fsync(fd)
            seconds = time.perf_counter()-start
            results.append({'mode':mode,'trial':trial,'operation':'sequential_write',
                            'bytes':SIZE,'seconds':seconds,'mib_per_second':SIZE/1048576/seconds})
            start = time.perf_counter()
            for offset in range(0, SIZE, BLOCK):
                assert os.preadv(fd, [view], offset) == BLOCK
            seconds = time.perf_counter()-start
            results.append({'mode':mode,'trial':trial,'operation':'sequential_read',
                            'bytes':SIZE,'seconds':seconds,'mib_per_second':SIZE/1048576/seconds})
            samples=[]
            for _ in range(4000):
                offset = rng.randrange(SIZE//4096)*4096
                start = time.perf_counter_ns()
                assert os.preadv(fd, [view[:4096]], offset) == 4096
                samples.append((time.perf_counter_ns()-start)/1000)
            results.append({'mode':mode,'trial':trial,'operation':'random_read_4k',
                            'operations':len(samples),'median_us':statistics.median(samples),
                            'p95_us':sorted(samples)[int(.95*len(samples))-1]})
            print(json.dumps(results[-3:]),flush=True)
        finally:
            os.close(fd)
        path.unlink()
view.release(); buf.close()
(ROOT/'results'/'direct-io.json').write_text(json.dumps(results,indent=2)+'\n')
