# Measure PostgreSQL encryption in Docker

This harness compares ordinary and LUKS-backed storage, each with plaintext and
`pgcrypto`-encrypted document bodies. It runs PostgreSQL directly in Docker on
192.168.1.253. The [results](results/2026-09-06-253/report.md) include query plans,
raw timings, result checks, and the changes those measurements support.

All documents and keys are synthetic. SQL timings start with per-record key
fixtures already available in a temporary session table. The native scope-wrap
test measures the repository's Vault cipher primitives. Vault service calls,
authorization, migration coordination, and the application search API remain
outside these measurements.

Managed encrypted storage requires a CPU with AES-NI exposed to the host kernel
and a usable accelerated AES-XTS driver. The proposal requires installer
preflight to verify both before provisioning or migration. VAES and AVX2 are
not prerequisites; the measured host happens to support them. The diagnostic
records the selected kernel driver as well as CPU flags.

## Provision the isolated environment

Run setup as root on the benchmark host. It creates fresh disk-image files in a
dedicated ZFS dataset and refuses existing images. The script targets this host's
`rpool` to reproduce the original SATA-backed fixtures. Those cached read rates
cannot measure Optane performance. Use the corrected Optane procedure below for
the storage comparison. Setup never formats an existing host disk.

The host's default Docker command connects to an LXC compatibility service. These
measurements use the official Docker Engine 28.5.2 static distribution with a
separate socket, data directory, and runtime directory. Existing services keep
their configuration. Download the engine from
[Docker's static releases](https://download.docker.com/linux/static/stable/x86_64/docker-28.5.2.tgz)
and extract its `docker` directory under `/opt/aimee-encryption-bench/tools`.

```bash
bench_root=/opt/aimee-encryption-bench
mkdir -p /run/aimee-encryption-bench
export PATH="$bench_root/tools/docker:$PATH"
nohup "$bench_root/tools/docker/dockerd" \
  --host unix:///run/aimee-encryption-bench/docker.sock \
  --data-root "$bench_root/docker-data" \
  --exec-root /run/aimee-encryption-bench/docker-exec \
  --pidfile /run/aimee-encryption-bench/dockerd.pid \
  --bridge none --iptables=false --ip6tables=false \
  --ip-forward=false --ip-masq=false --storage-driver vfs \
  > "$bench_root/docker.log" 2>&1 &
export DOCKER_HOST=unix:///run/aimee-encryption-bench/docker.sock
"$bench_root/tools/docker/docker" pull \
  postgres@sha256:4ef4dbc939d61acea57712655ddb4b4ab27419c913f94cca0cd57cb3ea3c2280
"$bench_root/tools/docker/docker" tag \
  postgres@sha256:4ef4dbc939d61acea57712655ddb4b4ab27419c913f94cca0cd57cb3ea3c2280 postgres:18
```

Copy this directory's scripts to `bench_root`. Setup expects cryptsetup 2.7.5 at
`tools/cryptsetup/usr/sbin/cryptsetup`. We extracted Debian's `cryptsetup-bin`
package locally using `dpkg-deb -x`; the host already supplied `libcryptsetup12`,
ext4 tools, ZFS, loop devices, and device mapper. Python used locally extracted
`python3-psycopg2` 2.9.10 and `libpq5` 17.11 under `tools/pydep`.

Run `bash storage-setup.sh`. Its dedicated AppArmor profile follows the
[Moby template](https://github.com/moby/profiles/blob/main/apparmor/template.go)
with ABI 3 declared explicitly; its [license](LICENSE.moby) is included. This host otherwise rejected Unix socket
creation under Docker's generated profile. Both containers have networking
disabled and expose only Unix sockets beneath the task directory.

Wait for both containers to report ready. Setup records the image digest,
container limits, mount paths, LUKS parameters, and host details in `results/`.
Both ext4 filesystems use 12 GiB images on the same ZFS dataset, with outer
compression disabled. PostgreSQL data, WAL, and database temporary files use those
mounts. Container image storage is outside the measured data path.

## Run the comparisons in order

```bash
export PYTHONPATH="$bench_root/tools/pydep/usr/lib/python3/dist-packages"
export LD_LIBRARY_PATH="$bench_root/tools/pydep/usr/lib/x86_64-linux-gnu"
cd "$bench_root"
python3 -u measure.py init --rows 100000 --width 4096
python3 -u verify-edges.py
python3 -u measure.py measure --rows 100000 --repeats 5
python3 -u ordered-page.py
python3 -u measure.py concurrent --rows 100000
python3 -u measure.py write --rows 100000 --repeats 5
python3 -u direct-io.py
python3 -u short-fragments.py
python3 -u short-probe.py
```

`init` creates tables once. `write` adds 600 rows beyond the search ID range;
running it again requires a fresh fixture or removing those benchmark rows.
`short-fragments.py` creates its supplementary projection once, after the
original storage and write comparisons. Search measurements can be repeated.
Run these steps sequentially to avoid interference between benchmark workloads.

The corpus contains 100 projects, 4 KiB ASCII bodies, a rare term every 5,000
rows, and deliberate fragment false positives. Query scopes select 10,000 or
100,000 eligible IDs from the same physical tables. Each ordinary search gets
one warmup, an `EXPLAIN ANALYZE`, and five timed repetitions in shuffled order.
Returned IDs and bodies must match across all four configurations.

`ordered-page.py` checks a broad first-page query with an evaluation barrier on
both payload representations. `short-fragments.py` measures the proposed
one/two-character extension in a separate indexed table so its storage cost can
be counted. Production can combine these fragments with the existing array.
`short-probe.py` selects a complete small candidate list or the full ordered path
in one repeatable-read transaction. Its probe limit selects the strategy and
never truncates the search result.
`verify-edges.py` checks PostgreSQL's matching semantics with Unicode, punctuation,
wildcards, and escapes, including the extended fragment filter.

For the scope-wrap test, copy `src/modules/vault/vault_crypto.{c,h}` from commit
`1cd5ad8987013208aa35963a05a7ad6b731195aa` beside `scope-wrap.c`. Compile with
`gcc -O2 scope-wrap.c vault_crypto.c -lcrypto -o scope-wrap` and run
`./scope-wrap > results/scope-wrap.json`. On .253, OpenSSL headers were extracted
under `tools/ssl-dev` and supplied with `-I`; linking used `-l:libcrypto.so.3`.

The direct-I/O test bypasses the inner filesystem's page cache. ZFS can still
cache the backing image. These results describe that storage stack; they do not
establish cold physical-device throughput. No host caches are flushed.

## Reproduce the Optane correction

The corrected run copied the stopped databases' disk images to a separate task
ZFS dataset on the verified `optane` pool. Preserve the original raw results.
Stop both named containers and the dedicated daemon, unmount both inner
filesystems, close the mapper, and detach their verified loop devices before
copying. On .253, the fresh destination was provisioned with:

```sh
zpool status -P optane
zfs create -o compression=off -o dedup=off -o atime=off \
  -o primarycache=metadata -o secondarycache=none -o direct=always \
  -o mountpoint=/opt/aimee-encryption-bench/storage-optane \
  optane/aimee-encryption-bench
cp --sparse=always /opt/aimee-encryption-bench/storage/ordinary.img \
  /opt/aimee-encryption-bench/storage-optane/ordinary.img
cp --sparse=always /opt/aimee-encryption-bench/storage/luks.img \
  /opt/aimee-encryption-bench/storage-optane/luks.img
```

These commands describe the already completed copy; do not overwrite retained
images or rerun dataset creation against an existing dataset. Verify that the
pool resolves to the Intel SSDPE21D960GA at `/dev/nvme0n1`. The scripts use that
device's physical read counters and are specific to this host. They use
metadata-only ARC and no secondary data cache, with aligned ZFS direct I/O
requested through `direct=always`. The loop devices remain `DIO=0`: enabling
loop direct I/O failed with `EINVAL` here.

Copy the diagnostic sources to `/opt/aimee-encryption-bench`, then run there:

```sh
gcc -O2 -pthread -Wall -Wextra io-read.c -o io-read
python3 luks-diagnostic.py --storage storage-optane --output optane-diagnostic
env PYTHONPATH=/opt/aimee-encryption-bench/tools/pydep/usr/lib/python3/dist-packages \
  LD_LIBRARY_PATH=/opt/aimee-encryption-bench/tools/pydep/usr/lib/x86_64-linux-gnu \
  python3 optane-postgres.py
```

The first script requires stopped Docker and closed images. It creates or checks
matching 512 MiB fixtures, compares three dm-crypt scheduling configurations
against ordinary storage, and rejects Optane samples whose physical reads fall
below 95% of logical reads. This checks for cached results; shared-device
counters can include unrelated traffic. The script leaves both filesystems
mounted for the second script, which starts the dedicated daemon and reuses the
existing PostgreSQL containers. Each cold database sample stops its container
and unmounts/remounts its filesystem. It hashes ciphertext to force payload reads
without PGP decryption and verifies the returned count and aggregate.

Both scripts retain intermediate results and leave resources available for
inspection if a command fails. Inspect mounts and loop associations before
resuming or cleaning up. Archive each run's JSON before repeating a filename.
The second script finishes with both containers stopped. Close mounts, mapper,
loops, and the dedicated daemon using the cleanup procedure below. The corrected
loop associations are in `results/optane-diagnostic-loops.json`.

## Keep production keys out of the fixture

The drive key fixture lives in root-only tmpfs at
`/run/aimee-encryption-bench/fixture-drive.key`. PostgreSQL's fixture keys are
public deterministic values. Production retains the proposal's design: Vault
owns every key and authorizes drive unlock and payload access.

Stop the two named containers through the dedicated Docker socket when finished.
Unmount their task-owned filesystems before closing `aimee-bench-luks` and
detaching loop devices. Verify each recorded device in `results/loop-devices.txt`
still maps to its task-owned image before detaching it. Retaining the
images allows another run; reopening the LUKS fixture requires its tmpfs key.
Stop the dedicated daemon using its recorded PID. Remove only these task-owned
resources when the benchmark is no longer needed.
