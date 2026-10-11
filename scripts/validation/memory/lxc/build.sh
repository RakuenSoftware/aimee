#!/bin/bash
set -euo pipefail
export PATH=/usr/local/go/bin:$PATH
export GOPATH=/var/cache/aimee-go GOCACHE=/var/cache/aimee-go-build GOMODCACHE=/var/cache/aimee-go/pkg/mod
while systemctl is-active --quiet aimee-memory-bootstrap; do sleep 2; done
test "$(systemctl show aimee-memory-bootstrap -p ExecMainStatus --value)" = 0
pg_isready -h 127.0.0.1 -p 5432
cd /opt/aimee
make -C src -j4 ../aimee ../aimee-server ../aimee-kb ../aimee-delegate-egress build/obj/aimee-module build/obj/aimee-module-config
install -m0755 aimee aimee-server aimee-kb /usr/local/bin/
if [[ ! -f /root/aimee-validation-env.json ]]; then python3 /root/aimee-memory-provision.py; fi
