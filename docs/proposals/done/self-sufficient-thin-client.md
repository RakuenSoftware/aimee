# Proposal: Self-sufficient thin client execution-plane residuals

- **State:** done
- **Status refreshed:** 2026-06-09
- **Completed:** 2026-06-09
- **Split:** thin-client invariant, data-plane routing, and detached-workspace reverse-channel foundation moved to `docs/proposals/done/self-sufficient-thin-client-data-plane.md`.

## Shipped

- Lift the remote refusal in `aimee mcp-serve` by registering a detached workspace, running `workspace serve` as the reverse channel, and executing MCP tool calls through the remote server.
- Apply the same register-serve-run-teardown flow to remote `chat.send_stream` so chat can execute against a remote server while file operations stay inside the served workspace.
- Expose delegate execution over `/v1` for thin clients under the appropriate remote-write and capability policy.
- Validate bearer scoping, workspace teardown, and failure cleanup for remote execution.

## Completion Notes

The residual execution plane is complete:

- `aimee mcp-serve` and remote chat/launch start the detached-workspace reverse channel automatically when a remote `/v1` endpoint is configured.
- The reverse channel registers the current working tree as a detached workspace, serves runner operations over `/v1/runner/poll` and `/v1/runner/respond`, and removes the detached workspace on stop.
- Remote launch failures and launch rejections stop the reverse channel immediately, preventing stale workspace registrations.
- Delegate execution is exposed through first-class `/v1/delegate/*` routes and remains gated by `remote_writes=full` plus unscoped delegate capability checks.
- Scoped bearer tests cover that detached workspace add/remove and runner execution remain unavailable to query-only scoped tokens.
