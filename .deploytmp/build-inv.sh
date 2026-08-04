cd /opt/aimee-now
echo -n "investigate in call table: "; grep -c investigate src/server/server_mcp_call_table.c
echo -n "investigate in skill:      "; grep -c investigate src/client_integrations.c
nohup docker build -f Dockerfile.server -t aimee-server:inv . > build-inv.log 2>&1 &
disown
echo BUILD_STARTED
