#!/bin/sh
# Finish the containerd move: the rm failed only on stale tmpmounts left behind by
# an interrupted build. Unmount them, drop the husk, symlink, restart.
#
# tmpmounts are containerd's scratch bind-mounts for layer extraction. cp -a
# FOLLOWED them, so the pool copy also carries their mounted contents as real
# files — junk that is removed here, which is why the pool shows more than the
# original store did.
set -u

echo "=== unmounting stale tmpmounts ==="
mount | grep -oE '/var/lib/containerd/tmpmounts/containerd-mount[0-9]*' | sort -u | while read -r m; do
  umount -l "$m" 2>/dev/null && echo "  unmounted $m" || echo "  could not unmount $m"
done
sleep 2

echo "=== removing the husk on / ==="
rm -rf /var/lib/containerd 2>/dev/null
if [ -e /var/lib/containerd ] && [ ! -L /var/lib/containerd ]; then
  echo "  still present:"; ls -A /var/lib/containerd | head -5
  exit 3
fi

echo "=== dropping copied tmpmount junk from the pool ==="
rm -rf /var/lib/docker-pool/containerd/tmpmounts 2>/dev/null
mkdir -p /var/lib/docker-pool/containerd/tmpmounts

echo "=== symlink ==="
ln -s /var/lib/docker-pool/containerd /var/lib/containerd
ls -ld /var/lib/containerd

echo "=== start services ==="
systemctl start containerd; sleep 4
systemctl start docker; sleep 8
systemctl is-active containerd docker | tr '\n' ' '; echo

echo "=== verify the image store survived ==="
docker info --format 'data-root={{.DockerRootDir}} images={{.Images}}' 2>/dev/null
docker images --format '{{.Repository}}:{{.Tag}}' 2>/dev/null | grep test || echo "(no test images)"
df -h / /var/lib/docker-pool | tail -2
