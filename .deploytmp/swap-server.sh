set -e
cd /opt/aimee-src
# Repin the server image and recreate just that service. The KB keeps running --
# it is a separate compose project and the batch change is server-only.
sed -i 's|^AIMEE_SERVER_IMAGE=.*|AIMEE_SERVER_IMAGE=aimee-server:batch|' .env
grep '^AIMEE_SERVER_IMAGE' .env
docker compose -f compose.server-managed.yaml up -d aimee-server
sleep 8
docker ps --format '{{.Names}}  {{.Image}}  {{.Status}}' | grep aimee-server
echo SWAP_DONE
