# Proposal: config as single source of truth — env/CLI surface audit

- **State:** DONE — initial audit and first safety fix archived 2026-08-04; residual extracted.

> **Archived after partial delivery.** The audit established the durable-config versus
> deployment-only doctrine and the falsey anti-pattern bypass bug is fixed and regression-tested.
> Cache TTL, split main-checkout authority, the autonomy rails, the remaining env-only families, and
> the missing Go/CLI enumeration now live in
> [`config-authority-surface-residual.md`](../pending/config-authority-surface-residual.md).
- **Author:** JBailes (drafted by Claude, 2026-07-30).
- **Scope:** every `getenv("AIMEE_*")` call site in `src/` (tests excluded), checked against the
  live config surface (`config_fields.c`, `config_sections.c`, `config_memory.c`,
  `config_kb_curator.c`, and the `config_load` parse blocks).

## Method and its limits

258 distinct `AIMEE_*` vars are read in `src/`. Naive name-matching (`AIMEE_FOO_BAR` → `foo_bar`)
produced 217 apparent gaps and was **wrong** — it misses config keys whose name differs from the env
name. `memory.citations.mode`, `memory.cognify.async.enabled`, `memory.pagerank.relations`, and the
`transport.*` flags all matched nothing by name yet are fully config-backed. Every finding below was
re-verified by reading the call site and the corresponding parser. Anything unverified is marked.

**Not covered:** the CLI-flag axis was sampled, not enumerated (one finding: `--log-level`).
Hardcoded tunables were not systematically swept. Go sources (`server-go/`, `webchat/`,
`kb-console/`) contribute only 6 env reads and were not analyzed.

## The doctrine already exists — and it is not "everything in config"

`src/posix/web_egress.c:183` states the rule for security-disabling switches:

> Deliberately an environment variable, not a config key. `config.set` is capability-gated but still
> reachable from inside the running system; an environment variable is set when the deployment is
> built. Permitting a private destination should require touching the deployment, not the running
> config.

That is correct and should stand. "Config is the single source of truth" therefore means: **every
durable behavioral/tuning knob is config-backed with env as a validated operator override**, while
controls that must not be reachable from inside a running system stay env-only *by an explicit,
documented decision*. The reference implementation of the first half is `config_autonomy_lookup`
(`src/modules/config/config.c:2048`): config snapshot is authoritative, an exported env var wins only
if it parses, and garbage falls through to the config value.

## Finding 1 (bug) — `AIMEE_ANTIPATTERNS_BYPASS` is gated on presence, not value

`src/modules/guardrails/guardrails_orchestrator.c:1644`:

```c
if (!getenv("AIMEE_ANTIPATTERNS_BYPASS"))
```

The comment two lines above (and the operator-facing message at :1689) both document it as
`AIMEE_ANTIPATTERNS_BYPASS=1`. But the guard tests only **presence**, so
`AIMEE_ANTIPATTERNS_BYPASS=0` and `=false` *disable the anti-pattern check* — the opposite of what an
operator setting `0` intends. Every other boolean env in the tree validates its value
(`AIMEE_KB_HARDENED`, `AIMEE_DELEGATE_SANDBOX`, `AIMEE_ALLOW_MAIN_CHECKOUT`, the
`config_autonomy_lookup` path). This one is inconsistent and fails open.

Fix: validate the value. This is a bug fix, independent of the rest of this proposal.

## Finding 2 (bug) — `kb_cache_ttl_s` can never be set from config

`kb_cache_configure(int ttl_s)` (`src/modules/kb_client/kb_client_cache.c:29`) falls back to
`AIMEE_KB_CACHE_TTL_S` only when `ttl_s < 0`. Its sole caller is
`src/server/server_main.c:301`: `kb_cache_configure(-1);`

So the parameter exists to accept a configured value and nothing ever passes one — the KB result
cache TTL is env-only in practice. The plumbing is already there; only the call site is wrong.

## Finding 3 — split authority on main-checkout edits

`require_session_worktree` is a config key (default true) enforcing that mutating sessions run in an
isolated worktree. `aimee_main_clone_edits_allowed` (`src/util.c:1167`) grants the same permission
from `AIMEE_ALLOW_MAIN_CHECKOUT` (value-validated) *or* a `.git/aimee-allow-main-edits` marker file.
Three independent authorities over one boundary, only one of which is visible to config review.
Not necessarily wrong — but it should be a stated decision, and today it is not documented anywhere.

## Finding 4 — the autonomy family is split in half

Eleven autonomy knobs are config-backed and live via `config_autonomy_lookup`. Seven are env-only,
and `config.c:2084` self-documents the split (`/* not a config-backed autonomy var (e.g. USD_PER_SEC)
-> caller falls back */`). The env-only seven are the **spend and admission rails**:

| Env var | Controls | Default | Site |
|---|---|---|---|
| `AIMEE_AUTONOMY_MAX_USD` | per-run USD budget ceiling | `5.0` | `wfe_autonomy.c:185` |
| `AIMEE_AUTONOMY_USD_PER_SEC` | cost-estimate rate | `0.0005` | `wfe_iface.c:69` |
| `AIMEE_AUTONOMY_PANEL_RETRIES` | transient panel-park retry cap | `WFE_AUTONOMY_PANEL_RETRY_CAP_DEFAULT` | `wfe_autonomy.c:63` |
| `AIMEE_AUTONOMY_BASE` | autonomous merge-target base branch | `"testing"` | `wfe_iface.c:32` |
| `AIMEE_AUTONOMY_MAX_ACTIVE_PER_PRINCIPAL` | concurrent runs per principal | `5` | `server_dev_submit.c:107` |
| `AIMEE_AUTONOMY_SUBMIT_RATE_PER_MIN` | submit rate cap | `10` | `server_dev_submit.c:108` |
| `AIMEE_AUTONOMY_SUBMIT_WINDOW_SECS` | rate window | `60` | `server_dev_submit.c:109` |

A per-run dollar ceiling and the branch autonomous work merges into cannot be set, reviewed, or
audited through config. `autonomy.max_usd` and `autonomy.usd_per_sec` need a `double` variant of
`config_autonomy_lookup` (today it returns `long`); the other five fit the existing path.

## Finding 5 — verified gaps outside autonomy

Each verified at its call site; all are durable tunables with no config key.

| Proposed key | Env var | Default | Site |
|---|---|---|---|
| `workflow_lease_ttl_secs` | `AIMEE_WORKFLOW_LEASE_TTL_SECS` | `3600` | `wfe_enforce.c:199` |
| `wfe_worktree_gc_grace_secs` | `AIMEE_WFE_WORKTREE_GC_GRACE_SECS` | `3600` | `wfe_scheduler.c:305` |
| `workflow_autonomous_router_enabled` | `AIMEE_WORKFLOW_AUTONOMOUS_ROUTER` | off | `server_dev_submit.c:44` |
| `workflow_enforce_stage` | `AIMEE_WORKFLOW_ENFORCE_STAGE` | — | 6 sites incl. `cmd_hooks.c:225` |
| `wfe_engine` | `AIMEE_WFE_ENGINE` | C engine | `trigger_scheduler.c:1368`, `wfe_scheduler.c:536` |
| `verify_parallel` | `AIMEE_VERIFY_PARALLEL` | derived, capped 4 | `git_verify.c:1017` |
| `verify_step_timeout_ms` | `AIMEE_VERIFY_STEP_TIMEOUT_MS` | `VERIFY_STEP_TIMEOUT_MS_DEFAULT` | `git_verify_step.c:20` |
| `panel_seat_wait_secs` | `AIMEE_PANEL_SEAT_WAIT_SECS` | `300` | `wfe_live_panel.c:56` |
| `delegate_heartbeat_monitor_enabled` | `AIMEE_DELEGATE_HEARTBEAT_MONITOR` | on | `server_delegate_monitor.c:153` |
| `db2_statement_timeout_ms` | `AIMEE_DB2_STATEMENT_TIMEOUT_MS` | `DB2_DEFAULT_STATEMENT_TIMEOUT_MS` | `db_postgres.c:931` |
| `db2_idle_in_transaction_timeout_ms` | `AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS` | `DB2_DEFAULT_…` | `db_postgres.c:937` |
| `pgvec_slow_query_ms` | `AIMEE_PGVEC_SLOW_QUERY_MS` | `100` | `pgvec_transport.c:680` |
| `vector_kb_batch_size` | `AIMEE_VECTOR_KB_BATCH_SIZE` | `64` (clamp 1..512) | `kb.c:1165` |
| `dim_probe_budget_ms` | `AIMEE_DIM_PROBE_BUDGET_MS` | set by `db2_set_dim_probe_budget_ms` | `embedder_probe.c:159` |
| `exec_pipe_timeout_ms` | `AIMEE_EXEC_PIPE_TIMEOUT_MS` | `EXEC_PIPE_DEFAULT_TIMEOUT_MS` | `platform_process.c:236` |
| `mcp_tool_profile` | `AIMEE_MCP_TOOL_PROFILE` | `"core"` | `mcp_tool_profile.c:58` |
| `code_index_source` | `AIMEE_CODE_INDEX_SOURCE` | branch SHA | `code_collect.c:328`, `:740` |
| `codex_refresh_skew_secs` | `AIMEE_CODEX_REFRESH_SKEW` | `3600` | `agent_config.c:1976` |
| `log_level` | `AIMEE_LOG_LEVEL` **and** `--log-level` | `LOG_INFO` | `log.c:29`, `server_main.c:588` |

Two notes on this table:

- `db2_pool_size` **is** config-backed with a validated env override
  (`config.h:132`), while the two `db2_*_timeout_ms` values beside it are not. Same family,
  inconsistent treatment.
- `log_level` is the one confirmed CLI-axis gap: settable per-invocation (`--log-level=`) and by env,
  but an operator cannot set a persistent default in config.
- `mcp_tool_profile`'s own comment says *"Operators set `full` to opt out"* — describing a config
  knob that is not one.

## Confirmed correct — do not "fix" these

- **Config-backed with validated env override** (the target pattern): `background_threads`,
  `compute_threads`, `session_threads`, `db2_pool_size`, `delegate_max_inflight` (`config.h:89-143`);
  `worktree_gc` / `worktree_gc_days` (`server.c:1172`); `delegate_sandbox` (`config.c:1396`);
  the eleven `config_autonomy_lookup` knobs; `transport.*`; `memory.citations.*`,
  `memory.cognify.async.*`, `memory.pagerank.*`; `memory_weight_profile`; `vault.tpm2.*`.
- **Env-only by design:** real secrets (`AIMEE_*_TOKEN`, `*_PIN`, `*_SECRET`, `vault_kms_*`);
  bootstrap values read before `config_load` (`AIMEE_HOME`, `AIMEE_RUNTIME_DIR`,
  `AIMEE_SERVER_STARTUP_FD`); per-process propagation between parent and delegate
  (`AIMEE_DELEGATE_DEPTH`, `AIMEE_PARENT_DELEGATION_ID`, `AIMEE_DELEGATE_SOURCE_*` — these are
  `setenv`'d by aimee itself, not operator input); `AIMEE_TEST_*` hooks; and
  `AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT` per the documented doctrine above.

## Sequencing

1. **Findings 1 and 2** — two independent bug fixes, each a few lines with a regression test. No
   config-surface change. Land first.
2. **Finding 4** — the autonomy seven, following the existing `autonomy.*` section pattern
   (`config_sections.c:806`). Needs a `double` lookup variant for the two USD fields. Highest value:
   these are spend and admission rails.
3. **Finding 5** — the remaining nineteen, in family-sized batches (workflow/wfe, verify, db2,
   retrieval, runtime). Each is the standard six-site add, now cheaper for flat scalars via
   `config_flat_defaults[]` + `config_parse_flat_fields`.
4. **Finding 3** — document the intended authority for main-checkout edits, then reconcile.

Each config key added must keep env as a *validated* override (the `config_autonomy_lookup`
contract), not a bypass, and must land with a `test_config.c` round-trip asserting the non-default
value survives save+reload — the failure mode `require_aimee_git` and `server_api_remote_writes`
already hit.
