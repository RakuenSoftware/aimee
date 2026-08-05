#!/bin/bash
# The RTX 5080 is Blackwell (sm_120). Ubuntu 24.04 ships CUDA 12.0, which predates
# that architecture and fails the llama.cpp build with:
#
#   nvcc fatal : Unsupported gpu architecture 'compute_120a'
#
# Blackwell needs CUDA 12.8 or newer, so this installs from NVIDIA's own repo and
# rebuilds. The first build attempt hid this by sending compiler output to
# /dev/null and then reporting "build finished" seventeen seconds later, which is
# the same class of error as every other silent failure tonight: the diagnostic
# existed and nothing read it.
set -u
HOST=root@192.168.1.253
log() { echo "[$(date -u +%H:%M:%SZ)] $*"; }

log "removing the distro CUDA 12.0 toolkit"
ssh -n -o ConnectTimeout=30 $HOST "pct exec 140 -- bash -lc '
export DEBIAN_FRONTEND=noninteractive
apt-get remove -y -qq nvidia-cuda-toolkit >/dev/null 2>&1; echo removed'" 2>&1 | tail -1

log "adding the NVIDIA CUDA repo for ubuntu 24.04"
ssh -n -o ConnectTimeout=120 $HOST "pct exec 140 -- bash -lc '
export DEBIAN_FRONTEND=noninteractive
cd /tmp
curl -fsSLO https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
dpkg -i cuda-keyring_1.1-1_all.deb >/dev/null 2>&1
apt-get update -qq >/dev/null 2>&1
echo repo-added'" 2>&1 | tail -1

log "cuda-toolkit packages now visible:"
ssh -n -o ConnectTimeout=60 $HOST "pct exec 140 -- bash -lc 'apt-cache search \"^cuda-toolkit-1[23]-\" | sort -V | tail -5'" 2>&1 | tail -5

# Pick the real meta-package. Sorting the whole match and taking the last gives
# cuda-toolkit-13-config-common, a config stub that sorts AFTER cuda-toolkit-13-3,
# installs cleanly, and contains no nvcc. The install then reports success and the
# build fails later with an empty compiler path.
PKG=$(ssh -n -o ConnectTimeout=60 $HOST "pct exec 140 -- bash -lc 'apt-cache search \"^cuda-toolkit-[0-9]+-[0-9]+\$\" | awk \"{print \\\$1}\" | grep -E \"^cuda-toolkit-[0-9]+-[0-9]+\$\" | sort -V | tail -1'" 2>/dev/null | tr -d '\r' | tr -d ' ')
[ -z "$PKG" ] && PKG=cuda-toolkit-13-3
log "installing $PKG (large download, no timeout applied)"
ssh -n -o ConnectTimeout=900 $HOST "pct exec 140 -- bash -lc '
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -qq $PKG >/dev/null 2>&1 && echo toolkit-ok || echo toolkit-FAILED'" 2>&1 | tail -1

log "nvcc version now:"
ssh -n -o ConnectTimeout=60 $HOST "pct exec 140 -- bash -lc 'ls -d /usr/local/cuda-*/bin/nvcc 2>/dev/null | sort -V | tail -1 | xargs -r -I{} {} --version | tail -1'" 2>&1 | tail -1

log "rebuilding llama.cpp; compiler output kept in /tmp/build.log this time"
ssh -n -o ConnectTimeout=1800 $HOST "pct exec 140 -- bash -lc '
NVCC=\$(ls -d /usr/local/cuda-*/bin/nvcc 2>/dev/null | sort -V | tail -1)
export PATH=\$(dirname \$NVCC):\$PATH
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -qq libssl-dev >/dev/null 2>&1 || true
cd /opt/llama.cpp
rm -rf build-cuda
cmake -B build-cuda -DGGML_CUDA=ON -DLLAMA_CURL=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER=\$NVCC > /tmp/cfg.log 2>&1
cmake --build build-cuda --config Release -j 16 --target llama-server > /tmp/build.log 2>&1
if [ -x build-cuda/bin/llama-server ]; then echo BUILD-OK; else echo BUILD-FAILED; tail -15 /tmp/build.log; fi'" 2>&1 | tail -6

log "ct140 cuda rebuild finished"
