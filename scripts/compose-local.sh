#!/bin/sh
# Seal startup credentials, then use the selected Docker context. Ordinary
# storage needs no device discovery on the machine invoking the Docker CLI.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 "$script_dir/compose-vault-init.py" "$@"
exec docker compose "$@"
