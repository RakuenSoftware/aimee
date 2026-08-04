export AIMEE_HOME=/var/lib/docker/volumes/aimee_aimee-server-home/_data
printf '%s\n%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"probe","version":"1"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
| timeout 60 /usr/local/bin/aimee mcp serve 2>&1 \
| python3 -c '
import json, sys
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        d = json.loads(line)
    except Exception:
        continue
    tools = ((d.get("result") or {}).get("tools"))
    if not tools:
        continue
    print("tools served: %d" % len(tools))
    for t in tools:
        if t.get("name") == "index":
            props = sorted((t.get("inputSchema") or {}).get("properties", {}))
            print("index properties: %s" % ", ".join(props))
            print("SPANS PRESENT: %s" % ("spans" in props))
'
