#!/bin/bash
# Rebuild the 5080's serving container from scratch.
#
# LXC 140 disappeared during a host load fix and there is no backup. This
# recreates it: Ubuntu 24.04, GPU passthrough for the RTX 5080, CUDA toolkit,
# llama.cpp built with CUDA, at the same paths the harness already expects
# (/opt/llama.cpp/build-cuda/bin/llama-server, HF_HOME=/opt/hf).
#
# NOT on root storage, per instruction: rootfs goes on `storage` (lvmthin,
# 3.8 TB, empty). Models are 6-21 GiB each and the HF cache grows without bound,
# which is what makes 300G the right size rather than generous.
#
# Passthrough is by device cgroup allow plus explicit dev mounts. The container
# is unprivileged=0 because the nvidia character devices need it; this box is a
# private benchmark host and that tradeoff is deliberate.
set -u
HOST=root@192.168.1.253
CTID=140
log() { echo "[$(date -u +%H:%M:%SZ)] $*"; }

log "creating CT $CTID on storage (300G)"
ssh -n -o ConnectTimeout=20 $HOST "pct create $CTID \
  /var/lib/vz/template/cache/ubuntu-24.04-standard_24.04-2_amd64.tar.zst \
  --hostname tierA-5080 --storage storage --rootfs storage:300 \
  --cores 16 --memory 32768 --swap 4096 \
  --net0 name=eth0,bridge=vmbr0,ip=dhcp \
  --unprivileged 0 --features nesting=1 --onboot 1" 2>&1 | tail -3

log "adding GPU passthrough to the container config"
ssh -n -o ConnectTimeout=20 $HOST "cat >> /etc/pve/lxc/$CTID.conf <<'EOF'
lxc.cgroup2.devices.allow: c 195:* rwm
lxc.cgroup2.devices.allow: c 511:* rwm
lxc.cgroup2.devices.allow: c 236:* rwm
lxc.mount.entry: /dev/nvidia0 dev/nvidia0 none bind,optional,create=file
lxc.mount.entry: /dev/nvidiactl dev/nvidiactl none bind,optional,create=file
lxc.mount.entry: /dev/nvidia-modeset dev/nvidia-modeset none bind,optional,create=file
lxc.mount.entry: /dev/nvidia-uvm dev/nvidia-uvm none bind,optional,create=file
lxc.mount.entry: /dev/nvidia-uvm-tools dev/nvidia-uvm-tools none bind,optional,create=file
EOF" 2>&1 | tail -2

log "starting CT"
ssh -n -o ConnectTimeout=20 $HOST "pct start $CTID; sleep 12; pct status $CTID" 2>&1 | tail -2

log "installing base packages"
ssh -n -o ConnectTimeout=30 $HOST "pct exec $CTID -- bash -lc '
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq curl git build-essential cmake python3 python3-pip libcurl4-openssl-dev pciutils >/dev/null 2>&1
echo base-ok'" 2>&1 | tail -2

log "installing the nvidia userspace matching the host driver"
DRV=$(ssh -n -o ConnectTimeout=20 $HOST "nvidia-smi --query-gpu=driver_version --format=csv,noheader" 2>/dev/null | tr -d ' ')
log "  host driver: $DRV"
ssh -n -o ConnectTimeout=60 $HOST "pct exec $CTID -- bash -lc '
export DEBIAN_FRONTEND=noninteractive
cd /tmp
curl -fsSL -o nv.run https://us.download.nvidia.com/XFree86/Linux-x86_64/$DRV/NVIDIA-Linux-x86_64-$DRV.run 2>/dev/null \
  && sh nv.run --no-kernel-module --silent >/dev/null 2>&1 && echo nvidia-userspace-ok || echo nvidia-userspace-FAILED'" 2>&1 | tail -2

log "verifying the GPU is visible inside the container"
ssh -n -o ConnectTimeout=30 $HOST "pct exec $CTID -- bash -lc 'nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>&1 | head -2'" 2>&1 | tail -2

log "installing the CUDA toolkit (nvcc) for the llama.cpp build"
ssh -n -o ConnectTimeout=120 $HOST "pct exec $CTID -- bash -lc '
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -qq nvidia-cuda-toolkit >/dev/null 2>&1 && echo cuda-toolkit-ok || echo cuda-toolkit-FAILED'" 2>&1 | tail -2

log "building llama.cpp with CUDA (this is the long step)"
ssh -n -o ConnectTimeout=120 $HOST "pct exec $CTID -- bash -lc '
set -e
mkdir -p /opt/hf /opt/tierA
cd /opt && rm -rf llama.cpp && git clone --depth 1 https://github.com/ggml-org/llama.cpp >/dev/null 2>&1
cd llama.cpp
cmake -B build-cuda -DGGML_CUDA=ON -DLLAMA_CURL=ON -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
cmake --build build-cuda --config Release -j 16 --target llama-server >/dev/null 2>&1
ls -la build-cuda/bin/llama-server && echo build-ok'" 2>&1 | tail -3

log "CT $CTID build finished"
