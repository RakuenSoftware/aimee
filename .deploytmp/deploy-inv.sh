set -e
cid=$(docker create aimee-server:fix2)
docker cp "$cid":/usr/local/bin/aimee /tmp/aimee-inv
docker rm "$cid" >/dev/null
cp -a /usr/local/bin/aimee /usr/local/bin/aimee.prev-inv
install -m755 /tmp/aimee-inv /usr/local/bin/aimee
echo -n "investigate in cli: "; strings /usr/local/bin/aimee | grep -c "STARTING on an unfamiliar area"
echo -n "plural forms:       "; strings /usr/local/bin/aimee | grep -c "file_paths"

cd /opt/aimee-src
sed -i 's|^AIMEE_SERVER_IMAGE=.*|AIMEE_SERVER_IMAGE=aimee-server:fix2|' .env
docker compose -f compose.server-managed.yaml up -d aimee-server >/dev/null 2>&1
sleep 8
docker ps --format '{{.Names}}  {{.Image}}  {{.Status}}' | grep aimee-server
echo DEPLOY_DONE
