cd /opt/aimee-now
echo -n "revert present:   "; grep -c "still run where this session started" src/cli_mcp_serve.c
echo -n "scoping guidance: "; grep -c "Fix the OWNER, not one caller" src/client_integrations.c
nohup docker build -f Dockerfile.server -t aimee-server:fix2 . > build-fix2.log 2>&1 &
disown
echo BUILD_STARTED
