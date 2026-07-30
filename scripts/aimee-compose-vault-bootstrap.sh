#!/bin/sh
# Seal first-boot host environment credentials into the server/KB Vault without
# ever placing their values in a long-lived container's Config.Env.
set -eu

usage() {
    printf 'usage: %s -f COMPOSE_FILE {server|kb|all}\n' "$0" >&2
    exit 2
}

compose_file=
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
        -*) usage ;;
        *) break ;;
    esac
done
[ -n "$compose_file" ] && [ -r "$compose_file" ] || usage
[ "$#" -eq 1 ] || usage
target=$1

compose() {
    docker compose -f "$compose_file" "$@"
}

bootstrap_server() {
    printf '%s\n' 'Sealing server first-boot credentials into Vault (values are not logged or stored in container metadata).'
    env -0 | compose run --rm --no-deps -T \
        --entrypoint /usr/sbin/runuser aimee-server \
        -u aimee -- /usr/local/bin/aimee-server --bootstrap-vault-stdin
}

bootstrap_kb() {
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
