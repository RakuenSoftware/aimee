#!/bin/bash
# Stop the kb and clear the sealed first-boot credential vault, so a changed
# AIMEE_DB2_URL is picked up instead of the value sealed on the first boot.
# Run AS ROOT in the container.
set -u
pkill -f aimee-kb 2>/dev/null
sleep 2
rm -rf /root/.config/aimee/.vault
rm -f /root/kb.log
echo "vault cleared"
