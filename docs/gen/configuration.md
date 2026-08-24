# Configuration Reference

> Auto-generated from the canonical source tables by `scripts/gen-reference-docs.py`: config keys from `src/modules/config/config_fields.c` + `src/config*.c`, env vars scanned from `getenv()` in `src/` and `os.Getenv()` in `server-go/`, and the workflow catalog from `server-go/internal/wfe/catalog.go`. Do not edit by hand; run `make -C src docs-gen` to regenerate.

This reference covers every configurable surface:

1. **Config-store keys**: the `aimee config` keys + config-file sections (below).
2. **Environment variables**: `AIMEE_*` runtime/deployment overrides.
3. **External & provider environment**: provider keys, endpoints, proxy, editor.
4. **Workflow engine**: workflow definition + custom-block (`blocks.yaml`) schema.
5. **Other config files**: `agents.json`, toolsets, guardrails.

CLI commands + flags are documented separately in [`cli-commands.md`](cli-commands.md).

Configuration lives in the per-`AIMEE_HOME` config store. Scalar keys in the table below are settable from the CLI:

```
aimee config show                 # print the effective config
aimee config get <key>            # read one value
aimee config set <key> <value>    # set one value
```

Structured options (arrays, nested objects: e.g. `ensemble.reference_models`) are not CLI-settable; they are written into the config file under the sections listed at the end.

## CLI-settable keys (93)

The everyday runtime surface. Deploy-time, advanced-tuning, and dev-only keys are still `aimee config set`-able but are filed into their own subsections below (and hidden from the Settings surface by default).

| Key | Type | Description |
|-----|------|-------------|
| `aimee_synthesis_model` | string | n/a |
| `aimee_with_llamacpp` | string | Whether THIS IMAGE bundles llama.cpp ("1" on the aimee-kb-*-llm variants). Set by the Dockerfile, not by an operator: it is a fact about the running image, and the setup wizard reads it to decide whether the local synthesis models can be offered at all. |
| `audit_action_enabled` | bool | Publish governed tool-action audit rows (default on); disabling it creates an audit coverage gap. |
| `audit_worm_enabled` | bool | Dual-write governed-action audit rows into the append-only, hash-chained WORM store alongside audit.log (default off). |
| `autonomous` | bool | Legacy mode flag. The current Go WFE records run mode at admission but does not branch scheduler behavior on it; human gates always park. |
| `cache_aware_rewrite_enabled` | bool | Rewrite prompts to align with the provider's prompt cache. |
| `cache_shaping_enabled` | bool | Enable prompt cache-shaping. |
| `claude_model` | string | Default Claude model (empty = CLI default). |
| `client_integrations_enabled` | bool | Auto-register aimee (MCP server, hooks, slash commands) into detected AI-tool user configs: Claude Code (~/.claude), Gemini, Copilot, Codex. Default-ON; set false, or export AIMEE_NO_CLIENT_INTEGRATIONS, to keep aimee out of every tool's global config and wire a single project by hand. |
| `client_tool_transport_preference` | string | n/a |
| `code_cochange_git_enabled` | bool | Mine git history at `index scan` time into co_edited edges (files that change together in a commit), which blast radius already reads. Incremental and idempotent via a per-project HEAD marker; bulk commits (>25 code files) are skipped. Default on. |
| `code_trust_actuation_enabled` | bool | Use earned code-graph trust lessons only as an equal-score retrieval tiebreak (default off). |
| `cost_reward_enabled` | bool | Factor token cost into the reward signal. |
| `cost_reward_ref_usd_milli` | int | Reference cost (USD-milli) normalizing the cost reward. |
| `cross_verify` | bool | Enable cross-model verification of outputs. |
| `css_render_command` | string | Render backend for the #4-full computed-style oracle: a command reading {html,css} JSON on stdin and writing a computed-style snapshot JSON on stdout (run an isolated headless-browser sidecar). |
| `css_style_graph_enabled` | bool | Enable the CSS migration assistant's style-graph write path during indexing. |
| `db2_url` | string | Vault-backed DB2 connection URL; reads are redacted and writes bypass YAML. |
| `dedup_enabled` | bool | Deduplicate near-identical responses. |
| `dedup_window_seconds` | int | Window (seconds) for response dedup. |
| `default_persona` | string | Persona a fresh primary session starts as, and the persona draft roundtable panelists author with when none is set (default 'engineer'). |
| `delegate_graph_context_enabled` | bool | Prepend a structural code-graph context block (callers/dependencies of files a delegate task references) to the delegate prompt (advisory, fail-open, default off). |
| `delegate_sandbox_learn_packages` | bool | Learned toolchain for delegate sandboxes (default on). aimee captures the apt packages a delegate installs inside its `--network none` sandbox, records them per project (git root), and pre-bakes the learned set into that project's next sandbox image build. It augments a declared `.aimee/project.yaml` from+packages spec, or synthesizing one (FROM the resolved base + the learned packages) when none is declared. Best-effort: a learned build that fails falls back to the un-augmented image. The first delegate turn after a new package is learned pays a one-time image build. |
| `delegate_sandbox_package_access` | string | Runtime package-access policy for a `--network none` delegate sandbox. aimee always performs and logs the fetch (the delegate holds no outside socket); this selects how much: `proxy` (default) proxies package-manager fetches to any host through aimee for out-of-the-box functionality; `off` no runtime proxy (build-time installs + learned pre-bake only); `gated` host-allowlisted registries, off-allowlist requires human approval; `governance` allowlist from a governance provider, off-allowlist refused. |
| `delegate_sandbox_require_isolation` | bool | Fail-closed guard for the `--network none` delegate sandbox (default off). aimee always passes `--network none`, but some runtimes ignore it and give the sandbox real egress, defeating the package-access proxy. After the container starts aimee asks the host daemon whether a network with an IP is attached and always logs an error on a breach; when this is set, sandboxing is mandatory. A delegate always runs in its own container -- there is no in-process host path to fall back to -- and this additionally refuses on a breach or an unverifiable probe. |
| `embedder_api_key` | string | Bearer token for an external embedder endpoint (blank if it needs none). |
| `embedder_command` | string | Command that produces embeddings (overrides the endpoint). |
| `embedder_dims` | int | Embedding vector width. Leave unset for a bundled embedder - it declares its own width and the kb derives it (pinned > recorded > probed). REQUIRED for an external endpoint, whose width cannot be derived; valid to 4000, the DB2 column ceiling. A ONE-WAY DOOR once anything is embedded: DB2 records the width and refuses to start on drift. |
| `embedder_model` | string | Embedder identity. Written for a bundled model too, not just an external one: it is the registry key pooling and prefixes resolve from, and the value recorded against the corpus. |
| `embedder_url` | string | External embedder endpoint. A non-empty value IS the external embedder; empty means the model baked into this image variant (bekko-a25m at 384, or nomic-v2 at 768 on the -nomic images). |
| `extended_thinking_enabled` | bool | Ask for extended thinking on aimee's OWN Anthropic requests (default off). Sends the adaptive thinking config, and only to a model whose capabilities report that it accepts it -- a model nobody has reported that for is left alone rather than sent a shape the provider would reject. Off by default because thinking tokens are billed: enabling it changes spend, not just visibility. |
| `fidelity_check_enabled` | bool | Run the answer-fidelity judge on terminal-text turns (default off; requires kb_evidence_emit_enabled + ingress_preinject_enabled). |
| `gateway_pin_model` | bool | Gateway forces the proxied /v1/messages served model to the configured primary's model, overriding the client-requested model. Default off (the passthrough honors the client model); enable for single-model Anthropic-compatible shims. |
| `gateway_prevent_subagents` | bool | Gateway strips subagent-spawning tools (Task/Agent/etc.) from proxied requests so the served model cannot spawn subagents. Default off. |
| `guardrail_mode` | string | Guardrail enforcement mode: approve (default; a tool call needs approval, so an unattended delegate is blocked), prompt, or deny. |
| `guardrails_blast_radius_advisory_enabled` | bool | Surface a structural blast-radius advisory (graph-impacted files) before an edit (advisory, fail-open). |
| `guardrails_semantic_command` | string | External semantic-guardrail classifier command. |
| `guardrails_semantic_mode` | string | Semantic guardrail mode: off, dry_run, advisory, or enforce. |
| `identity_working_profile_injection_enabled` | bool | Inject the working-profile identity into prompts. |
| `ingress_audit_async` | bool | Audit ingress requests asynchronously. |
| `ingress_max_raw_scans` | int | Max raw-content scans per ingress request. |
| `ingress_preinject_assembly_budget` | int | Token budget for ingress context pre-injection. |
| `ingress_preinject_enabled` | bool | Enable `<aimee-context>` pre-injection on ingress (memory/code preview envelope on primary ingress turns; default on). |
| `ingress_trusted_proxy_secret` | string | Vault-backed shared secret for a trusted ingress proxy; reads are redacted. |
| `integrity_dry_run` | bool | Run integrity checks without enforcing. |
| `integrity_enabled` | bool | Enable the integrity gate. |
| `kb_api_bearer_token` | string | Vault-backed bearer for the aimee-kb API; reads expose configured state only. |
| `kb_api_http_port` | int | HTTP port the aimee-kb API listens on. |
| `kb_client_bearer_token` | string | Vault-backed server-to-KB bearer; reads are redacted; restart required. |
| `kb_client_url` | string | Remote aimee-kb API base URL used by aimee-server; restart required. |
| `kb_curator_extract_code_workers` | int | Parallel curator code-extraction workers, bounded to the synthesis service slot count. |
| `kb_curator_extract_docs_workers` | int | Parallel curator document-extraction workers, bounded to the synthesis service slot count. |
| `kb_curator_tier` | string | KB curator pipeline preset: off | lite (core extract+index) | full (all stages, default). |
| `kb_evidence_embed_enabled` | bool | Drain evidence-index operations into evidence vectors. |
| `kb_evidence_emit_enabled` | bool | Emit evidence records from KB ingest. |
| `kb_fusion_mode` | string | KB retrieval fusion mode: rrf (default), static_alpha, or dynamic_alpha. |
| `kb_mining_enabled` | bool | Enable background KB mining. |
| `kb_mode` | string | Setup/deploy mode: local starts a KB, remote connects to kb_client_url. |
| `kb_pdf_tier` | string | Structured-PDF pipeline preset: off (plain pdftotext, default) | basic (ingest+vector) | full (all stages). |
| `learning_router_enabled` | bool | Enable the learning router. |
| `max_iterations` | int | Per-turn iteration cap for interactive chat (default 15). |
| `max_iterations_delegate` | int | Per-turn iteration cap for delegate sessions (default 25). |
| `memory_coref_mode` | string | Coreference-resolution mode for memory. |
| `memory_negation_enabled` | bool | Detect/handle negation in memory. |
| `memory_query_expansion_mode` | string | Query-expansion mode. |
| `memory_rerank_mode` | string | Reranker mode. |
| `memory_rewrite_command` | string | External query-rewrite command. |
| `memory_rewrite_enabled` | bool | Enable query rewriting for recall. |
| `ocr_command` | string | OCR sidecar endpoint/command for structured-PDF scanned-page recognition (resolves like embedder_command; AIMEE_OCR_URL env fallback). |
| `openai_endpoint` | string | OpenAI-compatible endpoint URL. |
| `openai_key_cmd` | string | Command that prints the OpenAI API key. |
| `openai_model` | string | OpenAI model name. |
| `provider` | string | Default model provider. |
| `reasoning_cap_enabled` | bool | Cap the model's reasoning effort. |
| `require_aimee_git` | bool | Block a delegate from running `git` or `gh` in a shell (reads included) and redirect git/forge work to aimee's `git_*` tools, which execute on aimee-server where the forge credential stays in-process; delegates are also spawned without git/gh credentials. Note the env strip also drops SSH_AUTH_SOCK (no agent-backed SSH to any host) and neuters the global/system git config (default on). |
| `require_aimee_memory` | bool | Block agent writes to external file-based agent-memory stores (~/.claude/projects/<slug>/memory/...) and redirect durable memories into aimee's memory system via `aimee memory store` (default on). |
| `require_session_worktree` | bool | Fail closed on mutating ops outside an aimee-managed worktree (session-isolation guard; default off). |
| `subagent_ban_enabled` | bool | Prevent provider-native sub-agent tools when an aimee delegate is available, and install the matching client guardrails (default on). |
| `synthesis_api_key` | string | Bearer token for the synthesis endpoint (blank for a keyless or loopback endpoint). |
| `synthesis_endpoint` | string | The ONE synthesis endpoint, remote or loopback. Empty means synthesis is off, which is supported - embedding, search, recall and indexing never call it. On a *-llm image the container entrypoint sets this to loopback itself after starting the bundled model. |
| `synthesis_model` | string | Synthesis model. On a *-llm image this selects the bundled model to fetch and serve (gemma-4-E2B-it or gemma-4-E4B-it); otherwise it is the model label sent to the configured endpoint. |
| `synthesis_thinking` | bool | Let the synthesis model think before answering (default on). It measured positive-to-neutral everywhere it was tried. Global rather than per-stage, and the operator's call: turn it off only for a model that reasons past its output budget without answering. |
| `tsr_command` | string | TSR sidecar endpoint/command for structured-PDF table recognition (resolves like embedding_command; AIMEE_TSR_URL env fallback). |
| `typed_facts_enabled` | bool | Enable the typed-fact knowledge layer (master gate; default off). |
| `verify_cmd` | string | Command run after a delegated fix to verify it. |
| `verify_cross_project` | bool | Let `aimee git verify` span other projects. |
| `verify_enabled` | bool | Master gate for `aimee git verify` (default off). |
| `verify_prompt` | string | Prompt template given to the cross-verification delegate. |
| `verify_role` | string | Delegate role used for cross-verification. |
| `virtual_context_assembly_budget` | int | Token budget for virtual-context assembly. |
| `virtual_context_enabled` | bool | Enable virtual-context assembly. |
| `wfe_live_forge_enabled` | bool | Gate for the autonomous live forge (default-ON). When off, the forge provider is not registered and every forge op fails closed, so an autonomous run can never open or merge a real PR. Even on, each op re-checks this flag and the merge-target rail. |
| `wfe_proposals_autoscan_enabled` | bool | Automatically scan watched proposal directories; off requires explicit trigger.fire. |

> **Undocumented** (add to `CFG_KEY_DESC` in gen-reference-docs.py): `aimee_synthesis_model`, `client_tool_transport_preference`

### Advanced tuning keys (85)

Expert scalars with sensible defaults; settable in the config file but off the everyday surface.

| Key | Type | Description |
|-----|------|-------------|
| `cache_min_chars` | int | Minimum prompt size (chars) before cache-shaping applies. |
| `code_context_mode` | string | Task-conditioned code packet rollout mode: `off` disables packet retrieval, `observe` retrieves and validates without changing model-visible bytes, and `on` injects a bounded current-project packet on first/new-task turns (default `on`). |
| `code_hybrid_rrf_k` | float | Reciprocal Rank Fusion rank constant k for /v1/code/hybrid (default 60). |
| `code_hybrid_weight_code` | float | RRF weight for the lexical-code signal in /v1/code/hybrid (default 1.0; <=0 disables it). |
| `code_hybrid_weight_graph` | float | RRF weight for the structural call-graph signal in /v1/code/hybrid (default 1.0; <=0 disables it). |
| `code_hybrid_weight_memory` | float | RRF weight for the cross-session knowledge-graph signal in /v1/code/hybrid (default 1.0; <=0 disables it; symbol-anchored, empty without an entity graph). |
| `code_hybrid_weight_vector` | float | RRF weight for the embedding-similarity signal in /v1/code/hybrid (default 1.0; <=0 disables it; auto-skips when no dim-matched embedder). |
| `code_span_max_lines` | int | Max line span the code_span_get recovery resolver returns per call (default 400). |
| `code_surprising_precision_floor` | float | §4 self-suppress: when the LLM-judge-sampled precision of surprising-link candidates falls below this floor, an unjudged /v1/code/graph/surprising request returns no candidates (default 0 = disabled). |
| `cost_reward_lambda_pct` | int | Cost-penalty weight (percent) in the reward. |
| `delegates_enabled` | bool | n/a |
| `feature_auto_promote` | bool | When every PR on a feature branch has merged, automatically open the feature -> default-branch PR as a draft (default on). Off leaves promotion to an explicit action. |
| `guardrails_semantic_block_threshold` | float | Semantic score threshold to block. |
| `guardrails_semantic_prompt_threshold` | float | Semantic score threshold for prompt-level flags. |
| `guardrails_semantic_warn_threshold` | float | Semantic score threshold to warn. |
| `ingress_cache_placement_enabled` | bool | Append the <aimee-context> envelope after the stable instructions prefix (not before) so provider prefix caches survive (default on). |
| `ingress_compress_enabled` | bool | Enable ingress envelope compression: span-enrich code hits and fold code entries into recoverable `file:line` references (recover via code_span_get). Default on (~48% prompt reduction on code turns); turn off (or send `X-Aimee-Compress: 0`) for agentic ingress where the agent re-opens folded code so recovery round-trips can erase the saving. |
| `ingress_compress_min_chars` | int | Minimum code-snippet length (chars) before it is folded to a file:line reference (default 80). |
| `ingress_preinject_anthropic_enabled` | bool | Inject the `<aimee-context>` envelope on the Anthropic-native /v1/messages passthrough too (default off). |
| `ingress_usage_accounting_enabled` | bool | Account token usage on ingress requests. |
| `kb_curator_cross_repo_graph_enabled` | bool | Resolve and maintain cross-repository dependency edges. |
| `kb_curator_custom_stages` | string | JSON definitions that recompose vetted curator operations with bounded budgets. |
| `kb_curator_detect_contradictions_enabled` | bool | Link claims that disagree on the same subject and attribute. |
| `kb_curator_extract_code_enabled` | bool | Extract typed curator artifacts from indexed code. |
| `kb_curator_extract_docs_enabled` | bool | Extract typed curator artifacts from documents. |
| `kb_curator_index_claims_enabled` | bool | Embed claim subject/attribute/value records. |
| `kb_curator_index_code_unit_enabled` | bool | Embed extracted code-unit artifacts. |
| `kb_curator_index_narrative_enabled` | bool | Embed summaries and synthesis narratives. |
| `kb_curator_link_artifacts_enabled` | bool | Link related document, entity, claim, and code artifacts. |
| `kb_curator_projection_graph_enabled` | bool | Publish the typed code projection graph for changed projects. |
| `kb_curator_promote_entity_enabled` | bool | Promote well-supported entities one step up the scope lattice. |
| `kb_curator_resolve_entities_enabled` | bool | Resolve proposed mentions against canonical entities. |
| `kb_curator_stage_order` | string | Comma-separated curator stage order; invalid dependency order falls back safely. |
| `kb_curator_synthesize_enabled` | bool | Create evidence-backed topic synthesis with the configured reasoning tier. |
| `kb_curator_user_presets` | string | JSON array of operator-defined curator stage presets. |
| `kb_fusion_static_alpha` | float | Lexical/dense blend weight (0-1) for the static_alpha fusion mode. |
| `kb_mining_min_poll_s` | int | Minimum interval (s) between KB mining polls. |
| `kb_pdf_assets_enabled` | bool | Render structured-PDF figure/table crops to the content-addressed blob store + kb_doc_assets at ingest, served via open_asset (default off; needs pdftoppm). |
| `kb_pdf_blob_dir` | string | Override the structured-PDF blob store root (default <kb-config-dir>/kb-blobs). |
| `kb_pdf_blob_orphan_alarm_mb` | int | Warn when reclaimable orphan blob bytes exceed this many MB (default 1024; <=0 disables the alarm). |
| `kb_pdf_blob_recon_secs` | int | Interval (seconds) for the orphan-blob reconciliation sweep (default 3600; <=0 disables it). |
| `kb_pdf_ingest_enabled` | bool | Route PDF uploads through the structured geometry extractor (kb_doc_pdf) instead of plain pdftotext (default off). |
| `kb_pdf_ocr_enabled` | bool | OCR a scanned / no-text-layer PDF via the OCR sidecar at ingest so its text + geometry feed the normal citation path (default off; without it a scanned PDF is ingested asset-only). |
| `kb_pdf_tsr_enabled` | bool | Run the table-structure-recognition (TSR) sidecar at PDF ingest to turn table regions into structured kb_table_cells, surfaced via lookup_table (default off; degrades to text-only when the sidecar is absent). |
| `kb_pdf_vector_enabled` | bool | Embed structured-PDF chunks into the isolated kb_pdf_embeddings relation and add the vector candidate leg to search_chunks (default off; degrades to lexical-only when the embedder is absent). |
| `kb_search_max_results` | int | Default max results for KB search. |
| `learning_implicit_citation_continuation` | bool | Implicit-learning signal: citation on continuation. |
| `learning_implicit_citation_repair` | bool | Implicit-learning signal: citation on repair. |
| `learning_implicit_repeat_question` | bool | Implicit-learning signal: repeated question. |
| `learning_implicit_repeated_correction` | bool | Implicit-learning signal: repeated correction. |
| `learning_implicit_retrieval_outcome` | bool | Bridge continuation/repair autolabels into retrieval outcomes (memory + ranker). |
| `learning_implicit_workflow_repetition` | bool | Implicit-learning signal: workflow repetition. |
| `learning_max_commits_per_week` | int | Cap on learning-derived commits per week. |
| `learning_proposal_ttl_days` | int | TTL (days) for learning proposals. |
| `memory_abstain_enabled` | bool | Allow memory recall to abstain on low confidence. |
| `memory_abstain_gate` | float | Confidence gate for memory abstention. |
| `memory_bm25_weight` | float | BM25 (lexical) weight in hybrid memory recall. |
| `memory_chunk_min_confidence` | float | Minimum confidence to keep a memory chunk. |
| `memory_coref_window` | int | Coreference lookback window. |
| `memory_fetch_budget_base` | int | Base token budget for memory fetch. |
| `memory_fetch_budget_enabled` | bool | Enable token-budgeted memory fetch. |
| `memory_fetch_budget_shape_aware` | bool | Shape-aware memory fetch budgeting. |
| `memory_hard_negative_log` | string | Path to the hard-negative recall log file (empty = disabled). |
| `memory_improve_dedupe_enabled` | bool | Dedupe during memory-improve. |
| `memory_improve_summarise_enabled` | bool | Summarise during memory-improve. |
| `memory_maintenance_trigger_inserts` | int | Inserts before a maintenance cycle triggers. |
| `memory_maintenance_trigger_secs` | int | Seconds before a maintenance cycle triggers. |
| `memory_profile_cards_enabled` | bool | Maintain profile cards from observations. |
| `memory_profile_cards_min_obs` | int | Min observations before a profile card forms. |
| `memory_profile_cards_stale_secs` | int | Profile-card staleness (seconds). |
| `memory_query_expansion_k` | int | Number of expanded queries for recall. |
| `memory_rewrite_decompose` | bool | Decompose queries during rewrite. |
| `memory_rewrite_hyde` | bool | Use HyDE (hypothetical-document) rewrite. |
| `memory_rewrite_max_subqueries` | int | Max sub-queries produced by rewrite. |
| `memory_scenes_enabled` | bool | Cluster memories into scenes. |
| `memory_semantic_floor_scale` | float | Multiplier on the semantic-recall cosine floors (0 = auto-scale by the active embedder dimension; >0 pins it). |
| `memory_semantic_weight` | float | Semantic (vector) weight in hybrid recall. |
| `memory_typed_facts_promote_threshold` | int | n/a |
| `memory_typed_facts_speculative_ttl_days` | int | n/a |
| `memory_window_radius` | int | Neighbour radius for memory-window expansion. |
| `pr_base_mode` | string | What a PR opened from a session targets. feature (default): the session's durable feature branch aimee/feat/<slug>, so a feature's slices accumulate on one branch and reach the default branch through a single reviewed PR. default_branch: every PR aims at the repo's default branch. Any other value fails closed rather than guessing a base. |
| `prompt_manager_block_enabled` | bool | n/a |
| `prompt_manager_review_enabled` | bool | n/a |
| `session_worktree_base` | string | What a new primary session's branch+worktree is cut from. Order: configured -> remote default -> main -> master. Values: remote_default (default), local_default, current (opt-in only, never a fallback), or an explicit ref. Env: AIMEE_SESSION_WORKTREE_BASE. |
| `tool_output_max_bytes` | int | Per-result cap (bytes) on the model-visible tool output (read_file/bash/grep/glob/git_* results). 0 = built-in default (32768); any positive value is clamped to (0, 32768]. Set it lower to bound the bytes a single tool result adds to the prompt + history; the context-economizer (aggressive tier) compresses older results to keep history bounded. |

### Dev-only keys (7)

Internal dogfood/QA knobs; not part of the user surface.

| Key | Type | Description |
|-----|------|-------------|
| `dogfood_autolabel_continuation` | bool | Auto-label continuation turns for dogfood capture. |
| `dogfood_autolabel_repair` | bool | Auto-label repair turns for dogfood capture. |
| `dogfood_autolabel_repeat_question` | bool | Auto-label repeated-question turns. |
| `dogfood_commit_raw` | bool | Commit raw (unredacted) dogfood transcripts. |
| `dogfood_enabled` | bool | Capture sessions as dogfood training/eval data. |
| `dogfood_inline_tagging` | bool | Inline-tag dogfood events during the session. |
| `dogfood_log_dir` | string | Directory for dogfood logs. |

## Config-file sections (54)

Set in the config JSON as `{"<section>": {"<key>": ...}}`. Keys are derived from the section parsers in `src/config*.c`; a key shown as a bare name that is itself a nested object is noted in the section description (see *Coverage & limitations*).

- **`aimee`**: _Core API/runtime settings._ Keys: `api`
- **`autonomy`**: `auto_resume_cap_parks`, `ci_retry_max`, `concurrency`, `fanout`, `max_resumes`, `max_turns`, `max_wall_secs`, `skeptics`, `stale_abandon_secs`, `unit_max`, `unit_retry`
- **`auxiliary`**: _Auxiliary (cheap/background) model used for side tasks._ Keys: `default_max_tokens`, `default_model`, `default_provider`, `enabled`, `tasks`
- **`cache_shaping`**: _Prompt-cache shaping._ Keys: `enabled`, `min_chars`
- **`charter`**: _Operating charter: values, constraints, safety axioms, tone._ Keys: `hard_constraints`, `safety_axioms`, `tone_boundaries`, `values`, `working_profile_drift_limit`
- **`compact`**: _Transcript compaction thresholds._ Keys: `coord_closet`, `enabled`, `from_record`, `head_bytes`, `per_tool`, `tail_bytes`, `threshold`
- **`computer_use`**: _Computer-use (browser) tool settings._ Keys: `allowed_domains`, `default_navigation`, `enabled`, `redact_sensitive_screenshots`
- **`concurrency`**: _Per-model / per-provider concurrency limits._ Keys: `default`, `maximum_total_concurrent_agent_sessions`, `per_model`, `per_provider`, `preempt`
- **`context`**: _Context-engine selection._ Keys: `engine`
- **`cost_reward`**: _Cost-aware reward shaping._ Keys: `enabled`, `lambda_pct`, `ref_usd_milli`
- **`cron_jobs`**: _Scheduled job definitions (array of objects)._ Keys: `context_from`, `deliver`, `enabled`, `id`, `mode`, `pre_wake_gate`, `prompt`, `schedule`, `script`, `skills`, `when_context_contains`, `workdir`
- **`cross_verify`**: _Cross-model output verification._ Keys: `enabled`, `prompt`, `role`, `verify_cmd`
- **`db2`**: _DB2 / vector store settings._ Keys: `vector`
- **`dedup`**: _Response deduplication._ Keys: `enabled`, `window_seconds`
- **`dogfood`**: _Session capture for dogfood data._ Keys: `commit_raw`, `enabled`, `inline_tagging`, `log_dir`
- **`ensemble`**: _Roundtable ensemble panel + aggregator._ Keys: `aggregator`, `max_cost_usd`, `min_successful`, `reference_models`, `reference_personas`
- **`extended_thinking`**: `enabled`
- **`fold`**: `enabled`, `excerpt_bytes`, `freeze`, `min_fold_msgs`, `recall`, `register_enabled`, `retained_msgs`
- **`guardrails`**: _Semantic guardrail policy._ Keys: `blast_radius`, `semantic`
- **`identity`**: _Working-profile identity injection._ Keys: `working_profile_injection`
- **`ingress`**: _Ingress (proxy frontends) behavior._ Keys: `audit_async`, `trusted_proxy_secret`, `usage_accounting_enabled`
- **`integrity`**: _Integrity gate._ Keys: `dry_run`, `enabled`
- **`intelligence`**: _Intelligence subsystems (bandit, planner, ranking, reasoning) + their external commands; most children are nested objects._ Keys: `bandit`, `bandit_optimize_command`, `calibrate`, `constraint_solver_command`, `demotion`, `kb`, `planner`, `planner_search_command`, `ranker_fuse_command`, `ranking`, `reasoning`, `reasoning_datalog_command`, `synthesize`
- **`kb`**: _Knowledge-base client + curator / evidence / maintenance / mining (nested objects)._ Keys: `api`, `background_ingest`, `code_hybrid`, `connection_pool_size`, `connection_workers`, `curator`, `evidence`, `maintenance`, `mining`, `purge_fence_ttl_s`, `reembed_on_dim_change`, `search_max_results`, `typed_facts`, `worker_count`
- **`learning`**: _Learning subsystem (router, implicit, embed, synthesize; nested objects)._ Keys: `embed`, `implicit`, `review`, `router`, `synthesize`
- **`lsp_servers`**: _LSP server definitions (array of objects)._ Keys: `args`, `command`, `extensions`, `name`
- **`mcp`**: _MCP integration (e.g. OSV)._ Keys: `osv`
- **`mcp_clients`**: _MCP client connections (array of objects)._ Keys: `bearer_token_env`, `command`, `cwd`, `install`, `name`, `transport`, `url`
- **`memory`**: _Memory subsystem; most children (recall, lifecycle, …) are nested objects with their own keys._ Keys: `abstain`, `aggregation`, `bm25_weight`, `briefing`, `citations`, `cognify`, `context_budget`, `coref`, `derive_facts`, `directives`, `dispositions`, `episode_summaries`, `failure_detection`, `fetch_budget`, `hard_negative_log`, `improve`, `lifecycle`, `pagerank`, `profile_cards`, `prospective`, `recall`, `rewrite`, `routing`, `salience`, `scenes`, `semantic_floor_scale`, `semantic_weight`, `typed_facts`
- **`memory_maintenance`**: _Memory maintenance scheduling._ Keys: `enabled`, `interval_seconds`, `summarize_enabled`, `trigger_inserts`, `trigger_secs`
- **`memory_negation`**: _Negation handling in memory._ Keys: `enabled`
- **`memory_query_expansion`**: _Recall query expansion._ Keys: `k`, `mode`
- **`memory_recall_lanes`**: _Per-lane recall floors / caps._ Keys: `enabled`, `fact_kinds`, `floor_fact`, `floor_summary`, `k_fact`, `k_summary`, `summary_kinds`
- **`memory_rewrite`**: _Recall query rewriting._ Keys: `command`, `decompose`, `enabled`, `hyde`, `max_subqueries`
- **`memory_window`**: _Memory-window neighbour expansion._ Keys: `radius`
- **`model_meta`**: _Model metadata + capability routing._ Keys: `capability_routing`, `refresh_minutes`
- **`otel`**: _OpenTelemetry export._ Keys: `endpoint`, `service_name`
- **`reasoning_cap`**: _Reasoning-effort cap._ Keys: `enabled`
- **`retry`**: _Provider retry / backoff._ Keys: `base_ms`, `max_attempts`, `max_ms`
- **`rewind`**: _Auto-snapshot / rewind._ Keys: `auto_snapshot`
- **`roundtable`**: _Roundtable pipeline thresholds, caps, gates, and turns._ Keys: `converge_threshold`, `deadline_ms`, `default`, `max_rounds`, `pipeline_done_bar`, `pipeline_gate_ttl_h`, `pipeline_max_attempts_per_pass`, `pipeline_max_cost_usd`, `pipeline_max_passes`, `pipeline_max_total_cost_usd`, `pipeline_parked_releases_slot`, `pipeline_unknown_context_tokens`, `turns`
- **`routing`**: _Capability-gated delegate routing: whether to gate seat choice on capability before cost, and whether to prefer free local seats._ Keys: `enabled`, `prefer_local`
- **`sandbox`**: _Tool sandbox (paths, network, mode)._ Keys: `allow_paths`, `mode`, `network`
- **`script`**: _Script-tool allowlist._ Keys: `allowed_tools`
- **`search`**: _Web-search backend (Tavily / SearXNG)._ Keys: `backend`, `backends`, `fetch_pages`, `max_results`, `searxng_url`, `tavily_api_key`
- **`session`**: _Session / worktree limits._ Keys: `max_sessions`, `max_worktrees`, `stale_threshold_secs`, `virtual_context`
- **`skills`**: _Skill subsystem (capability, dispatch, eval, review; nested objects)._ Keys: `capability`, `dispatch`, `eval`, `review`
- **`telemetry`**: `metrics_token`
- **`transport`**: _Transport tweaks (cache-aware rewrite)._ Keys: `cache_aware_rewrite`, `kb_gzip_enabled`, `kb_pool_enabled`, `server_keepalive_enabled`, `thinclient_gzip_enabled`
- **`trigger`**: _Trigger listener (auth, concurrency)._ Keys: `auth_token`, `max_concurrent`
- **`trigger_rules`**: _Trigger rule definitions (array of objects)._ Keys: `event`, `mode`, `pipeline`, `schedule`, `source`
- **`vault`**: `custody`, `tpm2`
- **`workspaces`**: _Workspace definitions (array of objects)._ Keys: `head`, `path`, `provider`, `remote`, `sandbox_image`
- **`worktree_gc`**: `enabled`, `max_age_days`

## Other top-level config-file keys (5)

Scalar keys read directly from the config root (not via the CLI allowlist above):

`db2_pool_size`, `economizer`, `modules`, `proxy_token`, `toolsets`

## Environment variables

The binaries read 246 `AIMEE_*` environment variables (scanned from `getenv()` in `src/` and `os.Getenv()` in `server-go/`, excluding tests, plus the generic first-boot credential inputs). Depending on the setting, these variables either override config-store values or provide fallbacks when no explicit config value is present. Module-activation variables use fallback semantics; deployment and runtime wiring variables commonly override stored values. A credential may enter through an environment variable only as first-boot transport (for example, a Kubernetes Secret): startup seals it into Vault, scrubs the environment, verifies custody, and fails closed before any long-lived service starts. Credentials are never runtime environment or config-file storage.

### Paths & assets

| Variable | Description |
|----------|-------------|
| `AIMEE_BUNDLED_SKILLS_DIR` | Override directory for the bundled skills. |
| `AIMEE_FORENSICS_DIR` | Directory for shutdown-forensics dumps. |
| `AIMEE_GUARDRAILS_PATH` | Path to the guardrails policy file. |
| `AIMEE_HARNESS_MEMORY_SCOPES` | Path to the agent memory-surface registry config (default `<AIMEE_HOME>/harness_memory_scopes.conf`). Each `client:projects_root:memory_seg` line adds a new agent or overrides a built-in's paths for memory-write interception (writes are redirected into aimee's db1). |
| `AIMEE_HOME` | Root of the per-user state/config store (config, `workflows/`, keys). Overrides the platform default. DB1 is no longer under it: the store is a PostgreSQL database reached through AIMEE_STORE_URL. |
| `AIMEE_MODELS_DEV_SNAPSHOT` | Path to an offline models.dev catalog snapshot. |
| `AIMEE_OAUTH_RUNTIME_DIR` | Private directory for transient OAuth callback/session state; it must not be used for durable credentials. |
| `AIMEE_PACK_DIR` | Directory of memory profile packs. |
| `AIMEE_RUNTIME_DIR` | Private runtime directory for sockets, temporary credentials, and process state. |
| `AIMEE_STORE_URL` | PostgreSQL DSN the DB1 store module connects to. The module is a separate process and cannot read the config store, so it is told the DSN and refuses to start without it rather than guessing a default and serving a different, empty database. Replaces AIMEE_DB1_PATH, which named a SQLite file back when the store was a C module; nothing reads that variable now. |
| `AIMEE_TOOLSETS_CONFIG` | Path to a toolsets config file (overrides the default tool allowlists). |
| `AIMEE_WORKSPACES_DIR` | Root directory for mirrored/registered workspaces. |

### Client & session

| Variable | Description |
|----------|-------------|
| `AIMEE_ACTIVE_TOOLSET` | Active toolset (tool allowlist) for the session. |
| `AIMEE_API_BEARER` | Bearer token for the `/v1` API endpoint. |
| `AIMEE_API_ENDPOINT` | Override the `/v1` API endpoint used by the client RPC layer. |
| `AIMEE_ATTACH_ID` | Presence attach id used when joining an existing session. |
| `AIMEE_CLIENT_TYPE` | Calling client type used for integration-specific request shaping. |
| `AIMEE_CLI_PATH` | Absolute path to the aimee CLI, quoted into the guidance an agent receives so it does not assume `aimee` is on PATH. |
| `AIMEE_HOOK_CLIENT` | Identifies the calling hook client (e.g. claude/codex) for hook routing. |
| `AIMEE_HOOK_TRANSPORT` | Which repository-intelligence surface the session-start guidance points an agent at: `mcp` names the Aimee MCP index capability, anything else names the CLI at `AIMEE_CLI_PATH`. |
| `AIMEE_MODE` | Operating-mode override (e.g. interactive / autonomous). |
| `AIMEE_NO_CLIENT_INTEGRATIONS` | If set (to any value other than 0/false), aimee does not auto-register itself into detected AI-tool user configs (Claude Code, Gemini, Copilot, Codex). Overrides the client_integrations_enabled config; honored by the aimee binary and by install.sh/configure-hooks.sh. |
| `AIMEE_PRIMARY_CLI_INGESTOR` | Name of the primary CLI integration that owns session-ingest events. |
| `AIMEE_PROFILE` | Active working-profile name. |
| `AIMEE_PROJECT_ID` | Explicit project identity for an integration request. |
| `AIMEE_SERVER_TOKEN` | Bearer token presented to aimee-server over TCP. |
| `AIMEE_SERVER_URL` | aimee-server endpoint the thin client connects to (UDS path or `tcp:host:port`). |
| `AIMEE_SESSION_ID` | Pre-set the session id (enables non-blocking session attach). |
| `AIMEE_SESSION_START_VERBOSE` | Verbose logging during session start. |
| `AIMEE_SESSION_WORKTREE_BASE` | Which checkout a session's worktree branches from. `remote_default` (the default) uses the server's default branch; `current` opts in to inheriting the source checkout's branch, for offline or detached workflows. Only ever an explicit opt-in -- `current` is never reached as a fallback. |
| `AIMEE_TRANSPORT_SERVER_KEEPALIVE_ENABLED` | Resident HTTPS connection reuse. Defaults on; set to 0 to restore one request per connection. |
| `AIMEE_TRANSPORT_THINCLIENT_GZIP_ENABLED` | Negotiated gzip for eligible buffered thin-client routes. Defaults off; set to 1 only for a measured remote link profile. |
| `AIMEE_TUI_SESSION` | Identifies the TUI session. |

### Server runtime

| Variable | Description |
|----------|-------------|
| `AIMEE_API_MTLS` | Client-certificate mode: `off`, `optional`, or `required`. The managed server image defaults to `optional` so enrollment works before the durable roster promotes the listener to required. |
| `AIMEE_API_REMOTE_WRITES` | Legacy value: `off`, `data`, or `full`. Still parsed, but no longer authorizes user writes; non-off values warn and feed `remote_writes.global_ignored`. |
| `AIMEE_BACKGROUND_THREADS` | Background worker thread count. |
| `AIMEE_COMPUTE_THREADS` | Compute-pool thread count. |
| `AIMEE_DEPLOY_COMPOSE_FILE` | Path to the managed compose file the server-orchestrated deploy runs (default /opt/aimee/deploy/aimee-managed.compose.yaml). |
| `AIMEE_DEPLOY_ENABLED` | Set to 1 to enable the server-orchestrated deploy: the setup wizard runs `docker compose up -d` for the managed sibling service (aimee-kb) via a mounted Docker socket. Off unless the deploy compose sets it. |
| `AIMEE_GITHUB_OAUTH_CLIENT_ID` | Client ID of a GitHub OAuth App for the webchat "Sign in with GitHub" button; populates the github.com git credential. Public. Overrides the built-in default baked in via oauth_defaults.h. |
| `AIMEE_GITLAB_OAUTH_CLIENT_ID` | Client ID of a GitLab OAuth application (device flow enabled) for the webchat "Sign in with GitLab" button on gitlab.com. Public. Overrides the built-in default baked in via oauth_defaults.h. |
| `AIMEE_MGMT_STATUS_KEY_ID` | Identifier of the management-status verification key. |
| `AIMEE_MGMT_STATUS_PUBLIC_KEY` | Hex-encoded Ed25519 key used to verify management-status staples. |
| `AIMEE_MODULE_BUS_SOCKET` | Unix socket the module bus listens on; the default for `--module-bus-socket`. |
| `AIMEE_MODULE_EVENT_BASE` | Retired. Event kinds are derived from the principal ref now. An instance still carrying this variable starts only if the value agrees with the derivation; when it disagrees the module refuses, because a deployment provisioned under the old scheme has a `.grant` naming kinds that belong to other modules. Re-run `scripts/provision-plugin-module.py` for the instance instead of unsetting it. |
| `AIMEE_MODULE_POLICY_DIR` | Directory of module `.grant` manifests, overriding the built-in location. The bus reads a peer's grant from here to decide what that module may serve. |
| `AIMEE_MODULE_PRINCIPAL_REF` | Principal reference an instanced module serves under, written into its `.grant` at provisioning time. A module cannot allocate its own authorization identity, so it refuses to start without this. Event kinds are derived from it: `kind = 4096 + ref*256 + stage`. |
| `AIMEE_MODULE_ROUNDTABLE` | Enable the optional roundtable module; invalid values fail closed to off. |
| `AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT` | Permit the operator-configured search backend (`search.searxng_url`) to resolve to a private, loopback, or link-local address. Off by default: every outbound fetch is validated and pinned, so a self-hosted SearXNG on a LAN address is refused unless this is set. Deliberately an environment variable rather than a config key, because `config.set` is reachable from inside the running system and pointing the search backend at a cloud metadata address would exfiltrate instance credentials through a tool that looks like search. Set to exactly `1`; any other value is off. Never widens fetches of model-supplied or search-result URLs, which stay denied. |
| `AIMEE_SERVER_HTTP_BIND` | TCP bind address for the server `/v1` HTTP listener (else UDS-only). |
| `AIMEE_SERVER_MGMT_BIND` | Bind address that enables the server management listener when its full TLS configuration is present. |
| `AIMEE_SERVER_MGMT_ISSUER` | Expected issuer for management-plane bearer identities. |
| `AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE` | Root-owned trust bundle used to verify signed management JWKS publications. |
| `AIMEE_SERVER_MGMT_STATUS_CLIENT_KEY` | Forbidden legacy private-key file setting. Startup rejects it; use AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY as first-boot Vault input. |
| `AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY` | PEM private-key content for the management-status client, accepted only as first-boot transport and synchronously sealed into Vault before the server starts. |
| `AIMEE_SERVER_MGMT_STATUS_SECONDARY_LEAF_PIN` | Secondary management-status TLS leaf pin accepted during certificate rotation. |
| `AIMEE_SERVER_MGMT_TLS_KEY` | Forbidden legacy private-key file setting. Startup rejects it; use AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY as first-boot Vault input. |
| `AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY` | PEM private-key content for the management listener, accepted only as first-boot transport and synchronously sealed into Vault before the server starts. |
| `AIMEE_SERVER_STARTUP_FD` | Inherited fd for startup-readiness signalling (service launch). |
| `AIMEE_SERVER_TLS_PRIVATE_KEY` | Optional PEM private-key content for first boot. The key is synchronously sealed into Vault and the environment is scrubbed; only the public server certificate may remain on disk. |
| `AIMEE_SESSION_THREADS` | Per-session worker thread count. |
| `AIMEE_SOCK` | Sandbox helper socket path. |
| `AIMEE_STAGE_GOVERNANCE` | Enable the governance stage in the canonical request pipeline. |
| `AIMEE_STAGE_MEMORY` | Enable the memory stage in the canonical request pipeline. |
| `AIMEE_VAULT_ENV_OVERWRITE` | First-boot control flag allowing supplied credential values to replace existing Vault records. It is not itself a credential. |
| `AIMEE_VAULT_KMS_HELPER` | Executable implementing the configured external KMS wrap/unwrap contract. |
| `AIMEE_VAULT_KMS_HWM_DOMAIN` | Domain separator for KMS high-water-mark signatures. |
| `AIMEE_VAULT_KMS_HWM_PUBKEY` | Public key used to verify KMS high-water-mark records. |
| `AIMEE_VAULT_KMS_KEY_ID` | External KMS key identifier used for vault wrapping. |
| `AIMEE_VAULT_PKCS11_LABEL` | PKCS#11 object label used for vault custody. |
| `AIMEE_VAULT_PKCS11_MODULE` | Path to the PKCS#11 provider module. |
| `AIMEE_VAULT_PKCS11_PIN` | PKCS#11 user PIN accepted only as first-boot transport; it is synchronously sealed into Vault, scrubbed from the environment, and loaded from Vault only when the HSM session opens. |
| `AIMEE_VAULT_PKCS11_SLOT` | PKCS#11 slot identifier used for vault custody. |
| `AIMEE_VAULT_TPM2_BLOB_PATH` | Path to the sealed TPM 2 vault-key blob. |
| `AIMEE_VAULT_TPM2_NV_INDEX` | TPM 2 NV index used for anti-rollback state. |
| `AIMEE_VAULT_TPM2_TCTI` | TPM 2 TCTI selector. |
| `AIMEE_WEBCHAT_EDITOR` | Per-webuser in-browser code-server editor (on by default; set to 0 to disable; needs a code-server binary, shipped by WITH_VSCODE images). |
| `AIMEE_WEBCHAT_EDITOR_BIN` | Override path to the code-server binary used for the in-browser editor. |
| `AIMEE_WEBCHAT_EDITOR_IDLE_SECS` | Idle timeout in seconds before a per-webuser code-server editor is reaped. Default 1800 (30 min); positive values are clamped to [60, 604800]; 0 disables idle reaping; malformed/negative/overflow values fall back to the default. An actively-open editor is kept alive by the proxy keepalive, so it is not reaped mid-session. |
| `AIMEE_WEBCHAT_EDITOR_UID` | Dedicated service user the per-webuser code-server drops to (defence in depth; only honoured when aimee-server runs as root). |
| `AIMEE_WEBCHAT_GIT` | Per-webuser webchat git surface: repo connect/clone, git ops (pull/commit/push/branch), per-host token + SSH-key credential intake, the workspace forge-token broker, project listing + session-dir resolution, and "Sign in with GitHub". It is on by default. Set the literal value 0 to disable the entire surface; all of those routes then return 503. Any other value leaves it on. Independent of AIMEE_WEBCHAT_EDITOR. |
| `AIMEE_WEBCHAT_PASSWORD` | Optional first-boot webchat password paired with AIMEE_WEBCHAT_USER. The bootstrap record is sealed into Vault and removed from the environment before runtime-web starts. |
| `AIMEE_WEBCHAT_USER` | Optional first-boot webchat username paired with AIMEE_WEBCHAT_PASSWORD. The bootstrap record is sealed into Vault and removed from the environment before runtime-web starts. |
| `AIMEE_WEBCHAT_USERS` | Optional first-boot webchat account registry. It is sealed into Vault and removed from the environment before runtime-web starts. |
| `AIMEE_WORKTREE_GC` | Enable/disable delegate-worktree garbage collection. |
| `AIMEE_WORKTREE_GC_DAYS` | Age threshold (days) for worktree GC. |

### Knowledge base (aimee-kb)

| Variable | Description |
|----------|-------------|
| `AIMEE_CODE_INDEX_SOURCE` | Source label recorded for code-index ingestion. |
| `AIMEE_EMBEDDERS_FILE` | Path to the embedder registry the server reads for GET /v1/embedders (the setup wizard's embedder picker). Defaults to /opt/aimee/embedders.json, then scripts/embedders.json in a source checkout. The same file the in-container embedder reads, so one declaration drives the picker, the loading and the serving flags. |
| `AIMEE_EMBEDDER_HOST` | DNS name of the embedder sidecar container (aimee-embedder-a25m or aimee-embedder-nomic). Setting it makes aimee-kb issue the mTLS identities for the kb -> embedder hop into $AIMEE_HOME/embedder-tls at startup, independently of the synthesis hop. Unset for an external embedder reached over plain HTTPS, or when no embedder is deployed. The sidecar refuses to start without this material. |
| `AIMEE_EMBED_HTTP_TIMEOUT_MS` | Deadline for one embedding HTTP call, default 180000. The previous hardcoded 30s was shorter than a cold model load plus a large batch, so the first request of a run could fail on a healthy embedder. |
| `AIMEE_KB_API_CA_BUNDLE` | CA bundle path for verifying the aimee-kb TLS certificate. |
| `AIMEE_KB_API_URL` | aimee-kb HTTP API base URL. |
| `AIMEE_KB_CACHE_TTL_S` | KB client cache TTL (seconds). |
| `AIMEE_KB_CONN` | First-boot KB connection string; sealed into the server Vault before long-lived startup. |
| `AIMEE_KB_EMBED_ALL_FILES` | Set to 1 to give EVERY indexed file a dense document vector, including source. Off by default because source files are already embedded by the code path, and embedding them a second time as prose was 82% of the doc-embedding token budget on a real corpus. Chunk rows are written either way, so lexical and FTS search over source is unaffected by this setting; only the redundant vector is skipped. |
| `AIMEE_KB_EMIT_ENROLL` | Emit a client enrollment token on KB start. |
| `AIMEE_KB_EMIT_SCOPE` | Scope for the emitted enrollment token. |
| `AIMEE_KB_HARDENED` | Require the hardened KB custody and transport posture at startup. |
| `AIMEE_KB_HTTP_BIND` | aimee-kb HTTP listener bind address. |
| `AIMEE_KB_JWKS_MANIFEST_ROOT_CUSTODY_ID` | Custody identifier for the key that signs JWKS manifests. |
| `AIMEE_KB_JWKS_PUBLICATION_HWM_CUSTODY_ID` | Custody identifier for the JWKS publication high-water mark. |
| `AIMEE_KB_JWKS_PUBLISH_DSN` | Provisioning database URL used by the JWKS publisher; secret. |
| `AIMEE_KB_MGMT_STATUS_SECONDARY_LEAF_PIN` | Secondary TLS leaf pin accepted during management-status certificate rotation. |
| `AIMEE_KB_MTLS_HOST` | Advertised mTLS hostname placed in the aimee-kb server certificate; the listener binds all interfaces. |
| `AIMEE_KB_MTLS_PORT` | aimee-kb mTLS listener port. |
| `AIMEE_KB_OIDC_AUDIENCE` | OIDC audience for KB API auth. |
| `AIMEE_KB_OIDC_ISSUER` | OIDC issuer for KB API auth. |
| `AIMEE_KB_OIDC_JWKS_FILE` | OIDC JWKS file for KB API auth. |
| `AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL` | IdP authorization endpoint the login redirects to (https only). |
| `AIMEE_KB_OIDC_LOGIN_CLIENT_ID` | OIDC relying-party client id; setting it enables the per-user login front end. |
| `AIMEE_KB_OIDC_LOGIN_REDIRECT_URI` | This kb's OIDC callback URL (https, or http on loopback only). |
| `AIMEE_KB_OIDC_LOGIN_SCOPE` | Space-delimited OIDC scopes for the login request; defaults to openid. |
| `AIMEE_KB_OIDC_LOGIN_TOKEN_URL` | IdP token endpoint for the code exchange (https, default port only). |
| `AIMEE_KB_OIDC_MAX_TOKEN_AGE` | Maximum accepted age in seconds for a KB OIDC token. |
| `AIMEE_KB_OIDC_SCOPE_CLAIM` | OIDC claim carrying the scope. |
| `AIMEE_KB_OIDC_SCOPE_KIND` | OIDC scope-kind interpretation. |
| `AIMEE_KB_READ_TIMEOUT_MS` | Deadline for a single KB read issued by the client, in milliseconds. |
| `AIMEE_KB_RUNTIME_UID` | Numeric runtime user allowed to receive the management token-authority socket. |
| `AIMEE_KB_SCAN_TIMEOUT_MS` | Deadline for a code-index scan request, in milliseconds. Scans are queued and drained by the ingest workers, so this bounds the REQUEST rather than the work. |
| `AIMEE_KB_STATUS_BIND` | Bind address for the management-status authority. |
| `AIMEE_KB_STATUS_DSN` | Runtime database URL used by the management-status authority; secret. |
| `AIMEE_KB_STATUS_PORT` | Listen port for the management-status authority. |
| `AIMEE_KB_STATUS_PROVISION_DSN` | Provisioning database URL used to initialize management-status state; secret. |
| `AIMEE_KB_TOKEN_AUTHORITY_DSN` | Database URL used by the management token authority; secret. |
| `AIMEE_KB_TOKEN_AUTHORITY_SOCKET_GID` | Group allowed to connect to the management token-authority socket. |
| `AIMEE_KB_TOKEN_ROOTS_PROVISION_DSN` | Provisioning database URL used to initialize token roots; secret. |
| `AIMEE_KB_TOKEN_ROOT_CUSTODY_ID` | Custody identifier for the management token signing root. |
| `AIMEE_KB_VAULT_OPERATOR_ENABLED` | Enable the dedicated KB vault-operator runtime. |
| `AIMEE_KB_VAULT_ORCHESTRATOR_URL` | Operator-configured vault orchestrator endpoint. |
| `AIMEE_LLM_HOST` | DNS name of the synthesis sidecar container. Setting it makes aimee-kb issue the mTLS identities for the kb -> aimee-llm hop into $AIMEE_HOME/synthesis-tls at startup, from the kb's own CA. Unset for an external or absent synthesis provider, which needs none of them. The sidecar refuses to start without this material. |
| `AIMEE_OCR_URL` | Structured-PDF OCR sidecar endpoint. |
| `AIMEE_SERVER_ID` | Registry identity used by the server mTLS heartbeat. |
| `AIMEE_SERVER_TEAM_ID` | The team this server serves, from the same registry row as AIMEE_SERVER_ID. Required for per-user /v1 write authorization: unset, the server still starts and serves reads but denies every write with no_team_configured. |
| `AIMEE_TRANSPORT_KB_POOL_ENABLED` | Override server-to-KB mTLS connection pooling. The config default is on; set to 0 for one-shot connections. |
| `AIMEE_TSR_URL` | Structured-PDF table-recognition sidecar endpoint. |
| `AIMEE_VECTOR_KB_BATCH_SIZE` | Embedding batch size for KB vector ingest. |

### Database & vectors

| Variable | Description |
|----------|-------------|
| `AIMEE_DB2_EVAL_URL` | Separate DB2 URL used by evaluation harnesses; never the production default. |
| `AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS` | Per-connection `idle_in_transaction_session_timeout` in ms, defaulting to the same pool stuck-lease ceiling (`DB2_POOL_HOLD_CEILING_MS`, 300000). `statement_timeout` bounds a STATEMENT, so a unit of work that opens a transaction and then stalls before its next statement is invisible to it and holds its pool member indefinitely. This measured at about 4.5 hours against a five-minute ceiling. Postgres ends such a backend itself, so the stalled thread unwinds and the lease is returned without a restart. Same value grammar as `AIMEE_DB2_STATEMENT_TIMEOUT_MS`; exactly `0` opts out, independently of the statement bound. |
| `AIMEE_DB2_POOL_SIZE` | DB2 connection-pool size override. |
| `AIMEE_DB2_STATEMENT_TIMEOUT_MS` | Per-connection `statement_timeout` in ms. Defaults to the pool's stuck-lease ceiling (`DB2_POOL_HOLD_CEILING_MS`, 300000), because a statement must not outlive the duration that defines a lease as stuck. The pool can report such a lease but cannot reclaim it. The value must be canonical decimal digits with no sign, surrounding whitespace or leading zero. Exactly `0` disables the bound. This is a deliberate opt-out for genuinely long work. Every other spelling of zero (`00`, `+0`, `-0`, ` 0`) is treated as malformed. Anything malformed or out-of-range falls back to the default and never to unlimited, so no typo can silently remove the bound. |
| `AIMEE_DB2_URL` | Postgres (DB2) connection URL for the KB store. |
| `AIMEE_DIM_PROBE_BUDGET_MS` | Time budget for probing an embedder's output dimension. |
| `AIMEE_PGVEC_SLOW_QUERY_MS` | Slow-query log threshold (ms) for the pgvector transport. |
| `AIMEE_TEST_DB2_TEMPLATE_URL` | Test-only. Postgres template database the DB2 test shim clones per test process, so unit tests run against the real engine instead of the sqlite shim (which translates DB2's SQL rather than executing it). Build the template with `make db2-test-template` and the suite with `make unit-tests-pg`; unset, tests use the sqlite shim as before. Read only by test binaries; no production code path consults it. |

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
| `AIMEE_MEMORY_RERANK_MODE` | Reranker mode. |
| `AIMEE_MEMORY_WEIGHT_PROFILE` | Recall scoring weight profile. |
| `AIMEE_NO_CACHE` | Disable the memory-assembly cache. |

### Delegates & backends

| Variable | Description |
|----------|-------------|
| `AIMEE_ALLOW_MAIN_CHECKOUT` | Allow an explicitly authorized delegate path to use the main checkout instead of a managed worktree. |
| `AIMEE_CODEX_REFRESH_SKEW` | Seconds before Codex OAuth expiry at which the server refreshes the token. |
| `AIMEE_DELEGATE_DEPTH` | Current delegation depth (recursion guard). |
| `AIMEE_DELEGATE_HEARTBEAT_MONITOR` | Enable the delegate heartbeat monitor. |
| `AIMEE_DELEGATE_MAX_INFLIGHT` | Process-wide maximum number of admitted delegate attempts. |
| `AIMEE_DELEGATE_SANDBOX` | Enable the configured delegate sandbox backend. |
| `AIMEE_DELEGATE_SOURCE_AUTHORITY` | Enable source-authority gating for delegate edits. |
| `AIMEE_DELEGATE_SOURCE_PATHS` | Allowed source paths for delegate edits. |
| `AIMEE_DELEGATE_WORKTREE_ROOT` | Root directory for delegate worktrees. |
| `AIMEE_DOCKER_BIN` | Docker delegate-backend binary. |
| `AIMEE_DOCKER_WORKDIR` | Docker delegate-backend working directory. |
| `AIMEE_FORWARDER_PORT` | Loopback port the in-sandbox aimee-forwarder listens on (default 3129); set by aimee when it starts the forwarder in a proxy-mode delegate container. |
| `AIMEE_FORWARDER_SOCK` | UNIX socket the in-sandbox aimee-forwarder bridges to (default /run/aimee/aimee-http.sock, the bound aimee UDS). |
| `AIMEE_PARENT_DELEGATION_ID` | Parent delegation id (threading). |
| `AIMEE_SANDBOX_HOST_MOUNTS` | Operator allowlist of host mounts available to the sandbox backend. |
| `AIMEE_SSH_BIN` | SSH delegate-backend binary. |

### Forge (GitHub App / tokens)

| Variable | Description |
|----------|-------------|
| `AIMEE_FORGE_API_BASE` | Forge API base URL. |
| `AIMEE_FORGE_APP_ID` | GitHub App id for minting forge tokens. |
| `AIMEE_FORGE_APP_INSTALLATION_ID` | GitHub App installation id. |
| `AIMEE_FORGE_APP_PRIVATE_KEY` | GitHub App private-key PEM accepted only as first-boot transport; it is sealed into Vault and filesystem paths are rejected. |
| `AIMEE_FORGE_SCOPE` | Scope for the minted forge token. |
| `AIMEE_FORGE_SERVICE_SOCKET` | Unix socket for the forge service, used instead of a URL when it is local. |
| `AIMEE_FORGE_SERVICE_URL` | Forge service endpoint; the default for `--forge-service-url`. |
| `AIMEE_FORGE_TOKEN` | First-boot static forge token. aimee-server seals it into the server Vault and unsets it before serving; subsequent boots read only from Vault. |

### Gateway (voice / webhooks / push)

| Variable | Description |
|----------|-------------|
| `AIMEE_GATEWAY_NTFY_BASE_URL` | ntfy push base URL. |
| `AIMEE_GATEWAY_STT_MODEL` | Speech-to-text model. |
| `AIMEE_GATEWAY_STT_PROVIDER` | Speech-to-text provider. |
| `AIMEE_GATEWAY_TTS_BASE_URL` | Text-to-speech base URL. |
| `AIMEE_GATEWAY_TTS_MODEL` | Text-to-speech model. |
| `AIMEE_GATEWAY_TTS_PROVIDER` | Text-to-speech provider. |
| `AIMEE_GATEWAY_TTS_VOICE` | Text-to-speech voice. |
| `AIMEE_GATEWAY_WEBHOOK_DELIVER_ONLY` | Webhook deliver-only mode (no reply path). |
| `AIMEE_GATEWAY_WEBHOOK_INSECURE` | Allow the webhook listener without TLS (dev). |
| `AIMEE_GATEWAY_WEBHOOK_PORT` | Inbound webhook listener port. |

### Workflow engine

| Variable | Description |
|----------|-------------|
| `AIMEE_AUTONOMY_BASE` | Legacy C workflow integration-branch fallback. The Go WFE uses the branch checked out when it admits the repository. |
| `AIMEE_AUTONOMY_MAX_ACTIVE_PER_PRINCIPAL` | Maximum active autonomous work items for one authenticated principal. |
| `AIMEE_AUTONOMY_MAX_USD` | Default USD ceiling for an autonomous work item; 0 disables this default ceiling. |
| `AIMEE_AUTONOMY_PANEL_RETRIES` | Legacy C scheduler budget for retrying transient roundtable parks. It does not configure the Go WFE scheduler. |
| `AIMEE_AUTONOMY_SUBMIT_RATE_PER_MIN` | Autonomous-submission rate limit per principal. |
| `AIMEE_AUTONOMY_SUBMIT_WINDOW_SECS` | Window used by the autonomous-submission rate limiter. |
| `AIMEE_AUTONOMY_USD_PER_SEC` | Fallback spend estimator for autonomous admission when exact provider cost is unavailable. |
| `AIMEE_DEFAULT_BRANCH` | Legacy C workflow override for default-branch resolution. The Go WFE derives branch authority from the admitted repository and checkout. |
| `AIMEE_ORCH_DELEGATES` | Enable delegate resource use by the orchestration plane. |
| `AIMEE_ORCH_WORKFLOWS` | Enable workflow orchestration surfaces. |
| `AIMEE_PANEL_SEAT_WAIT_SECS` | Maximum wait for a roundtable seat to acquire an eligible agent. |
| `AIMEE_PR_BASE_MODE` | What a pr.open with no explicit base targets: the run's feature branch (default) or, when set to default_branch, the autonomous base. The server exports the configured pr_base_mode into this variable at startup, so `aimee config set pr_base_mode` stays the one operator knob; the workflow engine reads it from the environment because that module is deliberately config-free. |
| `AIMEE_WFE_ENGINE` | Workflow runtime selector; current server images require `go`. |
| `AIMEE_WFE_RUNNER_SOCKET` | Unix socket for the workflow runner, used instead of a URL when the runner is local. |
| `AIMEE_WFE_RUNNER_URL` | Workflow runner endpoint; the default for `--runner-url`. |
| `AIMEE_WFE_WORKTREE_GC_GRACE_SECS` | Grace period before an unowned workflow worktree can be collected. |
| `AIMEE_WORKFLOW_AUTONOMOUS_ROUTER` | Enable automatic scheduling of admitted autonomous work items. |
| `AIMEE_WORKFLOW_BASE` | Legacy C workflow fallback for the freeze/diff base. It does not set the Go WFE integration branch. |
| `AIMEE_WORKFLOW_BRANCH` | Explicit workflow feature branch for a compatibility or test runner. |
| `AIMEE_WORKFLOW_ENFORCE_STAGE` | Require runner requests to match the persisted workflow stage. |
| `AIMEE_WORKFLOW_LEASE_TTL_SECS` | Lifetime of a workflow execution lease before recovery may reclaim it. |
| `AIMEE_WORKFLOW_REPO` | Legacy C workflow fallback for a local repository. The Go WFE uses the repository admitted with each work item. |

### Git verify / MCP

| Variable | Description |
|----------|-------------|
| `AIMEE_GIT_AUTHOR_EMAIL` | Author email on commits the native workflow runner makes. Unset falls back to the repository's own git configuration. |
| `AIMEE_GIT_AUTHOR_NAME` | Author name on commits the native workflow runner makes. Unset falls back to the repository's own git configuration. |
| `AIMEE_MCP_CWD` | Working-directory hint for MCP git-root resolution. |
| `AIMEE_MCP_TOOLS_WATCH_SECONDS` | Seconds between registry-generation polls in `aimee mcp serve`; a change emits notifications/tools/list_changed so a client sees a plugin that appeared mid-session (default: 15). |
| `AIMEE_MCP_TOOL_PROFILE` | MCP tools/list presentation profile: 'core'/'lean' (default: Tier-0 high-frequency tools only, with find_tools/describe_tool reaching the rest) or 'full' (present every tool upfront). |
| `AIMEE_VERIFY_LOCK_FILE` | Override the host-wide file lock that serializes complete repository verification runs. |
| `AIMEE_VERIFY_PARALLEL` | Run `aimee git verify` steps in parallel. |
| `AIMEE_VERIFY_STEP_TIMEOUT_MS` | Per-step timeout (ms) for git verify. |

### Models

| Variable | Description |
|----------|-------------|
| `AIMEE_MODEL_CAPABILITY_OVERRIDES` | Override model capability flags (reasoning/tools/vision/…). |

### TLS & networking

| Variable | Description |
|----------|-------------|
| `AIMEE_KB_MTLS_MAX_CONNECTIONS` | Maximum concurrent connections accepted by the KB mTLS listener. |
| `AIMEE_KB_STATUS_TLS_CA` | CA file used by the management-status authority. |
| `AIMEE_KB_STATUS_TLS_CERT` | TLS certificate for the management-status authority. |
| `AIMEE_KB_STATUS_TLS_KEY` | TLS private key for the management-status authority; secret. |
| `AIMEE_NET_DEBUG` | Verbose network debug logging. |
| `AIMEE_TLS_CLIENT_P12_PASS` | Password for an explicitly provisioned client PKCS#12 bundle; secret. |
| `AIMEE_TLS_CN` | Common Name used when generating a local TLS certificate. |
| `AIMEE_TLS_EXTRA_SAN` | Additional subject-alternative names for a generated TLS certificate. |
| `AIMEE_TLS_INSECURE` | Disable TLS certificate verification (development only). |

### Plugins

| Variable | Description |
|----------|-------------|
| `AIMEE_MCP_PLUGIN_ARGV` | Command line for an MCP plugin instance, as a JSON array of strings. JSON rather than a shell string so an argument containing a space stays one argument. With no argv the module runs inert: it serves its stages, declares no commands and answers CapabilityAbsent. |
| `AIMEE_MCP_PLUGIN_CWD` | Working directory the MCP plugin instance is started in. |
| `AIMEE_MCP_PLUGIN_PERMISSION` | Permission ceiling for an MCP plugin instance -- the most it may ever do, whatever it requests. Unset means `read`, the least privilege. |
| `AIMEE_MCP_TOOL_ALLOWLIST` | Comma-separated tool names `aimee mcp serve` will expose. Unset serves the full tool set; set, it serves only these. |
| `AIMEE_MCP_TOOL_PROSE` | Selects the wording of MCP tool descriptions offered to a model. |

### Diagnostics & misc

| Variable | Description |
|----------|-------------|
| `AIMEE_ANTIPATTERNS_BYPASS` | Bypass the guardrail antipattern checks. |
| `AIMEE_IR_PATH` | Diagnostic path for recording canonical request IR. |
| `AIMEE_IR_RESP_PATH` | Diagnostic path for recording canonical response IR. |
| `AIMEE_IR_SHADOW` | Run the canonical IR path in comparison/shadow mode. |
| `AIMEE_IR_STREAM_RELAY` | Enable the canonical streaming-response relay. |
| `AIMEE_LOG_LEVEL` | Log level: `error` | `warn` | `info` | `debug`. |
| `AIMEE_TEST_PG_URL` | PostgreSQL URL used only by live test harnesses. |
| `AIMEE_WITNESS_CADENCE_TEST_S` | Test-only override that shortens witness checkpoint and verification cadence. |
| `AIMEE_WITNESS_HARNESS_ROLE` | Restricted PostgreSQL role used by the witness live harness. |

### Agents & delegates

| Variable | Description |
|----------|-------------|
| `AIMEE_EXEC_PIPE_TIMEOUT_MS` | How long a sidecar subprocess (embed, cognify, rewrite, css render, oauth token, guardrails) may run before it is killed, in ms. Default 120000. Bounds the pathological case, not normal latency: an unbounded wait here parks the calling request thread permanently when a sidecar hangs instead of exiting, which has taken a kb offline while it still accepted connections. On expiry the child's whole process GROUP is killed, because the immediate child is /bin/sh and the work is its child. |

### Managed KB and inference

| Variable | Description |
|----------|-------------|
| `AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE` | Explicit first-boot migration/adoption transport for an existing wizard-managed LLM credential. aimee-server seals it into Vault and scrubs the environment before normal startup. Ordinary inherited SYNTHESIS_API_KEY is ignored by managed credential creation so stale child-service state cannot win. Must be a 32..512 character RFC 6750 b64token. |
| `AIMEE_OFFLINE_ALLOW_NO_SWAP_MLOCK_FALLBACK` | Internal managed-authority switch: still attempts mlockall first, but when an unprivileged container cannot raise RLIMIT_MEMLOCK, permits the offline one-shot to continue only if the kernel reports no active swap. Operator-run custody tools leave this unset and retain mandatory mlockall. |

## External & provider environment

Standard and third-party environment variables aimee honors (scanned non-`AIMEE_*` `getenv()` reads, plus provider keys resolved via `api_key_env`). Provider API keys are credentials: their environment variable names are overridable per agent, but values are accepted only as first-boot transport, sealed into Vault, and scrubbed before services start. Standard OS variables (`HOME`, `PATH`, `TMPDIR`, `XDG_*`, …) are used for their usual purposes and are not aimee configuration.

### Provider credentials

| Variable | Description |
|----------|-------------|
| `ANTHROPIC_API_KEY` | Anthropic API key (read via the agent's `api_key_env`). |
| `GEMINI_API_KEY` | Google Gemini API key (read via the agent's `api_key_env`). |
| `GEMINI_API_KEY_AUTH_MECHANISM` | Selects the Gemini key auth mechanism. |
| `GOOGLE_API_KEY` | Google API key fallback for Gemini (via `api_key_env`). |
| `OPENAI_API_KEY` | OpenAI API key (default for OpenAI-family agents). |
| `SYNTHESIS_API_KEY` | Bearer credential used by the generic llm-chat sidecar; prefer the vault or a secret command. |

### Provider endpoints

| Variable | Description |
|----------|-------------|
| `LLAMA_HOST` | llama.cpp server host/URL. |
| `OLLAMA_HOST` | Ollama server host/URL for local models. |
| `SYNTHESIS_CA_FILE` | CA that verifies the synthesis sidecar's certificate on the kb -> aimee-llm hop. REPLACES the system trust store for that endpoint, so set it only for a sidecar the kb's own CA issued. |
| `SYNTHESIS_CERT_FILE` | Client certificate the kb presents to the synthesis sidecar, whose terminator requires one. Offered only to the host:port `SYNTHESIS_ENDPOINT` names. |
| `SYNTHESIS_ENDPOINT` | OpenAI-compatible base URL used by the generic llm-chat sidecar. |
| `SYNTHESIS_KEY_FILE` | Private key for `SYNTHESIS_CERT_FILE`. |
| `SYNTHESIS_MODEL` | Model requested by the generic llm-chat sidecar. |

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
| `CODEX_SANDBOX` | Codex sandbox mode. |
| `CODEX_THREAD_ID` | Codex conversation/thread id. |

### Undocumented (add to `EXT_DESC`/`EXT_OS_IGNORE` in gen-reference-docs.py)

`EMBEDDER_DIMS`, `EMBEDDER_URL`, `SYNTHESIS_AUTH_REQUIRED`

## Workflow engine

Workflows are block-composed YAML definitions under `$AIMEE_HOME/workflows/<name>.yaml`, authored with the `aimee workflow` CLI or the web visual composer and saved in canonical form. The Go workflow engine owns execution. A run is a durable work item pinned to a definition version.

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

| Block | Required input ports and accepted artifacts | Produces |
|-------|---------------------------------------------|----------|
| `author.proposal` | `proposal` (optional); accepts `proposal` | `proposal` |
| `trigger.watch-dir` | none | `proposal` |
| `author.plan` | `proposal` (required); accepts `proposal` | `plan` |
| `implement` | `plan` (required); accepts `plan`, `intent` | `branch` |
| `document` | `branch` (required); accepts `branch` | `branch` |
| `source.archive` | `branch` (required); accepts `branch` | `branch` |
| `freeze` | `branch` (required); accepts `branch` | `frozen_diff` |
| `gate.roundtable` | `src` (required); accepts `proposal`, `plan`, `frozen_diff`; requires param `roundtable` | `verdict` |
| `gate.human` | `src` (required); accepts `proposal`, `plan`, `branch`, `frozen_diff`, `pr` | `approval` |
| `pr.open` | `src` (required); accepts `proposal`, `frozen_diff` | `pr` |
| `merge` | `pr` (required); accepts `pr` | `none` |
| `gate.ci` | `pr` (required); accepts `pr` | `verdict` |
| `check.mergeable` | `pr` (required); accepts `pr` | `verdict` |
| `understand` | none | `intent` |
| `split` | `intent` or `plan` (one required); accepts `intent`, `plan` | `plan` |
| `review` | `src` (required); accepts `frozen_diff`, `branch` | `verdict` |
| `gate.deliver` | `verdict` (required); accepts `verdict`, `approval` | `none` |
| `branch.open` | none | `branch` |
| `foreach.workflow` | `packets` (required), `feature` (required); accepts `plan`, `branch` | `branch` |

### Block parameters (`params:`)

- **Review panels:** `gate.roundtable` requires `roundtable`. Its optional `panel.required`, `panel.eligible`, and `quorum` fields select the seats. Quorum must be between one and the configured persona count.
- **Human gates:** `gate.human` parks until the browser or API records an approve or reject decision. The current record is a hashed approval artifact and lifecycle transition, not a cryptographic principal signature.
- **Loop budgets:** `max_rounds` limits repeated execution of one node. Blocks also read parameters such as `workflow`, `max_children`, `base`, `persona`, `focus`, and trigger workspace settings.

### Custom blocks: `$AIMEE_HOME/workflows/blocks.yaml`

Operator-owned and refused if it is a symlink or group/world-writable. It adds blocks to the catalog above:

```yaml
allow_command: false       # opt-in gate for command blocks
command_timeout_ms: 60000  # bounded timeout for command blocks
blocks:
  - name: <block-name>     # must not shadow a built-in or duplicate
    consumes: <artifact>   # input artifact type, or none (a source)
    produces: branch|none  # custom blocks cannot mint verdict/approval/pr
    executor: command|delegate
    command: [ /abs/path/to/tool, arg1, ... ]  # command executor, no shell
    command_sha256: <hex>  # digest of the executable
    persona: <name>        # executor: delegate
    prompt: <text>         # executor: delegate
```

### Run-level controls (not in the definition)

- **Per-node loop cap**: `params.max_rounds` bounds retries for a node that loops through `on_fail` (default `20`). Exhaustion parks the run with `retry_limit` or a more specific convergence reason. The retired `max_iters` and `on_max` fields are ignored by the Go engine.
- **Cost cap**: an optional per-work-item USD ceiling is set when the run is created. The engine parks the run when cumulative cost reaches it.
- **Trigger mode**: `interactive` or `autonomous` is recorded at admission. The current Go scheduler advances both the same way, so use `gate.human` or manual pause for an approval boundary.

### Workflow environment overrides

`AIMEE_WFE_RUNNER_URL` and `AIMEE_WFE_RUNNER_SOCKET` select a compatibility runner. `AIMEE_AUTONOMY_CONCURRENCY` supplies the startup fallback for global scheduler concurrency; live `autonomy.*` configuration then controls the running service. Legacy C variables are identified in the environment table.

## Other configuration files

Beyond the config store, aimee reads a few standalone JSON/policy files (paths under `$AIMEE_HOME` unless an env override is set).

### `agents.json`: agent / model definitions

`{"default_agent": "<name>", "agents": [ {<agent>}, … ]}`. Each agent object's non-credential fields (credential fields are vault-held and deliberately not enumerated here):

| Field | Description |
|-------|-------------|
| `agents` | Top-level: array of agent definitions. |
| `api_key` | Inline API key (prefer `api_key_env` or the vault). |
| `api_key_env` | Env var name holding the agent's API key. |
| `auth_cmd` | Command that prints an auth token. |
| `auth_type` | Auth scheme (bearer / oauth / none). |
| `auto_compact_pct` | Context % at which to auto-compact. |
| `backend` | Execution backend (http / cli / ssh / docker). |
| `catalog_provider` | Catalog vendor key used for model lookups (`anthropic`, `openai`, …), when it differs from the wire `provider`. |
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
| `default_delegate` | Delegate this agent hands work to when a task arrives with no explicit delegate. Lets a project pin its own worker without every caller passing --delegate. |
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
| `is_server_hosted` | Whether the provider session is hosted by the aimee server. |
| `max_parallel` | Max concurrent calls to this agent. |
| `max_reconnects` | Max reconnect attempts (streaming / relay). |
| `max_scope` | Largest task scope this agent may be given (`bounded` or `whole_task`). Routing never relaxes this. |
| `max_tokens` | Max output tokens. |
| `max_turns` | Max agent-loop turns. |
| `middleware` | Per-agent middleware overrides (e.g. `context_window`, `max_tokens`). |
| `model` | Model name. |
| `models` | Provider-general registration: the models to expand into individual routable agents. Omit to expand every routable model the catalog lists. |
| `name` | Agent identifier. |
| `network` | Network mode (backend sandbox). |
| `networks` | Allowed networks. |
| `personas` | Personas this agent may be dispatched AS (engineer, architect, …); `"all"` or omitted = every persona. |
| `port` | Target port (relay / tunnel). |
| `price_cached_per_mtok` | Cached-input price, USD per million tokens. Overrides the catalog price. |
| `price_in_per_mtok` | Input price, USD per million tokens. Overrides the catalog price. |
| `price_out_per_mtok` | Output price, USD per million tokens. Overrides the catalog price. |
| `primary_only` | Restrict this agent to primary sessions; do not use it for delegates. |
| `provider` | Provider name. |
| `recommended_sampling` | Provider-recommended sampling parameters. |
| `reconnect_delay` | Delay between reconnects (ms). |
| `registration` | Name of the provider registration this agent was expanded from. Set automatically; used to prefer same-provider peers during fallback. |
| `relay_key` | Relay auth key. |
| `relay_ssh` | SSH relay config. |
| `roles` | Roles this agent serves (review, plan, …); `"all"` = every role. |
| `session_reuse` | Reuse a session across calls. |
| `ssh_entry` | SSH entry point (ssh backend). |
| `ssh_key` | SSH key path (ssh backend). |
| `stall_threshold` | Stall-detection threshold. |
| `target_host` | Target host (relay / tunnel). |
| `target_port` | Target port (relay / tunnel). |
| `tier_price_exempt` | Reason this agent is exempt from the `cost_tier`-vs-price lint (e.g. a flat-rate seat whose per-token price is not meaningful). |
| `timeout_ms` | Per-call timeout (ms). |
| `tools_enabled` | Allow tool use for this agent. |
| `tunnel` | Tunnel config. |
| `tunnels` | Tunnel definitions. |
| `user` | Remote user (ssh backend). |

> **Undocumented agent fields** (add to `AGENT_FIELD_DESC`): `max_output`

### Toolsets: `AIMEE_TOOLSETS_CONFIG` (or the config `toolsets` map)

Named tool allowlists. `{"toolsets": {"<name>": { … }}}`; each toolset:

- `tools` / `allowed_tools`: the tool names the set permits.
- `include`: inherit another toolset's tools.
- `script`: script-tool configuration for the set.

### Guardrails: `AIMEE_GUARDRAILS_PATH`

A policy file governing path read/write classification and pre-tool enforcement (antipattern blocking). It is a behavioral policy rather than a flat key schema; the tunable thresholds are exposed as the `guardrails` section + `guardrails_semantic_*` / `guardrail_mode` keys documented above.

## Coverage & limitations

This reference is generated by scanning the canonical source tables, which covers the scalar/keyed config surface but has known blind spots. They are listed here so a reader can tell *deliberately out of scope* from *not auto-derived*:

- **Array/object element fields** are captured when the parser iterates with `cJSON_ArrayForEach` over a section's array; fields read through other access patterns (`cJSON_GetArrayItem`, indexing) or nested more than one object deep may appear only under their parent section name.
- **Env vars built at runtime** (a name assembled with `snprintf`/concatenation and passed to `getenv(var)`) are not discoverable by the string-literal scan. Provider API-key vars are the known case and are handled via each agent's `api_key_env`; only the common defaults are listed.
- **Compile-time `-D` defines** used as build-level configuration are not scanned (they are not runtime-overridable config).
- **Separate config files**: `agents.json`, toolsets, guardrails, and custom workflow blocks (`blocks.yaml`) / workflow definitions are documented in their own sections above. Per-agent field set is scanned from `agent_config.c`; the guardrails *policy* is behavioral (path classification + pre-tool enforcement), with its tunables exposed as config keys.

If the scan ever finds a config var with no description, it is emitted under an **Undocumented** heading in the relevant section, so a new option cannot silently bypass this reference.
