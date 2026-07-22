#!/usr/bin/env bash
set -euo pipefail
pct push 103 /tmp/schema_roles.sql /tmp/p5b-schema-roles.sql
pct push 103 /tmp/schema.sql /tmp/p5b-schema.sql
pct push 103 /tmp/schema_grants.sql /tmp/p5b-schema-grants.sql
pct push 103 /tmp/p5b_status_pg17_test.sql /tmp/p5b-status-pg17-test.sql
pct push 103 /tmp/run-p5b-status-ct103.sh /root/run-p5b-status-ct103.sh
pct exec 103 -- chmod 0644 /tmp/p5b-schema-roles.sql /tmp/p5b-schema.sql /tmp/p5b-schema-grants.sql /tmp/p5b-status-pg17-test.sql
pct exec 103 -- chmod 0700 /root/run-p5b-status-ct103.sh
pct exec 103 -- bash /root/run-p5b-status-ct103.sh
