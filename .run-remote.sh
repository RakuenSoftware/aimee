#!/bin/sh
# Start the verification chain on the test host so it SURVIVES the ssh session.
#
# `nohup` inside `pct exec` is not enough: pct exec runs the command in a
# transient scope that systemd tears down when the exec finishes, taking the
# background children with it. systemd-run gives each job its own unit that
# outlives the session, which is what an overnight run needs.
set -u

systemctl stop aimee-hashcheck aimee-matrix 2>/dev/null || true
systemd-run --unit=aimee-hashcheck --collect \
    --working-directory=/root \
    /bin/sh -c './hashcheck.sh > hashcheck.log 2>&1'
echo "hashcheck: $(systemctl is-active aimee-hashcheck 2>/dev/null)"
