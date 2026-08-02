#!/bin/sh
# Restart the model mirror and the matrix after the containerd move.
set -u

pkill -f http.server 2>/dev/null || true
cd /mnt/gguf || exit 1
nohup python3 -m http.server 8099 >/tmp/mirror.log 2>&1 &
sleep 3
echo "mirror: $(curl -sI -m 8 http://127.0.0.1:8099/gemma-4-E2B-it-UD-Q6_K_XL.gguf 2>/dev/null | head -1)"

cd /root/matrix || exit 1
rm -f matrix.log build-*.log
systemctl stop aimee-matrix 2>/dev/null || true
systemd-run --unit=aimee-matrix --collect --working-directory=/root/matrix \
  --setenv=AIMEE_MODEL_MIRROR=http://127.0.0.1:8099 \
  /bin/sh -c './matrix-test.sh > matrix.log 2>&1'
sleep 3
echo "matrix: $(systemctl is-active aimee-matrix)"
df -h / | tail -1
