cd /opt/aimee-now
echo -n "review_completeness in server: "; grep -c review_completeness src/server/server_mcp.c
echo -n "review_completeness in schema: "; grep -c review_completeness src/modules/protocols/mcp/mcp_tools_extended.c
nohup docker build -f Dockerfile.server -t aimee-server:rc . > build-rc.log 2>&1 &
disown
echo BUILD_STARTED
