#!/usr/bin/env bash
# Provision one fresh LXC guest and one fresh VM on .252, run the installed
# SQLite WORM acceptance suite in both, then remove every created resource.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${HOST:-root@192.168.1.252}"
CTID="${CTID:-9127}"
VMID="${VMID:-9128}"
TAG="worm-$(date -u +%Y%m%dT%H%M%SZ)-$$"
REMOTE_DIR="/tmp/aimee-$TAG"
LOCAL_STAGE="$(mktemp -d)"
CT_OWNED=0
VM_OWNED=0

cleanup() {
  set +e
  if [[ "$VM_OWNED" = 1 ]]; then
    ssh "$HOST" "qm stop '$VMID' >/dev/null 2>&1 || true; qm destroy '$VMID' --purge >/dev/null 2>&1 || true"
  fi
  if [[ "$CT_OWNED" = 1 ]]; then
    ssh "$HOST" "pct stop '$CTID' >/dev/null 2>&1 || true; pct destroy '$CTID' --force >/dev/null 2>&1 || true"
  fi
  ssh "$HOST" "rm -rf -- '$REMOTE_DIR'" >/dev/null 2>&1 || true
  rm -rf -- "$LOCAL_STAGE"
}
trap cleanup EXIT HUP INT TERM

for id in "$CTID" "$VMID"; do
  if ssh "$HOST" "pct config '$id' >/dev/null 2>&1 || qm config '$id' >/dev/null 2>&1"; then
    echo "refusing to use occupied guest ID $id" >&2
    exit 1
  fi
done

make -C "$ROOT/src" ../aimee-server ../aimee-kb ../aimee-kb-worm \
  build/obj/aimee-module-config -j"$(nproc)"
python3 "$ROOT/scripts/export_c_repositories.py" \
  --runtime-bundle "$LOCAL_STAGE/module-runtime" >/dev/null
tar -C "$ROOT" -czf "$LOCAL_STAGE/payload.tgz" \
  aimee-server aimee-kb aimee-kb-worm \
  scripts/run-worm-worker-pg-test.sh scripts/validate-worm-fresh-guest.sh \
  src/modules/db2/c/schema_roles.sql src/modules/db2/c/schema.sql \
  src/modules/db2/c/schema_grants.sql src/build/obj/aimee-module-config \
  -C "$LOCAL_STAGE" module-runtime
cp /home/virant/.ssh/id_ed25519.pub "$LOCAL_STAGE/validation.pub"
ssh "$HOST" "install -d -m 0700 '$REMOTE_DIR'"
scp -q "$LOCAL_STAGE/payload.tgz" "$LOCAL_STAGE/validation.pub" "$HOST:$REMOTE_DIR/"

echo "== creating fresh CT $CTID =="
CT_OWNED=1
ssh "$HOST" "pct create '$CTID' local:vztmpl/debian-13-standard_13.6-1_amd64.tar.zst \
  --hostname aimee-worm-ct --cores 4 --memory 6144 --swap 512 \
  --rootfs local-lvm:24 --net0 name=eth0,bridge=vmbr0,ip=dhcp \
  --unprivileged 1 --features nesting=1 --onboot 0 --start 1"
ssh "$HOST" "aimee-keepalive 'ct:$CTID' >/dev/null"
ssh "$HOST" "pct exec '$CTID' -- bash -lc 'export DEBIAN_FRONTEND=noninteractive; \
  { apt-get update -qq; apt-get install -y -qq postgresql postgresql-17-pgvector \
  libpq5 libsqlite3-0 sqlite3 libssl3 libzstd1 zlib1g libpam0g curl jq ca-certificates; \
  systemctl enable --now postgresql; } >/tmp/aimee-provision.log 2>&1 || \
  { tail -100 /tmp/aimee-provision.log; exit 1; }'"
ssh "$HOST" "pct push '$CTID' '$REMOTE_DIR/payload.tgz' /tmp/payload.tgz --perms 0600; \
  pct exec '$CTID' -- install -d -m 0755 /opt/aimee-worm-validation; \
  pct exec '$CTID' -- tar -C /opt/aimee-worm-validation -xzf /tmp/payload.tgz"
echo "== validating fresh CT $CTID =="
ssh "$HOST" "timeout 900 pct exec '$CTID' -- bash /opt/aimee-worm-validation/scripts/validate-worm-fresh-guest.sh"

echo "== creating fresh VM $VMID =="
VM_OWNED=1
ssh "$HOST" "qm create '$VMID' --name aimee-worm-vm --cores 4 --memory 6144 \
  --net0 virtio,bridge=vmbr0 --scsihw virtio-scsi-pci --agent enabled=1 \
  --serial0 socket --vga serial0 --ostype l26; \
  qm importdisk '$VMID' /var/lib/vz/template/iso/debian-13-genericcloud-amd64.qcow2 local-lvm; \
  qm set '$VMID' --scsi0 local-lvm:vm-'$VMID'-disk-0,discard=on,ssd=1 \
    --ide2 local-lvm:cloudinit --boot order=scsi0 --ciuser root \
    --sshkeys '$REMOTE_DIR/validation.pub' --ipconfig0 ip=dhcp --ciupgrade 0; \
  qm resize '$VMID' scsi0 24G; qm start '$VMID'; aimee-keepalive 'vm:$VMID' >/dev/null"

VM_IP=
VM_MAC="$(ssh "$HOST" "qm config '$VMID'" |
  sed -n 's/^net0:.*virtio=\([^,]*\).*/\1/p')"
VM_LINK_LOCAL="$(python3 - "$VM_MAC" <<'PY'
import sys

octets = [int(part, 16) for part in sys.argv[1].split(":")]
if len(octets) != 6:
    raise SystemExit("invalid VM MAC address")
octets[0] ^= 0x02
eui64 = octets[:3] + [0xff, 0xfe] + octets[3:]
groups = [(eui64[i] << 8) | eui64[i + 1] for i in range(0, 8, 2)]
print("fe80::" + ":".join(f"{group:x}" for group in groups))
PY
)"
for _ in $(seq 1 180); do
  VM_IP="$(ssh "$HOST" "qm guest cmd '$VMID' network-get-interfaces 2>/dev/null" |
    python3 -c 'import json,sys
try: data=json.load(sys.stdin)
except Exception: raise SystemExit
for iface in data:
  for addr in iface.get("ip-addresses",[]):
    ip=addr.get("ip-address","")
    if addr.get("ip-address-type")=="ipv4" and not ip.startswith("127."):
      print(ip); raise SystemExit' 2>/dev/null || true)"
  if [[ -z "$VM_IP" ]]; then
    # Debian's generic cloud image does not initially include qemu-guest-agent.
    # Reach its deterministic EUI-64 link-local address through the Proxmox host
    # and ask the guest for the DHCP address without requiring the agent.
    VM_IP="$(ssh -6 -J "$HOST" -o BatchMode=yes -o ConnectTimeout=2 \
      -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      "root@$VM_LINK_LOCAL%vmbr0" 'ip -4 -o addr show scope global' 2>/dev/null |
      awk '{split($4, address, "/"); print address[1]; exit}' || true)"
  fi
  [[ -n "$VM_IP" ]] && break
  sleep 2
done
[[ -n "$VM_IP" ]] || { echo "could not discover VM $VMID IP" >&2; exit 1; }

SSH_VM=(ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@"$VM_IP")
SCP_VM=(scp -q -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
for _ in $(seq 1 120); do
  "${SSH_VM[@]}" true >/dev/null 2>&1 && break
  sleep 2
done
"${SSH_VM[@]}" true
"${SSH_VM[@]}" "export DEBIAN_FRONTEND=noninteractive; { apt-get update -qq; \
  apt-get install -y -qq postgresql postgresql-17-pgvector libpq5 libsqlite3-0 sqlite3 \
  libssl3 libzstd1 zlib1g libpam0g curl jq ca-certificates qemu-guest-agent; \
  systemctl enable --now postgresql qemu-guest-agent; \
  install -d -m 0755 /opt/aimee-worm-validation; } >/tmp/aimee-provision.log 2>&1 || \
  { tail -100 /tmp/aimee-provision.log; exit 1; }"
"${SCP_VM[@]}" "$LOCAL_STAGE/payload.tgz" root@"$VM_IP":/tmp/payload.tgz
"${SSH_VM[@]}" "tar -C /opt/aimee-worm-validation -xzf /tmp/payload.tgz"
echo "== validating fresh VM $VMID ($VM_IP) =="
"${SSH_VM[@]}" "timeout 900 bash /opt/aimee-worm-validation/scripts/validate-worm-fresh-guest.sh"

echo "== validation passed in CT $CTID and VM $VMID; cleanup follows =="
