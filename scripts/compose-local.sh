#!/bin/sh
# Host discovery for the standard LUKS deployment. No encryption key is supplied
# here; the application Vault is its sole source.
set -eu
case "$(uname -s)" in Linux) ;; *) echo 'Local encrypted PostgreSQL requires a Linux Docker host.' >&2; exit 1 ;; esac
[ -c /dev/mapper/control ] && [ -c /dev/loop-control ] || {
    echo 'Load the host dm_mod and loop kernel modules before starting the deployment.' >&2; exit 1;
}
AIMEE_DEVICE_MAPPER_MAJOR=$(awk '$2 == "device-mapper" { print $1 }' /proc/devices)
case "$AIMEE_DEVICE_MAPPER_MAJOR" in ''|*[!0-9]*) echo 'Cannot identify the host device-mapper block device.' >&2; exit 1 ;; esac
export AIMEE_DEVICE_MAPPER_MAJOR
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 "$script_dir/compose-vault-init.py" "$@"
exec docker compose "$@"
