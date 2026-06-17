# Configuration Reference

> Auto-generated from the canonical source tables by `scripts/gen-reference-docs.py` — config keys from `src/config_fields.c` + `src/config*.c`, env vars scanned from `getenv()` in `src/`, and the workflow surface from `src/workflow/`. Do not edit by hand; run `make -C src docs-gen` to regenerate.

This reference covers every configurable surface:

1. **Config-store keys** — the `aimee config` keys + config-file sections (below).
2. **Environment variables** — `AIMEE_*` runtime/deployment overrides.
3. **External & provider environment** — provider keys, endpoints, proxy, editor.
4. **Workflow engine** — workflow definition + custom-block (`blocks.yaml`) schema.
5. **Other config files** — `agents.json`, toolsets, guardrails.

CLI commands + flags are documented separately in [`cli-commands.md`](cli-commands.md).

Configuration lives in the per-`AIMEE_HOME` config store. Scalar keys in the table below are settable from the CLI:

```
aimee config show                 # print the effective config
aimee config get <key>            # read one value
aimee config set <key> <value>    # set one value
```

Structured options (arrays, nested objects — e.g. `ensemble.reference_models`) are not CLI-settable; they are written into the config file under the sections listed at the end.

## CLI-settable keys (108)

| Key | Type | Description |
|-----|------|-------------|
| `autonomous` | bool | Run autonomously (auto-advance preauthorized gates) vs interactive. |
| `cache_aware_rewrite_enabled` | bool | Rewrite prompts to align with the provider's prompt cache. |
| `cache_min_chars` | int | Minimum prompt size (chars) before cache-shaping applies. |
| `cache_shaping_enabled` | bool | Enable prompt cache-shaping. |
| `claude_cli_delegate_enabled` | bool | Allow delegating to the local Claude CLI agent. |
| `claude_model` | string | Default Claude model (empty = CLI default). |
| `cost_reward_enabled` | bool | Factor token cost into the reward signal. |
| `cost_reward_lambda_pct` | int | Cost-penalty weight (percent) in the reward. |
| `cost_reward_ref_usd_milli` | int | Reference cost (USD-milli) normalizing the cost reward. |
| `cross_verify` | bool | Enable cross-model verification of outputs. |
| `css_render_command` | string | Render backend for the #4-full computed-style oracle: a command reading {html,css} JSON on stdin and writing a computed-style snapshot JSON on stdout (run an isolated headless-browser sidecar). |
| `css_style_graph_enabled` | bool | Enable the CSS migration assistant's style-graph write path during indexing. |
| `db2_url` | string | DB2 connection URL (aimee's vector / knowledge-base store). |
| `dedup_enabled` | bool | Deduplicate near-identical responses. |
| `dedup_window_seconds` | int | Window (seconds) for response dedup. |
| `dogfood_autolabel_continuation` | bool | Auto-label continuation turns for dogfood capture. |
| `dogfood_autolabel_repair` | bool | Auto-label repair turns for dogfood capture. |
| `dogfood_autolabel_repeat_question` | bool | Auto-label repeated-question turns. |
| `dogfood_commit_raw` | bool | Commit raw (unredacted) dogfood transcripts. |
| `dogfood_enabled` | bool | Capture sessions as dogfood training/eval data. |
| `dogfood_inline_tagging` | bool | Inline-tag dogfood events during the session. |
| `dogfood_log_dir` | string | Directory for dogfood logs. |
| `ecomode` | bool | Reduce background compute (eco mode). |
| `embedding_command` | string | Command that produces embeddings (overrides the endpoint). |
| `embedding_dim` | int | Embedding vector dimension. |
| `embedding_endpoint` | string | Embeddings provider endpoint URL. |
| `embedding_model` | string | Embeddings model name. |
| `fidelity_check_enabled` | bool | Run the answer-fidelity judge on terminal-text turns (default off; requires kb_evidence_emit_enabled + ingress_preinject_enabled). |
| `guardrail_mode` | string | Guardrail enforcement mode (off / warn / block). |
| `guardrails_semantic_allow_ml_only_block` | bool | Allow blocking on the ML classifier alone. |
| `guardrails_semantic_block_threshold` | float | Semantic score threshold to block. |
| `guardrails_semantic_command` | string | External semantic-guardrail classifier command. |
| `guardrails_semantic_dry_run` | bool | Evaluate but don't enforce semantic guardrails. |
| `guardrails_semantic_enabled` | bool | Enable the semantic guardrail classifier. |
| `guardrails_semantic_prompt_threshold` | float | Semantic score threshold for prompt-level flags. |
| `guardrails_semantic_warn_threshold` | float | Semantic score threshold to warn. |
| `identity_working_profile_injection_enabled` | bool | Inject the working-profile identity into prompts. |
| `ingress_audit_async` | bool | Audit ingress requests asynchronously. |
| `ingress_max_raw_scans` | int | Max raw-content scans per ingress request. |
| `ingress_preinject_assembly_budget` | int | Token budget for ingress context pre-injection. |
| `ingress_preinject_enabled` | bool | Enable `<aimee-context>` pre-injection on ingress. |
| `ingress_trusted_proxy_secret` | string | Shared secret authenticating a trusted ingress proxy. |
| `ingress_usage_accounting_enabled` | bool | Account token usage on ingress requests. |
| `integrity_dry_run` | bool | Run integrity checks without enforcing. |
| `integrity_enabled` | bool | Enable the integrity gate. |
| `kb_api_bearer_token` | string | Bearer token for the aimee-kb API. |
| `kb_api_http_port` | int | HTTP port the aimee-kb API listens on. |
| `kb_evidence_emit_enabled` | bool | Emit evidence records from KB ingest. |
| `kb_mining_enabled` | bool | Enable background KB mining. |
| `kb_mining_min_poll_s` | int | Minimum interval (s) between KB mining polls. |
| `kb_search_max_results` | int | Default max results for KB search. |
| `learning_implicit_citation_continuation` | bool | Implicit-learning signal: citation on continuation. |
| `learning_implicit_citation_repair` | bool | Implicit-learning signal: citation on repair. |
| `learning_implicit_repeat_question` | bool | Implicit-learning signal: repeated question. |
| `learning_implicit_repeated_correction` | bool | Implicit-learning signal: repeated correction. |
| `learning_implicit_workflow_repetition` | bool | Implicit-learning signal: workflow repetition. |
| `learning_max_commits_per_week` | int | Cap on learning-derived commits per week. |
| `learning_proposal_ttl_days` | int | TTL (days) for learning proposals. |
| `learning_router_enabled` | bool | Enable the learning router. |
| `max_iterations` | int | Per-turn iteration cap for interactive chat (default 15). |
| `max_iterations_delegate` | int | Per-turn iteration cap for delegate sessions (default 25). |
| `memory_abstain_enabled` | bool | Allow memory recall to abstain on low confidence. |
| `memory_abstain_gate` | float | Confidence gate for memory abstention. |
| `memory_bm25_weight` | float | BM25 (lexical) weight in hybrid memory recall. |
| `memory_chunk_min_confidence` | float | Minimum confidence to keep a memory chunk. |
| `memory_coref_mode` | string | Coreference-resolution mode for memory. |
| `memory_coref_window` | int | Coreference lookback window. |
| `memory_fetch_budget_base` | int | Base token budget for memory fetch. |
| `memory_fetch_budget_enabled` | bool | Enable token-budgeted memory fetch. |
| `memory_fetch_budget_shape_aware` | bool | Shape-aware memory fetch budgeting. |
| `memory_hard_negative_log` | string | Path to the hard-negative recall log file (empty = disabled). |
| `memory_improve_dedupe_enabled` | bool | Dedupe during memory-improve. |
| `memory_improve_summarise_enabled` | bool | Summarise during memory-improve. |
| `memory_kb_neighbour_expand` | bool | Expand recall to KB neighbours. |
| `memory_maintenance_trigger_inserts` | int | Inserts before a maintenance cycle triggers. |
| `memory_maintenance_trigger_secs` | int | Seconds before a maintenance cycle triggers. |
| `memory_negation_enabled` | bool | Detect/handle negation in memory. |
| `memory_profile_cards_enabled` | bool | Maintain profile cards from observations. |
| `memory_profile_cards_min_obs` | int | Min observations before a profile card forms. |
| `memory_profile_cards_stale_secs` | int | Profile-card staleness (seconds). |
| `memory_query_expansion_k` | int | Number of expanded queries for recall. |
| `memory_query_expansion_mode` | string | Query-expansion mode. |
| `memory_rerank_command` | string | External reranker command. |
| `memory_rerank_enabled` | bool | Enable cross-encoder reranking of recall. |
| `memory_rerank_mode` | string | Reranker mode. |
| `memory_rerank_top_k` | int | Top-K candidates to rerank. |
| `memory_rewrite_command` | string | External query-rewrite command. |
| `memory_rewrite_decompose` | bool | Decompose queries during rewrite. |
| `memory_rewrite_enabled` | bool | Enable query rewriting for recall. |
| `memory_rewrite_hyde` | bool | Use HyDE (hypothetical-document) rewrite. |
| `memory_rewrite_max_subqueries` | int | Max sub-queries produced by rewrite. |
| `memory_scenes_enabled` | bool | Cluster memories into scenes. |
| `memory_scenes_min_cluster_size` | int | Min cluster size for a scene. |
| `memory_scenes_top_m` | int | Top-M scenes to consider. |
| `memory_semantic_floor_scale` | float | Multiplier on the semantic-recall cosine floors (0 = auto-scale by the active embedder dimension; >0 pins it). |
| `memory_semantic_weight` | float | Semantic (vector) weight in hybrid recall. |
| `memory_window_radius` | int | Neighbour radius for memory-window expansion. |
| `openai_endpoint` | string | OpenAI-compatible endpoint URL. |
| `openai_key_cmd` | string | Command that prints the OpenAI API key. |
| `openai_model` | string | OpenAI model name. |
| `provider` | string | Default model provider. |
| `reasoning_cap_enabled` | bool | Cap the model's reasoning effort. |
| `server_cli_oauth_enabled` | bool | — |
| `typed_facts_enabled` | bool | Enable the typed-fact knowledge layer (master gate; default off). |
| `verify_cross_project` | bool | Let `aimee git verify` span other projects. |
| `verify_enabled` | bool | Master gate for `aimee git verify` (default off). |
| `virtual_context_assembly_budget` | int | Token budget for virtual-context assembly. |
| `virtual_context_enabled` | bool | Enable virtual-context assembly. |

> **Undocumented** (add to `CFG_KEY_DESC` in gen-reference-docs.py): `server_cli_oauth_enabled`

## Config-file sections (48)

Set in the config JSON as `{"<section>": {"<key>": ...}}`. Keys are derived from the section parsers in `src/config*.c`; a key shown as a bare name that is itself a nested object is noted in the section description (see *Coverage & limitations*).

- **`aimee`** — _Core API/runtime settings._ Keys: `api`
- **`auxiliary`** — _Auxiliary (cheap/background) model used for side tasks._ Keys: `default_max_tokens`, `default_model`, `default_provider`, `enabled`, `tasks`
- **`cache_shaping`** — _Prompt-cache shaping._ Keys: `enabled`, `min_chars`
- **`charter`** — _Operating charter: values, constraints, safety axioms, tone._ Keys: `hard_constraints`, `safety_axioms`, `tone_boundaries`, `values`, `working_profile_drift_limit`
- **`compact`** — _Transcript compaction thresholds._ Keys: `enabled`, `head_bytes`, `per_tool`, `tail_bytes`, `threshold`
- **`computer_use`** — _Computer-use (browser) tool settings._ Keys: `allowed_domains`, `default_navigation`, `enabled`, `redact_sensitive_screenshots`
- **`concurrency`** — _Per-model / per-provider concurrency limits._ Keys: `default`, `per_model`, `per_provider`, `preempt`
- **`context`** — _Context-engine selection._ Keys: `engine`
- **`cost_reward`** — _Cost-aware reward shaping._ Keys: `enabled`, `lambda_pct`, `ref_usd_milli`
- **`cron_jobs`** — _Scheduled job definitions (array of objects)._ Keys: `context_from`, `deliver`, `enabled`, `id`, `mode`, `pre_wake_gate`, `prompt`, `schedule`, `script`, `skills`, `when_context_contains`, `workdir`
- **`cross_verify`** — _Cross-model output verification._ Keys: `enabled`, `prompt`, `role`, `verify_cmd`
- **`db2`** — _DB2 / vector store settings._ Keys: `vector`
- **`dedup`** — _Response deduplication._ Keys: `enabled`, `window_seconds`
- **`dogfood`** — _Session capture for dogfood data._ Keys: `commit_raw`, `enabled`, `inline_tagging`, `log_dir`
- **`ensemble`** — _Roundtable ensemble panel + aggregator._ Keys: `aggregator`, `max_cost_usd`, `min_successful`, `reference_models`, `reference_personas`
- **`guardrails`** — _Semantic guardrail policy._ Keys: `semantic`
- **`identity`** — _Working-profile identity injection._ Keys: `working_profile_injection`
- **`ingress`** — _Ingress (proxy frontends) behavior._ Keys: `audit_async`, `trusted_proxy_secret`, `usage_accounting_enabled`
- **`integrity`** — _Integrity gate._ Keys: `dry_run`, `enabled`
- **`intelligence`** — _Intelligence subsystems (bandit, planner, ranking, reasoning) + their external commands; most children are nested objects._ Keys: `bandit`, `bandit_optimize_command`, `calibrate`, `constraint_solver_command`, `demotion`, `kb`, `planner`, `planner_search_command`, `ranker_fuse_command`, `ranking`, `reasoning`, `reasoning_datalog_command`, `synthesize`
- **`kb`** — _Knowledge-base client + curator / evidence / maintenance / mining (nested objects)._ Keys: `api`, `background_ingest`, `connection_pool_size`, `connection_workers`, `curator`, `evidence`, `maintenance`, `mining`, `search_max_results`, `worker_count`
- **`learning`** — _Learning subsystem (router, implicit, embed, synthesize; nested objects)._ Keys: `embed`, `implicit`, `router`, `synthesize`
- **`lsp_servers`** — _LSP server definitions (array of objects)._ Keys: `args`, `command`, `extensions`, `name`
- **`mcp`** — _MCP integration (e.g. OSV)._ Keys: `osv`
- **`mcp_clients`** — _MCP client connections (array of objects)._ Keys: `bearer_token_env`, `command`, `cwd`, `name`, `transport`, `url`
- **`memory`** — _Memory subsystem; most children (recall, rerank, lifecycle, …) are nested objects with their own keys._ Keys: `abstain`, `aggregation`, `bm25_weight`, `briefing`, `citations`, `cognify`, `context_budget`, `coref`, `derive_facts`, `directives`, `dispositions`, `episode_summaries`, `failure_detection`, `fetch_budget`, `hard_negative_log`, `improve`, `lifecycle`, `pagerank`, `profile_cards`, `prospective`, `recall`, `rewrite`, `routing`, `salience`, `scenes`, `semantic_floor_scale`, `semantic_weight`
- **`memory_maintenance`** — _Memory maintenance scheduling._ Keys: `enabled`, `interval_seconds`, `summarize_enabled`, `trigger_inserts`, `trigger_secs`
- **`memory_negation`** — _Negation handling in memory._ Keys: `enabled`
- **`memory_query_expansion`** — _Recall query expansion._ Keys: `k`, `mode`
- **`memory_recall_lanes`** — _Per-lane recall floors / caps._ Keys: `enabled`, `fact_kinds`, `floor_fact`, `floor_summary`, `k_fact`, `k_summary`, `summary_kinds`
- **`memory_rerank`** — _Recall reranking._ Keys: `command`, `enabled`, `mix`, `top_k`
- **`memory_rewrite`** — _Recall query rewriting._ Keys: `command`, `decompose`, `enabled`, `hyde`, `max_subqueries`
- **`memory_window`** — _Memory-window neighbour expansion._ Keys: `kb_neighbour_expand`, `radius`
- **`model_meta`** — _Model metadata + capability routing._ Keys: `capability_routing`, `refresh_minutes`
- **`otel`** — _OpenTelemetry export._ Keys: `endpoint`, `service_name`
- **`reasoning_cap`** — _Reasoning-effort cap._ Keys: `enabled`
- **`retry`** — _Provider retry / backoff._ Keys: `base_ms`, `max_attempts`, `max_ms`
- **`rewind`** — _Auto-snapshot / rewind._ Keys: `auto_snapshot`
- **`roundtable`** — _Roundtable pipeline thresholds, caps, gates, and turns._ Keys: `converge_threshold`, `deadline_ms`, `max_rounds`, `pipeline_done_bar`, `pipeline_gate_ttl_h`, `pipeline_max_attempts_per_pass`, `pipeline_max_cost_usd`, `pipeline_max_passes`, `pipeline_max_total_cost_usd`, `pipeline_parked_releases_slot`, `pipeline_unknown_context_tokens`, `turns`
- **`sandbox`** — _Tool sandbox (paths, network, mode)._ Keys: `allow_paths`, `mode`, `network`
- **`script`** — _Script-tool allowlist._ Keys: `allowed_tools`
- **`search`** — _Web-search backend (Tavily / SearXNG)._ Keys: `backend`, `max_results`, `searxng_url`, `tavily_api_key`
- **`session`** — _Session / worktree limits._ Keys: `max_sessions`, `max_worktrees`, `stale_threshold_secs`, `virtual_context`
- **`skills`** — _Skill subsystem (capability, curator, dispatch, eval, manage, review; nested objects)._ Keys: `capability`, `curator`, `dispatch`, `eval`, `manage`, `review`
- **`transport`** — _Transport tweaks (cache-aware rewrite)._ Keys: `cache_aware_rewrite`
- **`trigger`** — _Trigger listener (auth, concurrency)._ Keys: `auth_token`, `max_concurrent`
- **`trigger_rules`** — _Trigger rule definitions (array of objects)._ Keys: `event`, `pipeline`, `schedule`, `source`
- **`workspaces`** — _Workspace definitions (array of objects)._ Keys: `head`, `path`, `provider`, `remote`

## Other top-level config-file keys (5)

Scalar keys read directly from the config root (not via the CLI allowlist above):

`db2_pool_size`, `kb_client_bearer_token`, `kb_client_url`, `proxy_token`, `toolsets`

## Environment variables

The binaries read 109 `AIMEE_*` environment variables (scanned from `getenv()` in `src/`, excluding tests). They override config-store values and are mostly for deployment/runtime wiring. Secrets/tokens should be supplied via the environment or the credential vault, never committed.

### Paths & assets

| Variable | Description |
|----------|-------------|
| `AIMEE_BUNDLED_SKILLS_DIR` | Override directory for the bundled skills. |
| `AIMEE_FORENSICS_DIR` | Directory for shutdown-forensics dumps. |
| `AIMEE_GUARDRAILS_PATH` | Path to the guardrails policy file. |
| `AIMEE_HOME` | Root of the per-user state/config store (config, DB1, `workflows/`, keys). Overrides the platform default. |
| `AIMEE_INSTALL_PREFIX` | Install prefix used to locate bundled assets and plugins. |
| `AIMEE_MODELS_DEV_SNAPSHOT` | Path to an offline models.dev catalog snapshot. |
| `AIMEE_PACK_DIR` | Directory of memory profile packs. |
| `AIMEE_TOOLSETS_CONFIG` | Path to a toolsets config file (overrides the default tool allowlists). |
| `AIMEE_WORKSPACES_DIR` | Root directory for mirrored/registered workspaces. |

### Client & session

| Variable | Description |
|----------|-------------|
| `AIMEE_ACTIVE_TOOLSET` | Active toolset (tool allowlist) for the session. |
| `AIMEE_API_BEARER` | Bearer token for the `/v1` API endpoint. |
| `AIMEE_API_ENDPOINT` | Override the `/v1` API endpoint used by the client RPC layer. |
| `AIMEE_ATTACH_ID` | Presence attach id used when joining an existing session. |
| `AIMEE_EFFORT` | Reasoning-effort hint for the session/model. |
| `AIMEE_GUARD` | Attention-guard control; set to bypass the guard hook. |
| `AIMEE_HOOK_CLIENT` | Identifies the calling hook client (e.g. claude/codex) for hook routing. |
| `AIMEE_MODE` | Operating-mode override (e.g. interactive / autonomous). |
| `AIMEE_MODEL` | Override the primary model for the session. |
| `AIMEE_NO_AUTOSTART` | If set, the client does not auto-start a local aimee-server. |
| `AIMEE_PROFILE` | Active working-profile name. |
| `AIMEE_SERVER_TOKEN` | Bearer token presented to aimee-server over TCP. |
| `AIMEE_SERVER_URL` | aimee-server endpoint the thin client connects to (UDS path or `tcp:host:port`). |
| `AIMEE_SESSION_ID` | Pre-set the session id (enables non-blocking session attach). |
| `AIMEE_SESSION_START_VERBOSE` | Verbose logging during session start. |
| `AIMEE_TUI_SESSION` | Identifies the TUI session. |

### Server runtime

| Variable | Description |
|----------|-------------|
| `AIMEE_API_REMOTE_WRITES` | Gate remote (TCP) write methods: `off` | `data` | `full`. |
| `AIMEE_BACKGROUND_THREADS` | Background worker thread count. |
| `AIMEE_COMPUTE_THREADS` | Compute-pool thread count. |
| `AIMEE_INGRESS_PROXY_SECRET` | Shared secret authenticating a trusted ingress proxy's identity headers. |
| `AIMEE_PARALLEL_MAX` | Maximum parallel agent fan-out. |
| `AIMEE_SERVER_HTTP_BIND` | TCP bind address for the server `/v1` HTTP listener (else UDS-only). |
| `AIMEE_SERVER_STARTUP_FD` | Inherited fd for startup-readiness signalling (service launch). |
| `AIMEE_SESSION_THREADS` | Per-session worker thread count. |
| `AIMEE_SOCK` | Sandbox helper socket path. |
| `AIMEE_WORKTREE_GC` | Enable/disable delegate-worktree garbage collection. |
| `AIMEE_WORKTREE_GC_DAYS` | Age threshold (days) for worktree GC. |

### Knowledge base (aimee-kb)

| Variable | Description |
|----------|-------------|
| `AIMEE_KB_API_BEARER_TOKEN` | Bearer token for the aimee-kb API. |
| `AIMEE_KB_API_CA_BUNDLE` | CA bundle path for verifying the aimee-kb TLS certificate. |
| `AIMEE_KB_API_URL` | aimee-kb HTTP API base URL. |
| `AIMEE_KB_CACHE_TTL_S` | KB client cache TTL (seconds). |
| `AIMEE_KB_CONN` | KB connection string (mTLS transport). |
| `AIMEE_KB_EMIT_ENROLL` | Emit a client enrollment token on KB start. |
| `AIMEE_KB_EMIT_SCOPE` | Scope for the emitted enrollment token. |
| `AIMEE_KB_HTTP_BIND` | aimee-kb HTTP listener bind address. |
| `AIMEE_KB_MTLS_HOST` | aimee-kb mTLS listener host. |
| `AIMEE_KB_MTLS_PORT` | aimee-kb mTLS listener port. |
| `AIMEE_KB_OIDC_AUDIENCE` | OIDC audience for KB API auth. |
| `AIMEE_KB_OIDC_ISSUER` | OIDC issuer for KB API auth. |
| `AIMEE_KB_OIDC_JWKS_FILE` | OIDC JWKS file for KB API auth. |
| `AIMEE_KB_OIDC_SCOPE_CLAIM` | OIDC claim carrying the scope. |
| `AIMEE_KB_OIDC_SCOPE_KIND` | OIDC scope-kind interpretation. |
| `AIMEE_VECTOR_KB_BATCH_SIZE` | Embedding batch size for KB vector ingest. |

### Database & vectors

| Variable | Description |
|----------|-------------|
| `AIMEE_DB2_URL` | Postgres (DB2) connection URL for the KB store. |
| `AIMEE_EMBEDDING_DIM` | Embedding dimension (drives halfvec column sizing). |
| `AIMEE_PGVEC_SLOW_QUERY_MS` | Slow-query log threshold (ms) for the pgvector transport. |

### Memory

| Variable | Description |
|----------|-------------|
| `AIMEE_CONTEXT_NO_KB` | Skip KB lookups during context assembly. |
| `AIMEE_MEMORY_CITATIONS_MODE` | Citation rendering mode for memory recall. |
| `AIMEE_MEMORY_CITATIONS_STRIP_UNVERIFIED` | Strip unverified citations from recall output. |
| `AIMEE_MEMORY_COGNIFY_ASYNC_ENABLED` | Enable the async cognify pipeline. |
| `AIMEE_MEMORY_COREF_MODE` | Coreference-resolution mode. |
| `AIMEE_MEMORY_MAINTENANCE_TRIGGER_INSERTS` | Inserts before a maintenance cycle triggers. |
| `AIMEE_MEMORY_MAINTENANCE_TRIGGER_SECS` | Seconds before a maintenance cycle triggers. |
| `AIMEE_MEMORY_PAGERANK_RELATIONS` | Relation types included in memory PageRank. |
| `AIMEE_MEMORY_RERANK_FORCE_OFF` | Force the cross-encoder reranker off. |
| `AIMEE_MEMORY_RERANK_MODE` | Reranker mode. |
| `AIMEE_MEMORY_WEIGHT_PROFILE` | Recall scoring weight profile. |
| `AIMEE_NO_CACHE` | Disable the memory-assembly cache. |

### Delegates & backends

| Variable | Description |
|----------|-------------|
| `AIMEE_DELEGATE_DEPTH` | Current delegation depth (recursion guard). |
| `AIMEE_DELEGATE_HEARTBEAT_MONITOR` | Enable the delegate heartbeat monitor. |
| `AIMEE_DELEGATE_SOURCE_AUTHORITY` | Enable source-authority gating for delegate edits. |
| `AIMEE_DELEGATE_SOURCE_PATHS` | Allowed source paths for delegate edits. |
| `AIMEE_DELEGATE_WORKTREE_ROOT` | Root directory for delegate worktrees. |
| `AIMEE_DOCKER_BIN` | Docker delegate-backend binary. |
| `AIMEE_DOCKER_WORKDIR` | Docker delegate-backend working directory. |
| `AIMEE_OPENCODE_BIN` | opencode CLI frontend binary. |
| `AIMEE_PARENT_DELEGATION_ID` | Parent delegation id (threading). |
| `AIMEE_SSH_BIN` | SSH delegate-backend binary. |

### Forge (GitHub App / tokens)

| Variable | Description |
|----------|-------------|
| `AIMEE_FORGE_API_BASE` | Forge API base URL. |
| `AIMEE_FORGE_APP_ID` | GitHub App id for minting forge tokens. |
| `AIMEE_FORGE_APP_INSTALLATION_ID` | GitHub App installation id. |
| `AIMEE_FORGE_APP_PRIVATE_KEY` | GitHub App private key (PEM or path). |
| `AIMEE_FORGE_SCOPE` | Scope for the minted forge token. |
| `AIMEE_FORGE_TOKEN` | Static forge access token (bypasses App auth). |

### Gateway (voice / webhooks / push)

| Variable | Description |
|----------|-------------|
| `AIMEE_GATEWAY_NTFY_BASE_URL` | ntfy push base URL. |
| `AIMEE_GATEWAY_NTFY_TOKEN` | ntfy push token. |
| `AIMEE_GATEWAY_STT_MODEL` | Speech-to-text model. |
| `AIMEE_GATEWAY_STT_PROVIDER` | Speech-to-text provider. |
| `AIMEE_GATEWAY_TTS_BASE_URL` | Text-to-speech base URL. |
| `AIMEE_GATEWAY_TTS_MODEL` | Text-to-speech model. |
| `AIMEE_GATEWAY_TTS_PROVIDER` | Text-to-speech provider. |
| `AIMEE_GATEWAY_TTS_VOICE` | Text-to-speech voice. |
| `AIMEE_GATEWAY_WEBHOOK_DELIVER_ONLY` | Webhook deliver-only mode (no reply path). |
| `AIMEE_GATEWAY_WEBHOOK_INSECURE` | Allow the webhook listener without TLS (dev). |
| `AIMEE_GATEWAY_WEBHOOK_PORT` | Inbound webhook listener port. |
| `AIMEE_GATEWAY_WEBHOOK_SECRET` | Inbound webhook HMAC secret. |

### Workflow engine

| Variable | Description |
|----------|-------------|
| `AIMEE_WORKFLOW_BASE` | Base branch for the engine's freeze/diff. |
| `AIMEE_WORKFLOW_REPO` | Local repository directory the workflow engine operates on. |

### Git verify / MCP

| Variable | Description |
|----------|-------------|
| `AIMEE_MCP_CWD` | Working-directory hint for MCP git-root resolution. |
| `AIMEE_VERIFY_PARALLEL` | Run `aimee git verify` steps in parallel. |
| `AIMEE_VERIFY_STEP_TIMEOUT_MS` | Per-step timeout (ms) for git verify. |

### Models

| Variable | Description |
|----------|-------------|
| `AIMEE_MODEL_CAPABILITY_OVERRIDES` | Override model capability flags (reasoning/tools/vision/…). |

### TLS & networking

| Variable | Description |
|----------|-------------|
| `AIMEE_NET_DEBUG` | Verbose network debug logging. |
| `AIMEE_TLS_INSECURE` | Disable TLS certificate verification (development only). |

### Plugins

| Variable | Description |
|----------|-------------|
| `AIMEE_ENABLE_PROJECT_PLUGINS` | Allow loading project-local plugins. |

### Diagnostics & misc

| Variable | Description |
|----------|-------------|
| `AIMEE_ANTIPATTERNS_BYPASS` | Bypass the guardrail antipattern checks. |
| `AIMEE_LOG_LEVEL` | Log level: `error` | `warn` | `info` | `debug`. |

### Undocumented (add to `ENV_DESC` in gen-reference-docs.py)

> These are read by the code but have no description yet — the generator surfaces them so the reference can't silently fall behind.

`AIMEE_DB2_POOL_SIZE`, `AIMEE_DELEGATE_MAX_INFLIGHT`, `AIMEE_TLS_CLIENT_P12_PASS`

## External & provider environment

Standard and third-party environment variables aimee honors (scanned non-`AIMEE_*` `getenv()` reads, plus provider keys resolved via `api_key_env`). Provider API keys are credentials — prefer the credential vault; the env var is the per-provider fallback and its name is overridable per agent via `api_key_env`. Standard OS variables (`HOME`, `PATH`, `TMPDIR`, `XDG_*`, …) are used for their usual purposes and are not aimee configuration.

### Provider credentials

| Variable | Description |
|----------|-------------|
| `ANTHROPIC_API_KEY` | Anthropic API key (read via the agent's `api_key_env`). |
| `GEMINI_API_KEY` | Google Gemini API key (read via the agent's `api_key_env`). |
| `GEMINI_API_KEY_AUTH_MECHANISM` | Selects the Gemini key auth mechanism. |
| `GOOGLE_API_KEY` | Google API key fallback for Gemini (via `api_key_env`). |
| `MINIMAX_API_KEY` | MiniMax API key. |
| `MISTRAL_API_KEY` | Mistral API key. |
| `OPENAI_API_KEY` | OpenAI API key (default for OpenAI-family agents). |
| `OPENROUTER_API_KEY` | OpenRouter API key. |

### Provider endpoints

| Variable | Description |
|----------|-------------|
| `LLAMA_HOST` | llama.cpp server host/URL. |
| `OLLAMA_HOST` | Ollama server host/URL for local models. |

### Reasoning effort

| Variable | Description |
|----------|-------------|
| `CODEX_REASONING_EFFORT` | Reasoning-effort passed through the Codex frontend. |
| `OPENAI_REASONING_EFFORT` | Reasoning-effort default for OpenAI-family models. |

### Network / proxy

| Variable | Description |
|----------|-------------|
| `HTTPS_PROXY` | HTTPS proxy for outbound provider/API calls. |
| `NO_PROXY` | Hosts excluded from proxying. |

### Editor

| Variable | Description |
|----------|-------------|
| `EDITOR` | Editor invoked for interactive edits. |
| `VISUAL` | Editor invoked for interactive edits (preferred over `EDITOR`). |

### Codex / Claude integration

| Variable | Description |
|----------|-------------|
| `CLAUDE_SESSION_ID` | Claude Code session id when aimee runs as its backend. |
| `CODEX_CWD` | Working directory reported by the Codex frontend. |
| `CODEX_HOME` | Codex home directory (Codex-frontend integration). |
| `CODEX_MODEL` | Model the Codex frontend requests. |
| `CODEX_SANDBOX` | Codex sandbox mode. |
| `CODEX_THREAD_ID` | Codex conversation/thread id. |

### Undocumented (add to `EXT_DESC`/`EXT_OS_IGNORE` in gen-reference-docs.py)

`LLM_API_KEY`, `LLM_ENDPOINT`, `LLM_MODEL`

## Workflow engine

Workflows are block-composed YAML definitions under `$AIMEE_HOME/workflows/<name>.yaml`, authored with the `aimee workflow` CLI or the web visual composer and saved in canonical form. A run is a DB1 work item pinned to a definition version.

### Workflow definition schema

```yaml
name: <id>                 # workflow name
start: <node-id>           # entry node (default: first node)
nodes:
  - id: <node-id>          # unique within the workflow
    block: <block-name>    # a built-in or custom block (see catalog)
    in:                    # typed input bindings (map: slot -> producer.output)
      <slot>: <node-id>.<output>
    params: { ... }        # block-specific params (see below)
    next: <node-id>        # unconditional successor
    on_pass: <node-id>     # gate verdict pass edge
    on_fail: <node-id>     # gate verdict fail edge (loop-back)
```

### Built-in block catalog

| Block | Produces | Accepts inputs |
|-------|----------|----------------|
| `author.proposal` | `proposal` | _(source: none)_ |
| `author.plan` | `plan` | `proposal` |
| `implement` | `branch` | `plan` |
| `document` | `branch` | `branch` |
| `freeze` | `frozen_diff` | `branch` |
| `gate.roundtable` | `verdict` | `proposal`, `plan`, `frozen_diff` |
| `gate.human` | `approval` | `proposal`, `plan`, `branch`, `frozen_diff`, `pr` |
| `pr.open` | `pr` | `proposal`, `frozen_diff` |
| `merge` | `none` | `pr` |
| `gate.ci` | `verdict` | `pr` |
| `check.mergeable` | `verdict` | `pr` |

### Block parameters (`params:`)

- **`gate.roundtable`** — `panel.required` (list of required reviewer personas), `panel.eligible` (list of additional eligible personas), `quorum` (int; effective quorum is `max(2, quorum)` and at least the required-panel size).
- **`gate.human`** — `policy: preauthorized` (auto-approve in autonomous mode) and/or `optional: true` (skippable). Without these, an autonomous run parks at the gate for a human.
- Other blocks take no params today; unknown params are ignored by the validator.

### Custom blocks — `$AIMEE_HOME/workflows/blocks.yaml`

Operator-owned (refused if a symlink or group/world-writable). Adds blocks to the catalog above:

```yaml
allow_command: false       # opt-in gate for the `command` executor (no-shell, argv-only)
blocks:
  - name: <block-name>     # must not shadow a built-in or duplicate
    consumes: <artifact>   # input artifact type, or none (a source)
    produces: branch|none  # custom blocks may NOT mint verdict/approval/pr
    executor: command|delegate
    command: [ argv0, arg1, ... ]   # executor: command (run in the repo, no shell)
    persona: <name>        # executor: delegate
    prompt: <text>         # executor: delegate
```

### Run-level controls (not in the definition)

- **Per-stage loop cap** — a gate that loops back via `on_fail` is retried at most `20` times (`WFE_MAX_ATTEMPTS`) before the run parks; fixed, not configurable.
- **Gate-override cap** — a parked human gate may be overridden at most `2` times (`WFE_MAX_OVERRIDES`) before the run is forced terminal.
- **Cost cap** — an optional per-work-item USD ceiling set at run creation (`work_item_max_cost_usd`); the engine parks the run when cumulative cost reaches it.
- **Trigger / autonomy mode** — `interactive` vs `autonomous`, set when the run is created.

### Workflow environment overrides

`AIMEE_WORKFLOW_REPO` (repo the engine operates on) and `AIMEE_WORKFLOW_BASE` (base branch for freeze/diff) — see Environment variables above.

## Other configuration files

Beyond the config store, aimee reads a few standalone JSON/policy files (paths under `$AIMEE_HOME` unless an env override is set).

### `agents.json` — agent / model definitions

`{"default_agent": "<name>", "agents": [ {<agent>}, … ]}`. Each agent object's fields (scanned from `src/server/agent_config.c`):

| Field | Description |
|-------|-------------|
| `access_token` | Static auth token for the endpoint. |
| `agents` | Top-level: array of agent definitions. |
| `api_key` | Inline API key (prefer `api_key_env` or the vault). |
| `api_key_env` | Env var name holding the agent's API key. |
| `auth_cmd` | Command that prints an auth token. |
| `auth_type` | Auth scheme (bearer / oauth / none). |
| `auto_compact_pct` | Context % at which to auto-compact. |
| `backend` | Execution backend (http / cli / ssh / docker). |
| `cidr` | Allowed CIDR (relay / tunnel networking). |
| `cli_cmd` | CLI command for a cli-backend agent. |
| `cli_idle_timeout_ms` | Idle timeout (ms) for a CLI agent. |
| `cli_kind` | CLI agent kind (claude / codex / opencode). |
| `context_warn_pct` | Context % at which to warn. |
| `context_window` | Model context window (tokens). |
| `cost_limit` | Per-agent cost cap (USD). |
| `cost_tier` | Cost-tier label for routing. |
| `credentials` | Credential block / reference. |
| `default_agent` | Top-level: name of the default agent. |
| `desc` | Human description of the agent. |
| `enabled` | Whether the agent is active. |
| `endpoint` | Provider endpoint URL. |
| `exec_roles` | Roles this agent may execute with tools. |
| `exec_system_prompt` | System prompt for exec/tool runs. |
| `extra_headers` | Extra HTTP headers for requests. |
| `fallback_chain` | Ordered fallback agent chain. |
| `fallback_model` | Fallback model on failure. |
| `hosts` | Allowed hosts (relay / tunnel). |
| `inject_respond_tool` | Inject the `respond` tool. |
| `ip` | Bind/target IP (relay / tunnel). |
| `max_parallel` | Max concurrent calls to this agent. |
| `max_reconnects` | Max reconnect attempts (streaming / relay). |
| `max_tokens` | Max output tokens. |
| `max_turns` | Max agent-loop turns. |
| `middleware` | Per-agent middleware overrides (e.g. `context_window`, `max_tokens`). |
| `model` | Model name. |
| `name` | Agent identifier. |
| `network` | Network mode (backend sandbox). |
| `networks` | Allowed networks. |
| `port` | Target port (relay / tunnel). |
| `provider` | Provider name. |
| `recommended_sampling` | Provider-recommended sampling parameters. |
| `reconnect_delay` | Delay between reconnects (ms). |
| `relay_key` | Relay auth key. |
| `relay_ssh` | SSH relay config. |
| `roles` | Roles this agent serves (review, plan, …). |
| `session_reuse` | Reuse a session across calls. |
| `ssh_entry` | SSH entry point (ssh backend). |
| `ssh_key` | SSH key path (ssh backend). |
| `stall_threshold` | Stall-detection threshold. |
| `target_host` | Target host (relay / tunnel). |
| `target_port` | Target port (relay / tunnel). |
| `timeout_ms` | Per-call timeout (ms). |
| `tokens` | Token budget / accounting block. |
| `tools_enabled` | Allow tool use for this agent. |
| `tunnel` | Tunnel config. |
| `tunnels` | Tunnel definitions. |
| `user` | Remote user (ssh backend). |

> **Undocumented agent fields** (add to `AGENT_FIELD_DESC`): `is_server_hosted`

### Toolsets — `AIMEE_TOOLSETS_CONFIG` (or the config `toolsets` map)

Named tool allowlists. `{"toolsets": {"<name>": { … }}}`; each toolset:

- `tools` / `allowed_tools` — the tool names the set permits.
- `include` — inherit another toolset's tools.
- `script` — script-tool configuration for the set.

### Guardrails — `AIMEE_GUARDRAILS_PATH`

A policy file governing path read/write classification and pre-tool enforcement (antipattern blocking). It is a behavioral policy rather than a flat key schema; the tunable thresholds are exposed as the `guardrails` section + `guardrails_semantic_*` / `guardrail_mode` keys documented above.

## Coverage & limitations

This reference is generated by scanning the canonical source tables, which covers the scalar/keyed config surface but has known blind spots — listed here so a reader can tell *deliberately out of scope* from *not auto-derived*:

- **Array/object element fields** are captured when the parser iterates with `cJSON_ArrayForEach` over a section's array; fields read through other access patterns (`cJSON_GetArrayItem`, indexing) or nested more than one object deep may appear only under their parent section name.
- **Env vars built at runtime** (a name assembled with `snprintf`/concatenation and passed to `getenv(var)`) are not discoverable by the string-literal scan. Provider API-key vars are the known case and are handled via each agent's `api_key_env`; only the common defaults are listed.
- **Compile-time `-D` defines** used as build-level configuration are not scanned (they are not runtime-overridable config).
- **Separate config files** — `agents.json`, toolsets, guardrails, and custom workflow blocks (`blocks.yaml`) / workflow definitions are documented in their own sections above. Per-agent field set is scanned from `agent_config.c`; the guardrails *policy* is behavioral (path classification + pre-tool enforcement), with its tunables exposed as config keys.

If the scan ever finds a config var with no description, it is emitted under an **Undocumented** heading in the relevant section — so a new option cannot silently bypass this reference.
