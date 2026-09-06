#!/bin/bash
# Install the memory module's grant so aimee-kb's module bus will admit it.
#
# obs_bus_configure_daemon_module_runtime("kb", <config dir>) resolves the bus
# socket to <config dir>/kb-module-bus.sock and the policy directory to
# <config dir>/modules.d/kb, which holds one strict *.grant per installed
# executable. The grant below is exactly what scripts/export_c_repositories.py
# generates from src/modules/process-contracts.json now that `memory` is placed
# in kb -- serving its six stages, requesting nothing, publishing nothing.
# Run AS ROOT in the container.
set -u
CONF=/root/.config/aimee
mkdir -p "$CONF/modules.d/kb"

cat > "$CONF/modules.d/kb/memory.grant" <<'EOF'
version=1
principal_class=1
principal_ref=7
uid=self
executable=/usr/local/libexec/aimee-modules/aimee-module-memory
publish=
subscribe=
request=
serve=5889,5890,5891,5892,5893,5894,5895
EOF

cat > "$CONF/modules.d/kb/memory-postgres.grant" <<'EOF'
version=1
principal_class=1
principal_ref=73
uid=self
executable=/usr/local/libexec/aimee-modules/aimee-module-memory
publish=
subscribe=
request=11266
serve=
EOF

cat > "$CONF/modules.d/kb/memory-egress.grant" <<'EOF'
version=1
principal_class=1
principal_ref=70
uid=self
executable=/usr/local/libexec/aimee-modules/aimee-module-memory
publish=
subscribe=
request=12290
serve=
EOF

echo "grant installed:"
cat "$CONF/modules.d/kb/memory.grant"
cat "$CONF/modules.d/kb/memory-postgres.grant"
cat "$CONF/modules.d/kb/memory-egress.grant"
