# Proposal: Self-sufficient thin client execution-plane residuals

- **State:** pending
- **Status refreshed:** 2026-06-09
- **Split:** thin-client invariant, data-plane routing, and detached-workspace reverse-channel foundation moved to `docs/proposals/done/self-sufficient-thin-client-data-plane.md`.

## Remaining Work

- Lift the remote refusal in `aimee mcp-serve` by registering a detached workspace, running `workspace serve` as the reverse channel, and executing MCP tool calls through the remote server.
- Apply the same register-serve-run-teardown flow to remote `chat.send_stream` so chat can execute against a remote server while file operations stay inside the served workspace.
- Expose delegate execution over `/v1` for thin clients under the appropriate remote-write and capability policy.
- Validate bearer scoping, workspace teardown, and failure cleanup for remote execution.
