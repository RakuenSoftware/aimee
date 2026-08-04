set -e
# Extract the CLI from the freshly built server image and install it, keeping the
# outgoing binary next to it. Run inside the target CT.
cid=$(docker create aimee-server:batch)
docker cp "$cid":/usr/local/bin/aimee /tmp/aimee-batch
docker rm "$cid" >/dev/null
cp -a /usr/local/bin/aimee /usr/local/bin/aimee.prev-batch
install -m755 /tmp/aimee-batch /usr/local/bin/aimee
echo -n "version: "; aimee --version 2>&1 | head -1
echo -n "hooks pre: "; strings /usr/local/bin/aimee | grep -c "hooks pre"
echo -n "batching guidance: "; strings /usr/local/bin/aimee | grep -c "ONE call, joined with"
echo -n "spans guidance: "; strings /usr/local/bin/aimee | grep -c "takes a .spans. array"
echo INSTALL_DONE
