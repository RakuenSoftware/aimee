#!/usr/bin/env bash
# .253 benchmark host only: format fresh task-owned disk-image files, never host disks.
set -euo pipefail
ROOT=/opt/aimee-encryption-bench
DOCKER="$ROOT/tools/docker/docker"
export DOCKER_HOST=unix:///run/aimee-encryption-bench/docker.sock
CRYPT="$ROOT/tools/cryptsetup/usr/sbin/cryptsetup"
[[ $(id -u) == 0 && $(hostname) == pve ]]
[[ ! -e "$ROOT/storage/ordinary.img" && ! -e "$ROOT/storage/luks.img" ]]
zfs create -o compression=off -o atime=off -o mountpoint="$ROOT/storage" rpool/aimee-encryption-bench
mkdir -p "$ROOT"/{mounts,sockets}/{ordinary,luks} "$ROOT/results"
truncate -s 12G "$ROOT/storage/ordinary.img"
truncate -s 12G "$ROOT/storage/luks.img"
PLAIN_LOOP=$(losetup --find --show --offset 16777216 "$ROOT/storage/ordinary.img")
CRYPT_LOOP=$(losetup --find --show "$ROOT/storage/luks.img")
umask 077
head -c 64 /dev/urandom > /run/aimee-encryption-bench/fixture-drive.key
"$CRYPT" luksFormat --type luks2 --batch-mode --cipher aes-xts-plain64 --key-size 512 \
    --pbkdf argon2id --pbkdf-memory 65536 --pbkdf-parallel 2 --iter-time 100 \
    --key-file /run/aimee-encryption-bench/fixture-drive.key "$CRYPT_LOOP"
"$CRYPT" open --key-file /run/aimee-encryption-bench/fixture-drive.key "$CRYPT_LOOP" aimee-bench-luks
mkfs.ext4 -q -F -E lazy_itable_init=0,lazy_journal_init=0 "$PLAIN_LOOP"
mkfs.ext4 -q -F -E lazy_itable_init=0,lazy_journal_init=0 /dev/mapper/aimee-bench-luks
mount -o noatime "$PLAIN_LOOP" "$ROOT/mounts/ordinary"
mount -o noatime /dev/mapper/aimee-bench-luks "$ROOT/mounts/luks"
printf '%s\n' "$PLAIN_LOOP" "$CRYPT_LOOP" > "$ROOT/results/loop-devices.txt"
umask 022
chmod 755 "$ROOT" "$ROOT/sockets"
chmod 1777 "$ROOT/sockets/ordinary" "$ROOT/sockets/luks"
IMAGE=$("$DOCKER" image inspect postgres:18 --format '{{index .RepoDigests 0}}')
apparmor_parser -r "$ROOT/apparmor.profile"
printf '%s\n' "$IMAGE" > "$ROOT/results/postgres-image.txt"
for MODE in ordinary luks; do
    "$DOCKER" run -d --name "aimee-encryption-$MODE" --network none --cpus 4 \
        --security-opt apparmor=aimee-encryption-benchmark \
        --memory 6g --memory-swap 6g --shm-size 1g --restart no \
        -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=bench \
        -e POSTGRES_INITDB_ARGS='--locale=C.UTF-8 --encoding=UTF8 --data-checksums' \
        --mount "type=bind,src=$ROOT/mounts/$MODE,dst=/var/lib/postgresql" \
        --mount "type=bind,src=$ROOT/sockets/$MODE,dst=/var/run/postgresql" \
        "$IMAGE" postgres -c shared_buffers=512MB -c work_mem=64MB \
        -c maintenance_work_mem=512MB -c jit=off -c max_parallel_workers_per_gather=0 \
        -c max_wal_size=4GB -c checkpoint_timeout=30min -c track_io_timing=on \
        -c log_statement=none -c log_parameter_max_length=0 \
        -c log_parameter_max_length_on_error=0 -c autovacuum=off
done
"$CRYPT" status aimee-bench-luks > "$ROOT/results/luks-status.txt"
"$CRYPT" luksDump "$CRYPT_LOOP" > "$ROOT/results/luks-header-metadata.txt"
"$DOCKER" version > "$ROOT/results/docker-version.txt"
"$DOCKER" inspect aimee-encryption-ordinary aimee-encryption-luks > "$ROOT/results/containers.json"
{
    date -u --iso-8601=seconds
    uname -a
    lscpu
    free -h
    uptime
    zfs get compression,recordsize,atime rpool/aimee-encryption-bench
    findmnt "$ROOT/mounts/ordinary"
    findmnt "$ROOT/mounts/luks"
    lsblk -o NAME,TYPE,SIZE,MODEL
} > "$ROOT/results/host.txt"
