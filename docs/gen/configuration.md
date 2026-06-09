# Configuration Reference

> Auto-generated from `src/config_fields.c` and the `src/config*.c` parsers by `scripts/gen-reference-docs.py`. Do not edit by hand; run `make -C src docs-gen` to regenerate.

Configuration lives in the per-`AIMEE_HOME` config store. Scalar keys in the table below are settable from the CLI:

```
aimee config show                 # print the effective config
aimee config get <key>            # read one value
aimee config set <key> <value>    # set one value
```

Structured options (arrays, nested objects — e.g. `ensemble.reference_models`) are not CLI-settable; they are written into the config file under the sections listed at the end.

## CLI-settable keys (87)

| Key | Type |
|-----|------|
| `autonomous` | bool |
| `cache_aware_rewrite_enabled` | bool |
| `claude_model` | string |
| `cross_verify` | bool |
| `db2_url` | string |
| `dogfood_autolabel_continuation` | bool |
| `dogfood_autolabel_repair` | bool |
| `dogfood_autolabel_repeat_question` | bool |
| `dogfood_commit_raw` | bool |
| `dogfood_enabled` | bool |
| `dogfood_inline_tagging` | bool |
| `dogfood_log_dir` | string |
| `ecomode` | bool |
| `embedding_command` | string |
| `embedding_dim` | int |
| `embedding_endpoint` | string |
| `embedding_model` | string |
| `guardrail_mode` | string |
| `guardrails_semantic_allow_ml_only_block` | bool |
| `guardrails_semantic_block_threshold` | float |
| `guardrails_semantic_command` | string |
| `guardrails_semantic_dry_run` | bool |
| `guardrails_semantic_enabled` | bool |
| `guardrails_semantic_prompt_threshold` | float |
| `guardrails_semantic_warn_threshold` | float |
| `identity_working_profile_injection_enabled` | bool |
| `ingress_preinject_enabled` | bool |
| `integrity_dry_run` | bool |
| `integrity_enabled` | bool |
| `kb_api_bearer_token` | string |
| `kb_api_http_port` | int |
| `kb_mining_enabled` | bool |
| `kb_mining_min_poll_s` | int |
| `kb_search_max_results` | int |
| `learning_implicit_citation_continuation` | bool |
| `learning_implicit_citation_repair` | bool |
| `learning_implicit_repeat_question` | bool |
| `learning_implicit_repeated_correction` | bool |
| `learning_implicit_workflow_repetition` | bool |
| `learning_max_commits_per_week` | int |
| `learning_proposal_ttl_days` | int |
| `learning_router_enabled` | bool |
| `max_iterations` | int |
| `max_iterations_delegate` | int |
| `memory_bm25_weight` | float |
| `memory_coref_mode` | string |
| `memory_coref_window` | int |
| `memory_fetch_budget_base` | int |
| `memory_fetch_budget_enabled` | bool |
| `memory_fetch_budget_shape_aware` | bool |
| `memory_hard_negative_log` | string |
| `memory_kb_neighbour_expand` | bool |
| `memory_maintenance_trigger_inserts` | int |
| `memory_maintenance_trigger_secs` | int |
| `memory_negation_enabled` | bool |
| `memory_profile_cards_enabled` | bool |
| `memory_profile_cards_min_obs` | int |
| `memory_profile_cards_stale_secs` | int |
| `memory_query_expansion_k` | int |
| `memory_query_expansion_k` | int |
| `memory_query_expansion_mode` | string |
| `memory_query_expansion_mode` | string |
| `memory_rerank_command` | string |
| `memory_rerank_command` | string |
| `memory_rerank_enabled` | bool |
| `memory_rerank_enabled` | bool |
| `memory_rerank_mode` | string |
| `memory_rerank_top_k` | int |
| `memory_rerank_top_k` | int |
| `memory_rewrite_command` | string |
| `memory_rewrite_decompose` | bool |
| `memory_rewrite_enabled` | bool |
| `memory_rewrite_hyde` | bool |
| `memory_rewrite_max_subqueries` | int |
| `memory_scenes_enabled` | bool |
| `memory_scenes_min_cluster_size` | int |
| `memory_scenes_top_m` | int |
| `memory_semantic_weight` | float |
| `memory_window_radius` | int |
| `openai_endpoint` | string |
| `openai_key_cmd` | string |
| `openai_model` | string |
| `provider` | string |
| `verify_cross_project` | bool |
| `verify_enabled` | bool |
| `virtual_context_assembly_budget` | int |
| `virtual_context_enabled` | bool |

## Config-file sections (38)

Set in the config JSON as `{"<section>": {"<key>": ...}}`. Keys are derived from the section parsers in `src/config*.c`.

- **`aimee`** — `api`
- **`auxiliary`** — `default_max_tokens`, `default_model`, `default_provider`, `enabled`, `tasks`
- **`charter`** — `hard_constraints`, `safety_axioms`, `tone_boundaries`, `values`, `working_profile_drift_limit`
- **`compact`** — `enabled`, `head_bytes`, `per_tool`, `tail_bytes`, `threshold`
- **`computer_use`** — `allowed_domains`, `default_navigation`, `enabled`, `redact_sensitive_screenshots`
- **`concurrency`** — `default`, `per_model`, `per_provider`, `preempt`
- **`context`** — `engine`
- **`cross_verify`** — `enabled`, `prompt`, `role`, `verify_cmd`
- **`db2`** — `vector`
- **`dogfood`** — `commit_raw`, `enabled`, `inline_tagging`, `log_dir`
- **`ensemble`** — `aggregator`, `enabled`, `max_cost_usd`, `min_successful`, `reference_models`
- **`guardrails`** — `semantic`
- **`identity`** — `working_profile_injection`
- **`integrity`** — `dry_run`, `enabled`
- **`intelligence`** — `bandit`, `bandit_optimize_command`, `calibrate`, `constraint_solver_command`, `demotion`, `kb`, `planner`, `planner_search_command`, `ranker_fuse_command`, `ranking`, `reasoning`, `reasoning_datalog_command`, `synthesize`
- **`kb`** — `api`, `background_ingest`, `connection_workers`, `curator`, `evidence`, `maintenance`, `mining`, `search_max_results`, `worker_count`
- **`learning`** — `embed`, `router`, `synthesize`
- **`mcp`** — `osv`
- **`memory`** — `aggregation`, `bm25_weight`, `briefing`, `citations`, `cognify`, `context_budget`, `coref`, `derive_facts`, `directives`, `dispositions`, `episode_summaries`, `failure_detection`, `fetch_budget`, `hard_negative_log`, `improve`, `lifecycle`, `pagerank`, `profile_cards`, `prospective`, `recall`, `rewrite`, `routing`, `salience`, `scenes`, `semantic_weight`
- **`memory_maintenance`** — `enabled`, `interval_seconds`, `summarize_enabled`, `trigger_inserts`, `trigger_secs`
- **`memory_negation`** — `enabled`
- **`memory_query_expansion`** — `k`, `mode`
- **`memory_recall_lanes`** — `enabled`, `fact_kinds`, `floor_fact`, `floor_summary`, `k_fact`, `k_summary`, `summary_kinds`
- **`memory_rerank`** — `command`, `enabled`, `mix`, `top_k`
- **`memory_rewrite`** — `command`, `decompose`, `enabled`, `hyde`, `max_subqueries`
- **`memory_window`** — `kb_neighbour_expand`, `radius`
- **`model_meta`** — `capability_routing`, `refresh_minutes`
- **`otel`** — `endpoint`, `service_name`
- **`retry`** — `base_ms`, `max_attempts`, `max_ms`
- **`rewind`** — `auto_snapshot`
- **`roundtable`** — `converge_threshold`, `deadline_ms`, `max_rounds`, `turns`
- **`sandbox`** — `allow_paths`, `mode`, `network`
- **`script`** — `allowed_tools`
- **`search`** — `backend`, `max_results`, `searxng_url`, `tavily_api_key`
- **`session`** — `max_sessions`, `max_worktrees`, `stale_threshold_secs`, `virtual_context`
- **`skills`** — `capability`, `curator`, `dispatch`, `eval`, `manage`, `review`
- **`transport`** — `cache_aware_rewrite`
- **`trigger`** — `auth_token`, `max_concurrent`

## Other top-level config-file keys (11)

Scalar keys read directly from the config root (not via the CLI allowlist above):

`cron_jobs`, `db2_pool_size`, `db2_url`, `kb_client_bearer_token`, `kb_client_url`, `lsp_servers`, `mcp_clients`, `proxy_token`, `toolsets`, `trigger_rules`, `workspaces`
