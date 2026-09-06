#!/usr/bin/env bash
# Fresh CT + VM acceptance, with ownership-checked cleanup on success/failure.
# Does not update host packages or touch existing guests. SSH host verification
# remains enabled; only the newly created VM uses a run-private TOFU key file.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
HOST=${HOST:-root@192.168.1.253}
CTID=${CTID:-9201}
VMID=${VMID:-9202}
STORAGE=${STORAGE:-optane}
TEMPLATE=${TEMPLATE:-local:vztmpl/debian-13-standard_13.1-2_amd64.tar.zst}
VM_IMAGE=${VM_IMAGE:-/var/lib/vz/template/iso/debian-13-genericcloud-amd64.qcow2}
PUBLIC_KEY=${PUBLIC_KEY:-/home/virant/.ssh/id_ed25519.pub}
[[ $CTID =~ ^[1-9][0-9]{2,8}$ && $VMID =~ ^[1-9][0-9]{2,8}$ && $CTID != "$VMID" ]]
[[ $STORAGE =~ ^[a-zA-Z0-9_-]+$ ]]
[[ $TEMPLATE =~ ^[a-zA-Z0-9_./:-]+$ && $VM_IMAGE =~ ^/[a-zA-Z0-9_./-]+$ ]]
[[ -f $PUBLIC_KEY ]]
STAGE=$(mktemp -d /tmp/aimee-proxy-validation.XXXXXXXX)
TAG=${STAGE##*.}
CT_NAME=aimee-proxy-ct-$TAG
VM_NAME=aimee-proxy-vm-$TAG
REMOTE_DIR=/tmp/aimee-proxy-validation-$TAG
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 "$HOST")
CT_ATTEMPTED=0
VM_ATTEMPTED=0
REMOTE_CREATED=0
cleanup() {
  local result=$? cleanup_failed=0
  trap - EXIT HUP INT TERM
  set +e
  if [[ $VM_ATTEMPTED == 1 ]]; then
    "${SSH[@]}" "if qm config '$VMID' 2>/dev/null | grep -qx 'name: $VM_NAME'; then
      qm stop '$VMID' && qm destroy '$VMID' --purge; fi
      ! test -e '/etc/pve/qemu-server/$VMID.conf'" || cleanup_failed=1
  fi
  if [[ $CT_ATTEMPTED == 1 ]]; then
    "${SSH[@]}" "if pct config '$CTID' 2>/dev/null | grep -qx 'hostname: $CT_NAME'; then
      pct stop '$CTID' && pct destroy '$CTID'; fi
      ! test -e '/etc/pve/lxc/$CTID.conf'" || cleanup_failed=1
  fi
  if [[ $REMOTE_CREATED == 1 ]]; then
    "${SSH[@]}" "rm -rf -- '$REMOTE_DIR'" || cleanup_failed=1
  fi
  if [[ $cleanup_failed == 0 ]]; then
    rm -rf -- "$STAGE"
    echo "Cleanup verified for this run's created guests and temporary payload."
  else
    echo "ERROR: cleanup incomplete; inspect CT $CTID, VM $VMID and $REMOTE_DIR; local stage $STAGE retained." >&2
    result=1
  fi
  exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
for id in "$CTID" "$VMID"; do
  "${SSH[@]}" "test ! -e /etc/pve/lxc/$id.conf && test ! -e /etc/pve/qemu-server/$id.conf" || {
    echo "Refusing occupied guest ID $id" >&2; exit 1;
  }
done
make -s -C "$REPO/src" -j4 ../aimee build/obj/tests/unit-test-openai-shape \
  build/obj/tests/unit-test-cli-profile build/obj/tests/unit-test-server-dispatch \
  build/obj/tests/unit-test-guardrails build/obj/tests/unit-test-util \
  build/obj/tests/unit-test-agent build/obj/aimee-module
tar -C "$REPO" -czf "$STAGE/payload.tgz" aimee \
  scripts/validate-proxy-fresh-guest.sh scripts/tests/test_thin_client_proxy.py \
  skills \
  --transform='s|^src/build/obj/tests/||' src/build/obj/tests/unit-test-openai-shape \
  src/build/obj/tests/unit-test-cli-profile src/build/obj/tests/unit-test-server-dispatch \
  src/build/obj/tests/unit-test-guardrails src/build/obj/tests/unit-test-util \
  src/build/obj/tests/unit-test-agent \
  --transform='s|^src/build/obj/||' src/build/obj/aimee-module
"${SSH[@]}" "mkdir -m 0700 '$REMOTE_DIR'"
REMOTE_CREATED=1
scp -q "$STAGE/payload.tgz" "$HOST:$REMOTE_DIR/payload.tgz"
scp -q "$PUBLIC_KEY" "$HOST:$REMOTE_DIR/validation.pub"

echo "Creating fresh CT $CTID ($CT_NAME)"
CT_ATTEMPTED=1
"${SSH[@]}" "pct create '$CTID' '$TEMPLATE' --hostname '$CT_NAME' \
  --cores 2 --memory 2048 --swap 512 --rootfs '$STORAGE:8' \
  --net0 name=eth0,bridge=vmbr0,ip=dhcp --unprivileged 1 --onboot 0 --start 1"
"${SSH[@]}" "pct push '$CTID' '$REMOTE_DIR/payload.tgz' /tmp/aimee-proxy-payload.tgz --perms 0600 &&
  pct exec '$CTID' -- mkdir -p /opt/aimee-proxy-validation &&
  pct exec '$CTID' -- tar -C /opt/aimee-proxy-validation -xzf /tmp/aimee-proxy-payload.tgz &&
  pct exec '$CTID' -- bash /opt/aimee-proxy-validation/scripts/validate-proxy-fresh-guest.sh"

echo "Creating fresh VM $VMID ($VM_NAME)"
VM_ATTEMPTED=1
"${SSH[@]}" "qm create '$VMID' --name '$VM_NAME' --cores 2 --memory 2048 \
  --net0 virtio,bridge=vmbr0 --scsihw virtio-scsi-pci --serial0 socket --vga serial0 --ostype l26 &&
  qm importdisk '$VMID' '$VM_IMAGE' '$STORAGE'"
DISK=$("${SSH[@]}" "qm config '$VMID'" | sed -n 's/^unused0: \([^,]*\).*/\1/p')
[[ $DISK =~ ^[a-zA-Z0-9_:/.-]+$ ]]
"${SSH[@]}" "qm set '$VMID' --scsi0 '$DISK' --ide2 '$STORAGE:cloudinit' \
  --boot order=scsi0 --ciuser root --sshkeys '$REMOTE_DIR/validation.pub' \
  --ipconfig0 ip=dhcp --ciupgrade 0 && qm resize '$VMID' scsi0 8G && qm start '$VMID'"
MAC=$("${SSH[@]}" "qm config '$VMID'" | sed -n 's/^net0:.*virtio=\([^,]*\).*/\1/p')
LINK_LOCAL=$(python3 - "$MAC" <<'PY'
import sys
b = [int(x, 16) for x in sys.argv[1].split(':')]
assert len(b) == 6
b[0] ^= 2
b = b[:3] + [255, 254] + b[3:]
print('fe80::' + ':'.join(f'{b[i]*256+b[i+1]:x}' for i in range(0, 8, 2)))
PY
)
VM_SSH=(ssh -6 -J "$HOST" -o BatchMode=yes -o ConnectTimeout=3 \
  -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="$STAGE/known_hosts" \
  "root@$LINK_LOCAL%vmbr0")
ready=0
for ((attempt=0; attempt<90; attempt++)); do
  if "${VM_SSH[@]}" true 2>/dev/null; then ready=1; break; fi
  sleep 2
done
[[ $ready == 1 ]] || { echo "VM SSH readiness timed out" >&2; exit 1; }
"${VM_SSH[@]}" 'mkdir -p /opt/aimee-proxy-validation && tar -C /opt/aimee-proxy-validation -xzf -' < "$STAGE/payload.tgz"
"${VM_SSH[@]}" 'bash /opt/aimee-proxy-validation/scripts/validate-proxy-fresh-guest.sh'
echo 'Fresh CT and VM proxy validation passed; cleanup follows.'
