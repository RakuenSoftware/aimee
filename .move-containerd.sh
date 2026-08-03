#!/bin/sh
# Move the containerd image store onto the big pool.
#
# Docker 29 stores images via containerd, so `data-root` in daemon.json moves
# Docker's own directory and NOT the image content — which is why the pool showed
# 28MB while / filled to 99%. The image store is /var/lib/containerd.
#
# Moved and symlinked rather than reconfigured: one less config file to be wrong,
# and it survives a containerd package update that would rewrite config.toml.
set -eu

echo "before: $(df -h / | tail -1 | awk '{print $4}') free on /, $(df -h /var/lib/docker-pool | tail -1 | awk '{print $4}') free on pool"

systemctl stop aimee-matrix 2>/dev/null || true
pkill -f matrix-test.sh 2>/dev/null || true
sleep 2
systemctl stop docker docker.socket 2>/dev/null || true
systemctl stop containerd 2>/dev/null || true
sleep 3

if [ -L /var/lib/containerd ]; then
  echo "already a symlink -> $(readlink /var/lib/containerd)"
else
  mkdir -p /var/lib/docker-pool/containerd
  echo "moving image store (this takes a few minutes)..."
  # -a preserves the hardlinks/xattrs the snapshotter relies on.
  cp -a /var/lib/containerd/. /var/lib/docker-pool/containerd/
  rm -rf /var/lib/containerd
  ln -s /var/lib/docker-pool/containerd /var/lib/containerd
fi

# Docker's own dir is small but move it too, so nothing lands on / later.
if [ ! -L /var/lib/docker ]; then
  mkdir -p /var/lib/docker-pool/docker-lib
  cp -a /var/lib/docker/. /var/lib/docker-pool/docker-lib/ 2>/dev/null || true
  rm -rf /var/lib/docker
  ln -s /var/lib/docker-pool/docker-lib /var/lib/docker
fi

systemctl start containerd
sleep 3
systemctl start docker
sleep 6

echo "after:  $(df -h / | tail -1 | awk '{print $4}') free on /, $(df -h /var/lib/docker-pool | tail -1 | awk '{print $4}') free on pool"
docker info --format 'data-root={{.DockerRootDir}} images={{.Images}}' 2>/dev/null
docker images --format '{{.Repository}}:{{.Tag}}' 2>/dev/null | grep test || echo "(no test images — cache lost)"
