#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
socket_pid=""
supervisor_pid=""
cleanup() {
    [ -z "$supervisor_pid" ] || kill "$supervisor_pid" 2>/dev/null || true
    [ -z "$socket_pid" ] || kill "$socket_pid" 2>/dev/null || true
    rm -rf "$tmp"
}
trap 'cleanup' EXIT HUP INT TERM

socket=$tmp/bus.sock
count=$tmp/count
module_pid_file=$tmp/module.pid
manifest=$tmp/server.modules
# Exercise the production Go role entry, including its immutable identity and
# required composition, without writing into the host's installed libexec path.
(cd "$root/server-go" && CGO_ENABLED=0 go build -o "$tmp/aimee-module-server" ./cmd/aimee-module)
mkdir -p "$tmp/home"
"$tmp/aimee-module-server" __aimee_instance_bootstrap "$tmp/home" >/dev/null
cat >"$tmp/idle" <<'SH'
#!/bin/sh
trap 'exit 0' TERM INT
while :; do sleep 1; done
SH
chmod 0700 "$tmp/idle"
for module in server config postgres; do
    printf '%s\t%s\n' "$module" "$tmp/idle" >>"$manifest"
done
printf 'memory\t%s\n' "$root/src/tests/support/fake_module_process.sh" >> "$manifest"

python3 -c 'import socket,sys,time
s=socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.bind(sys.argv[1])
time.sleep(30)' "$socket" &
socket_pid=$!
ticks=0
while [ ! -S "$socket" ]; do
    ticks=$((ticks + 1))
    [ "$ticks" -lt 50 ] || { echo "module-supervisor: socket setup timed out" >&2; exit 1; }
    sleep 0.1
done

AIMEE_TEST_MODULE_COUNT="$count" AIMEE_TEST_MODULE_PID="$module_pid_file" \
    "$tmp/aimee-module-server" __aimee_supervise_modules "$tmp/home" "$socket" "$manifest" &
supervisor_pid=$!

ticks=0
while :; do
    starts=0
    [ ! -r "$count" ] || read -r starts < "$count"
    [ "$starts" -ge 2 ] && break
    ticks=$((ticks + 1))
    [ "$ticks" -lt 50 ] || { echo "module-supervisor: module was not restarted" >&2; exit 1; }
    sleep 0.1
done

restarted_pid=""
read -r restarted_pid < "$module_pid_file" || true
[ -n "$restarted_pid" ] || { echo "module-supervisor: restarted module wrote no pid" >&2; exit 1; }
# The shutdown check below only proves something if this pid is the live
# restarted child. A dead pid here would make that check pass without testing
# anything, so refuse to continue on one.
kill -0 "$restarted_pid" 2>/dev/null || { echo "module-supervisor: restarted module pid $restarted_pid is not alive" >&2; exit 1; }
kill "$supervisor_pid"
wait "$supervisor_pid" 2>/dev/null || true
supervisor_pid=""
ticks=0
while kill -0 "$restarted_pid" 2>/dev/null; do
    ticks=$((ticks + 1))
    [ "$ticks" -lt 50 ] || { echo "module-supervisor: child survived shutdown" >&2; exit 1; }
    sleep 0.1
done

echo "module-supervisor: ok (crash restarted; child stopped with supervisor)"
