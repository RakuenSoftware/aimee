# Config authority surface residual

- **State:** PENDING — residual scope only.
- **Archived parent:**
  [`config-single-source-of-truth-audit.md`](../done/config-single-source-of-truth-audit.md).

## Delivered foundation

The archived audit established the governing rule: durable behavior and tuning belong to config with
validated environment overrides, while secrets, bootstrap transport, process propagation, tests, and
security-disabling deployment controls may remain env-only by an explicit decision. The falsey
`AIMEE_ANTIPATTERNS_BYPASS` fail-open bug is fixed and tested.

## Remaining work

- Feed the configured KB cache TTL to `kb_cache_configure` instead of always selecting the env/default
  path.
- Decide and document the authority relationship among `require_session_worktree`,
  `AIMEE_ALLOW_MAIN_CHECKOUT`, and the repository marker.
- Reconcile the autonomy spend/admission rails and the verified workflow, verification, DB2,
  retrieval, runtime, MCP, index, refresh, and log-level families listed in the archived audit.
- Enumerate the previously sampled CLI axis and the omitted Go sources before claiming full coverage.
- Keep every retained env-only exception in a machine-readable allowlist with rationale and owner.

## Acceptance

A checker inventories C, Go, and CLI configuration inputs; every durable knob has a config key,
round-trip coverage, and a value-validated override, while every env-only input matches a reviewed
exception. Unknown or invalid override values never silently weaken the config value.

