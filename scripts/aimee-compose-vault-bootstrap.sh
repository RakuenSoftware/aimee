#!/bin/sh
# Seal first-boot host environment credentials into the server/KB Vault without
# ever placing their values in a long-lived container's Config.Env.
set -eu

usage() {
    printf 'usage: %s [-p PROJECT] [-e ENV_FILE] -f COMPOSE_FILE {server|kb|all}\n' "$0" >&2
    exit 2
}

compose_file=
project=
# Compose reads `.env` from the PROJECT directory, which is the directory holding
# the first -f file -- so `-f deploy/compose/aimee.yaml` looks in deploy/compose/
# and never sees the repository-root .env. Once a profile declares a required
# variable with no default, every compose call here fails at interpolation before
# it can seal anything. -e forwards the same --env-file the operator used for
# `up`; the repo-root .env is picked up automatically when it exists, so the
# common case needs no flag.
env_file=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -f|--file)
            [ "$#" -ge 2 ] || usage
            compose_file=$2
            shift 2
            ;;
        --)
            shift
            break
            ;;
        -p|--project)
            [ "$#" -ge 2 ] || usage
            project=$2
            shift 2
            ;;
        -e|--env-file)
            [ "$#" -ge 2 ] || usage
            env_file=$2
            shift 2
            ;;
        -*) usage ;;
        *) break ;;
    esac
done
[ -n "$compose_file" ] && [ -r "$compose_file" ] || usage
if [ -z "$env_file" ]; then
    repo_env=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/.env
    [ -r "$repo_env" ] && env_file=$repo_env
fi
[ -z "$env_file" ] || [ -r "$env_file" ] || {
    printf '%s: cannot read env file %s\n' "$0" "$env_file" >&2
    exit 2
}
[ "$#" -eq 1 ] || usage
target=$1

# Seal into the project the deployment ACTUALLY uses.
#
# `docker compose -f FILE` derives the project name from the directory holding
# FILE, not from where the operator runs the stack. A deployment brought up as
# project "aimee" from a file under deploy/container/ is project "container"
# here — so the credential is sealed into an orphan volume the long-lived
# service never reads, and this script still prints "Vault bootstrap complete".
# That happened: a kb was left with no bearer after a seal that reported success.
#
# If a project is already running this compose file, use it. Otherwise fall back
# to compose's own default, which is correct for a first install (the stack is
# started the same way straight after). An explicit -p always wins, and
# COMPOSE_PROJECT_NAME is honoured by compose itself.
if [ -z "$project" ] && [ -z "${COMPOSE_PROJECT_NAME:-}" ]; then
    running=$(docker compose -f "$compose_file" ls --format json 2>/dev/null \
        | tr ',' '\n' | sed -n 's/.*"Name":"\([^"]*\)".*/\1/p' | sort -u)
    count=$(printf '%s' "$running" | grep -c . || true)
    if [ "$count" -gt 1 ]; then
        printf 'error: several compose projects use %s:\n%s\nre-run with -p PROJECT.\n' \
            "$compose_file" "$running" >&2
        exit 2
    fi
    [ "$count" -eq 1 ] && project=$running
fi

compose() {
    set -- -f "$compose_file" "$@"
    [ -z "$env_file" ] || set -- --env-file "$env_file" "$@"
    [ -z "$project" ] || set -- -p "$project" "$@"
    docker compose "$@"
}

announce_project() {
    if [ -n "$project" ]; then
        printf 'Sealing into compose project %s (verify: docker compose -p %s ps).\n' \
            "$project" "$project"
    else
        printf '%s\n' 'Sealing into the default compose project for this file (no running project found).'
    fi
}

bootstrap_server() {
    announce_project
    printf '%s\n' 'Sealing server first-boot credentials into Vault (values are not logged or stored in container metadata).'
    env -0 | compose run --rm --no-deps -T \
        --entrypoint /usr/sbin/runuser aimee-server \
        -u aimee -- /usr/local/bin/aimee-server --bootstrap-vault-stdin
}

bootstrap_kb() {
    announce_project
    printf '%s\n' 'Sealing KB first-boot credentials into Vault (values are not logged or stored in container metadata).'
    env -0 | compose run --rm --no-deps -T \
        --entrypoint /usr/local/bin/aimee-kb aimee-kb \
        --bootstrap-vault-stdin
}

case "$target" in
    server) bootstrap_server ;;
    kb) bootstrap_kb ;;
    all)
        bootstrap_kb
        bootstrap_server
        ;;
    *) usage ;;
esac

printf '%s\n' 'Vault bootstrap complete. Unset the host credential variables, then start the long-lived services normally.'
