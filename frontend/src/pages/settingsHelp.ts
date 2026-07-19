/* Plain-English help for the Settings page. Keyed by the exact config key the
 * server's config_fields allowlist exposes. Descriptions are grounded in the
 * config_t field comments in src/headers/config.h — keep them in sync when a
 * field's behaviour or default changes. A key with no entry here still renders;
 * it just shows no help line, so the page degrades gracefully for new keys. */

// Fields that only take effect after a server restart. This is the exhaustive
// set of keys carrying reload_class RELOAD_RESTART in src/config_fields.c (the
// Postgres pool + the kb API listener bind at startup); every other exposed key
// applies on the next turn. Keep in sync if a field's reload_class changes.
export const RESTART_KEYS = new Set<string>([
  "db2_url", "kb_api_http_port", "kb_api_bearer_token",
  // Deploy-topology (page-2) record: the deploy layer reads these and the
  // topology (which containers run) only changes on a restart — RELOAD_RESTART
  // in src/config_fields.c. (embedding_endpoint/model/dim apply on the next turn.)
  "kb_mode", "kb_client_url", "kb_client_bearer_token",
  "llm_embed_backend", "llm_embed_host", "llm_embed_gpu", "llm_embed_tier",
  "llm_rerank_backend", "llm_rerank_host", "llm_rerank_gpu", "llm_rerank_tier", "llm_rerank_endpoint",
  "llm_synth_backend", "llm_synth_host", "llm_synth_gpu", "llm_synth_tier", "llm_synth_endpoint",
  "llm_synth_model",
]);

// One line per section (see category() in Settings.tsx).
export const SECTION_HELP: Record<string, string> = {
  "Providers & delegates":
    "The primary agent aimee drives, the embedding backend, and how work is handed to delegates.",
  "Knowledge base": "The typed-fact and knowledge-base layers, plus search limits.",
  "Knowledge — PDF ingest":
    "How PDFs are pulled into the knowledge base. Most of these need a sidecar service and are off by default.",
  "Knowledge curation": "Background curation, mining, and synthesis over the knowledge base.",
  Memory:
    "How aimee stores, ranks, and recalls memory. Most retrieval-quality knobs are off by default — turn them on one at a time and watch the results.",
  "Gateway & context":
    "What aimee adds to a request and how it trims context to save tokens. Most are off by default.",
  "Audit & governance": "Logging of actions and the guardrails that gate risky ones.",
  "Agent behavior": "How autonomously aimee runs, per-turn limits, and fix verification.",
  "Learning & intelligence": "Feedback-driven learning and the reranking knobs it feeds.",
  Other: "Options that don't fall under the sections above.",
};

// One line per config key. Plain language, states the default.
export const FIELD_HELP: Record<string, string> = {
  // Knowledge curation — curator pipeline stage gates (see the Pipeline tab)
  kb_curator_extract_docs_enabled:
    "Curator: extract structured claims and entities from ingested documents (LLM). The entry stage for document knowledge; feeds claim indexing and contradiction detection.",
  kb_curator_extract_code_enabled:
    "Curator: extract code units (functions/types) from indexed source for the code knowledge graph (LLM).",
  kb_curator_resolve_entities_enabled:
    "Curator: resolve and deduplicate extracted entities into canonical records (LLM).",
  kb_curator_index_narrative_enabled:
    "Curator: embed narrative summaries into the vector store (CPU/index lane).",
  kb_curator_index_claims_enabled:
    "Curator: embed extracted claims into curator_claim_vectors — the prerequisite for contradiction detection (CPU/index lane).",
  kb_curator_detect_contradictions_enabled:
    "Curator: flag contradicting claims (same subject + attribute, different value) as 'contradicts' artifact links (CPU/index lane). Requires claim indexing to be on.",
  kb_curator_index_code_unit_enabled:
    "Curator: embed extracted code units into the vector store (CPU/index lane).",
  kb_curator_link_artifacts_enabled:
    "Curator: build relationship links between related artifacts (CPU/index lane).",
  kb_curator_synthesize_enabled:
    "Curator: synthesize per-topic summaries from clustered knowledge (LLM).",
  kb_curator_promote_entity_enabled:
    "Curator: promote well-supported entities to durable canonical status (LLM).",
  kb_curator_projection_graph_enabled:
    "Curator: publish the code projection graph (typed edges per changed project) and refresh cross-repo metadata (CPU/index lane).",
  kb_curator_cross_repo_graph_enabled:
    "Curator: keep cross-repo precision metadata (identities, routes, build deps, distinctiveness) fresh; also gates cross-repo dependency resolution. Refreshed alongside the projection graph.",
  kb_evidence_embed_enabled:
    "Curator: embed evidence spans backing each claim, for grounding and citation (CPU/index lane).",
  // Providers & delegates
  guardrail_mode:
    "How aimee handles a risky tool call: 'approve' asks you first (default), 'prompt' warns but proceeds, 'deny' refuses it outright.",
  db2_url: "Postgres connection URL for the shared knowledge store (DB2). Leave blank to use the bundled Postgres the deploy stack spawns automatically; set it only to point at an existing database. Changing it needs a server restart.",
  provider: "Which primary agent aimee drives — e.g. claude, codex, or an openai-compatible endpoint.",
  default_persona:
    "The persona a fresh primary session starts as, and the persona draft roundtable panelists author with when none is set. Defaults to 'engineer' (e.g. qa, security, reviewer, architect, or a custom persona).",
  claude_model:
    "Model to force when the primary is Claude, passed as --model on launch. Blank uses the CLI's own default.",
  openai_endpoint: "Base URL for an OpenAI-compatible primary, e.g. https://api.openai.com/v1.",
  openai_model: "Model name for an OpenAI-compatible primary, e.g. gpt-4o.",
  openai_key_cmd:
    "Shell command that prints the API key for the OpenAI-compatible primary. Keeps the key out of the config file.",
  embedding_command:
    "Command that turns text into an embedding vector (text on stdin, JSON float array on stdout). Blank falls back to the built-in 384-dim hash, which only works in a test setup.",
  embedding_model: "Name of the embedding model, recorded next to the vectors so a model change can be spotted.",
  embedding_endpoint: "HTTP endpoint for the embedder when it's a service rather than a command.",
  embedding_dim:
    "Vector width the embedder produces (e.g. 1024, 2560). Has to match what the database columns expect.",

  // Deploy topology (setup wizard page 2). The deploy layer reads these; the
  // topology only changes on a restart. Set them from the wizard's Deploy page.
  kb_mode:
    "Where the knowledge base runs: 'local' deploys an aimee-kb here; 'remote' connects to an existing one (see kb_client_url).",
  kb_client_url: "URL of an existing aimee-kb to connect to when kb_mode is 'remote'. Nothing is deployed locally.",
  kb_client_bearer_token: "Bearer token for the remote aimee-kb (kb_mode='remote'). Needs a restart.",
  llm_embed_backend: "How the embedder is served: 'local' (on the shared aimee-llm container), 'external' (an endpoint), or 'off'.",
  llm_embed_host: "Host that runs the local aimee-llm container serving the embedder.",
  llm_embed_gpu: "GPU index on the host for a local embedder; blank runs it on CPU.",
  llm_embed_tier: "Local embedder tier: cpu / small / mid / large (sizes the model to the card).",
  llm_rerank_backend: "How the reranker is served: 'local', 'external', or 'off'.",
  llm_rerank_host: "Host that runs the local reranker.",
  llm_rerank_gpu: "GPU index on the host for a local reranker; blank runs it on CPU.",
  llm_rerank_tier: "Local reranker tier: cpu / small / mid / large.",
  llm_rerank_endpoint: "Endpoint URL for an external reranker.",
  llm_synth_backend: "How the synthesizer is served: 'local', 'external', or 'off'.",
  llm_synth_host: "Host that runs the local synthesizer.",
  llm_synth_gpu: "GPU index on the host for a local synthesizer; blank runs it on CPU.",
  llm_synth_tier: "Local synthesizer tier: cpu / small / mid / large. CPU serves the Tier-A model only.",
  llm_synth_endpoint: "Endpoint URL for an external synthesizer.",
  llm_synth_model: "Model name the synthesizer serves.",

  delegate_graph_context_enabled:
    "Prepend the callers and dependencies of the files a delegate task mentions to its prompt. Advisory, off by default.",

  // Knowledge base
  typed_facts_enabled: "Turn on the typed-fact knowledge layer. Off by default.",
  kb_search_max_results:
    "Cap on results from a knowledge-base search. Requests asking for more are clamped to this. Default 50.",
  kb_api_http_port: "TCP port for the knowledge-base REST API. 0 disables it. Needs a restart.",
  kb_api_bearer_token:
    "Bearer token required by the knowledge-base API. Blank means no auth. Can be scoped for limited access. Needs a restart.",
  kb_evidence_emit_enabled: "Record what evidence each answer pulled, per turn. Off by default.",
  css_style_graph_enabled:
    "Build the CSS style graph while indexing (for the CSS migration assistant). On by default; off keeps only the plain class-name scan.",
  css_render_command:
    "Command that renders HTML/CSS to a computed-style snapshot for the CSS oracle. It runs untrusted markup, so point it at an isolated sidecar. Blank disables it.",

  // Knowledge — PDF ingest
  kb_pdf_ingest_enabled:
    "Send uploaded PDFs through the geometry-aware extractor instead of plain text extraction. Off by default.",
  kb_pdf_vector_enabled: "Embed PDF chunks so they're searchable by meaning, not just keyword. Off by default.",
  kb_pdf_tsr_enabled:
    "Run the table-recognition sidecar at ingest to pull tables out of PDFs as cells. Off by default; needs the sidecar.",
  tsr_command: "Endpoint or command for the table-recognition sidecar.",
  kb_pdf_assets_enabled:
    "Render figure and table crops from PDFs into the blob store so they can be served back. Off by default; needs pdftoppm.",
  kb_pdf_blob_dir: "Where PDF image crops are stored. Blank uses the default blob directory.",
  kb_pdf_blob_recon_secs:
    "How often (seconds) to sweep for orphaned PDF blobs. 0 turns the sweep off. Default 3600.",
  kb_pdf_blob_orphan_alarm_mb:
    "Warn when reclaimable orphaned PDF storage passes this many MB. Default 1024. 0 turns the warning off.",
  kb_pdf_ocr_enabled:
    "OCR a scanned PDF (one with no text layer) at ingest. Off by default; needs the OCR sidecar.",
  ocr_command: "Endpoint or command for the OCR sidecar.",

  // Knowledge curation
  kb_mining_enabled:
    "Run the background miner that finds recurring patterns in the knowledge base. Off by default.",
  kb_mining_min_poll_s: "Fewest seconds between miner passes. Default 300.",

  // Memory — retrieval quality
  memory_rerank_enabled:
    "Second-pass scoring of retrieved memories with a cross-encoder for sharper ordering. Off by default; needs a rerank command set.",
  memory_rerank_command:
    "Command that scores (query, candidate) pairs for the reranker. Blank disables reranking even when it's switched on.",
  memory_rerank_mode: "Which reranking strategy runs on retrieved memories.",
  memory_rerank_top_k: "How many candidates the reranker scores. Default 50.",
  memory_query_expansion_mode: "How queries are widened before search: lexical (default) or semantic.",
  memory_query_expansion_k: "How many extra related terms to add when widening a query. Default 5.",
  memory_coref_mode: "How pronouns and references in stored text are tied back to their subject. Blank leaves it off.",
  memory_coref_window: "How many nearby turns coref looks at when resolving a reference.",
  memory_rewrite_enabled: "Rewrite a query before searching (see the HyDE and decompose options). Off by default.",
  memory_rewrite_command:
    "Command that does the query rewrite. Blank disables rewriting even when it's switched on.",
  memory_rewrite_hyde:
    "Generate a hypothetical answer and embed that instead of the raw question. Often finds better matches. Off by default.",
  memory_rewrite_decompose: "Split a compound question into sub-questions and search each. Off by default.",
  memory_rewrite_max_subqueries: "Cap on sub-questions when decomposing. Default 4.",
  memory_window_radius:
    "When a stored conversation turn matches, also pull this many turns before and after it for context. 0 is off; 1–3 is typical.",
  memory_negation_enabled:
    "Track explicit absences ('X is not Y') so negative facts are searchable. Off by default.",
  memory_scenes_enabled:
    "Cluster conversation turns into scenes and use them to focus retrieval. Off by default.",
  memory_bm25_weight: "How much keyword (BM25) matching counts in the blended score. 0 uses the profile default.",
  memory_semantic_weight:
    "How much semantic (embedding) matching counts in the blended score. 0 uses the profile default.",
  memory_semantic_floor_scale:
    "Minimum-similarity cutoff for semantic memory matches. 0 auto-scales it to the embedder; a positive value pins it.",
  memory_fetch_budget_enabled:
    "Size the candidate pool by how specific the query is, instead of a fixed number. Off by default.",
  memory_fetch_budget_base: "Base candidate-pool size when the dynamic budget is on. Default 128 (range 32–512).",
  memory_fetch_budget_shape_aware:
    "Widen or shrink the candidate pool by query shape — list and quantitative queries widen it, yes/no shrinks it. On by default once the fetch budget is on.",
  memory_abstain_enabled: "Refuse to answer from weak evidence rather than guess. Off by default.",
  memory_abstain_gate: "Confidence cutoff below which aimee abstains. Effective default 0.40 when abstain is on.",
  memory_chunk_min_confidence: "Drop retrieved chunks below this confidence. 0 turns the floor off. Range 0–1.",
  memory_profile_cards_enabled:
    "Build per-entity summary cards from accumulated observations. Off by default.",
  memory_profile_cards_min_obs: "How many observations before an entity gets a profile card. Default 10.",
  memory_profile_cards_stale_secs: "Rebuild a profile card after this many seconds. Default 86400 (one day).",
  memory_improve_dedupe_enabled: "During maintenance, merge duplicate memory entries. Off by default.",
  memory_improve_summarise_enabled:
    "During maintenance, collapse clusters of related memories into a summary. Off by default.",
  memory_maintenance_trigger_inserts: "Run memory maintenance after this many inserts.",
  memory_maintenance_trigger_secs: "Run memory maintenance after this many seconds.",
  memory_hard_negative_log:
    "File path to log failing-eval candidates for later tuning. Blank turns the log off.",

  // Gateway & context
  ingress_preinject_enabled:
    "For an OpenAI/Codex primary, prepend a recalled-context block to the request so the model stops re-exploring the repo. Off by default.",
  ingress_preinject_anthropic_enabled:
    "The same context pre-injection, but on the Claude /v1/messages path. Off by default; kept separate because it can disturb Claude's prompt cache.",
  ingress_preinject_assembly_budget: "Max size (bytes) of the pre-injected context block.",
  ingress_compress_enabled:
    "Fold long code snippets in the injected context down to file:line references the model can expand on demand. Off by default.",
  ingress_compress_min_chars:
    "Only fold a code snippet to a reference when it's longer than this many characters. Default 80.",
  ingress_cache_placement_enabled:
    "Put the volatile context block after the stable instructions instead of before, so the provider's prompt cache isn't thrown away each turn. Off by default.",
  ingress_max_raw_scans: "Cap on how many raw file scans the context builder does per request.",
  code_span_max_lines: "Longest span of lines a code-reference expansion returns in one call.",
  tool_output_max_bytes:
    "Cap on how many bytes of a single tool result the model sees. 0 uses the built-in 32 KB. Lower it to stop big outputs flooding the context.",
  gateway_prevent_subagents:
    "Strip sub-agent-spawning tools (Task/Agent and the like) out of proxied requests so a served model can't fan out. Off by default.",
  gateway_pin_model:
    "Force proxied requests to the configured primary model, ignoring the model the client asked for. Off by default. Handy for single-model local backends.",
  virtual_context_enabled:
    "Build compact tool-chain stubs to manage the prompt working set. On by default; off falls back to raw turns.",
  virtual_context_assembly_budget: "Max bytes of tool-chain stubs injected into delegate prompts. Default 4096.",
  cache_aware_rewrite_enabled:
    "Hold off rewriting the request payload until it pays off against the prompt cache. Off by default.",
  cache_shaping_enabled:
    "Mark aimee's stable system prompt as cacheable on the Claude path so the provider caches it across calls. Off by default. Claude only.",
  cache_min_chars:
    "Only cache-mark a system prompt longer than this many bytes. 0 always marks it when shaping is on.",
  dedup_enabled:
    "Serve an identical repeated request from a short cache instead of paying for it again. Off by default. Per-account, so callers never see each other's responses.",
  dedup_window_seconds: "How long a deduplicated response stays cached. Default 5.",
  ingress_usage_accounting_enabled: "Record cost rows for proxied requests. Off by default.",
  ingress_audit_async:
    "Write those cost rows on a background thread so the response isn't held up. Off by default (written inline).",
  ingress_trusted_proxy_secret:
    "Shared secret that lets a front proxy stamp the caller identity on a request. Blank means no client-supplied identity is ever trusted — aimee derives it from the socket peer instead.",
  reasoning_cap_enabled:
    "Lower the reasoning effort on simple turns automatically. Only ever lowers, never raises. Off by default.",

  // Audit & governance
  audit_action_enabled:
    "Log every governed tool call to the audit log. On by default. Logging only — it never blocks an action.",
  audit_worm_enabled:
    "Also write each audit row into the append-only, hash-chained WORM store so the trail is tamper-evident. Off by default. The WORM store isn't authoritative yet, so a failed write is recoverable audit loss, never an enforcement change.",
  guardrails_semantic_enabled: "Run the ML sidecar that scores prompts for injection or abuse. Off by default.",
  guardrails_semantic_dry_run:
    "Run the semantic guardrail in shadow mode — scores are logged but never change the outcome. On by default; always start here.",
  guardrails_semantic_command:
    "Command for the semantic-guardrail sidecar. Blank means no scoring runs.",
  guardrails_semantic_warn_threshold: "A score at or above this produces a warning.",
  guardrails_semantic_prompt_threshold: "A score at or above this flags a prompt-injection concern.",
  guardrails_semantic_block_threshold: "A score at or above this would block, once blocking is enabled.",
  guardrails_semantic_allow_ml_only_block:
    "Let the ML score block on its own, with no rule backing it. Off by default; needs precision evidence first.",
  guardrails_blast_radius_advisory_enabled:
    "Show which dependent files an edit could touch before it runs. Advisory only. Off by default.",
  integrity_enabled:
    "Run the pattern-based gate that screens untrusted content at write entry points. Off by default.",
  integrity_dry_run:
    "Run the integrity gate in shadow mode — it logs but never blocks. On by default, so enabling the gate is safe to try first.",
  require_session_worktree:
    "Refuse file-changing tools unless they run inside an aimee-managed worktree, forcing each session onto its own branch. Off by default.",
  fidelity_check_enabled:
    "Run a judge that checks answers against their cited evidence. Off by default; needs evidence logging and context pre-injection on first.",

  // Agent behavior
  autonomous:
    "Launch the agent CLI with its full autonomous flags, relying on aimee's guardrails for safety. Off by default.",
  cross_verify:
    "Have delegates check the primary's fixes and the primary check delegates' fixes. Off by default.",
  max_iterations: "Max tool-call rounds in one interactive turn. 0 uses the default (15).",
  max_iterations_delegate: "Max tool-call rounds in one delegate turn. 0 uses the default (25).",
  verify_enabled:
    "Automatically gate pushes and PRs and generate an enforcing project.yaml. Off by default — only repos with an explicit enforcing project.yaml are gated. Manual 'aimee git verify' still works either way.",
  verify_cross_project:
    "Let verify run and enforce across other repositories, not just the session's own. Off by default.",
  wfe_live_forge_enabled:
    "Let autonomous runs actually open and merge real PRs. Off by default. Only turn this on deliberately, with branch protection and scoped credentials in place — it writes to your repo for real.",

  // Learning & intelligence
  learning_router_enabled:
    "Turn explicit feedback into a reviewed proposal before it's committed, instead of applying it straight away. Off by default.",
  learning_proposal_ttl_days: "How long a learning proposal stays open before it expires.",
  learning_max_commits_per_week: "Cap on learning changes committed per week.",
  learning_implicit_citation_repair:
    "Treat a correction right after a memory call as negative feedback, on its own. Off by default; needs the learning router on.",
  learning_implicit_citation_continuation:
    "Treat continuing the task right after a memory call as positive feedback, on its own. Off by default; needs the learning router on.",
  learning_implicit_repeat_question:
    "Treat a repeated question as negative feedback, on its own. Off by default; needs the learning router on.",
  learning_implicit_repeated_correction:
    "Treat repeated corrections on the same thing as negative feedback. Off by default; needs the learning router on.",
  learning_implicit_workflow_repetition:
    "Treat a re-saved workflow as a learning signal. Off by default; needs the learning router on.",
  code_hybrid_weight_code:
    "Weight of the lexical-code signal in code-search ranking. Default 1.0. 0 or less turns the signal off.",
  code_hybrid_weight_graph:
    "Weight of the code-graph (structure) signal in code-search ranking. Default 1.0. 0 or less turns it off.",
  code_hybrid_weight_vector:
    "Weight of the vector (meaning) signal in code-search ranking. Default 1.0. 0 or less turns it off.",
  code_hybrid_weight_memory:
    "Weight of the memory signal in code-search ranking. Default 1.0. 0 or less turns it off.",
  code_hybrid_rrf_k:
    "Rank constant for blending the code-search signals. Higher flattens the weighting. Default 60.",
  code_trust_actuation_enabled:
    "Let a project's learned lessons break ties in code-search ranking — tie-break only, never over a real score gap. Off by default.",
  code_surprising_precision_floor:
    "Hide 'surprising link' code suggestions when their measured precision drops below this. 0 (default) turns the check off.",
  cost_reward_enabled:
    "Let the delegate router prefer cheaper agents when quality is similar. Off by default.",
  cost_reward_lambda_pct: "How hard the router leans on cost when cost_reward is on, as a percent. Default 30.",
  cost_reward_ref_usd_milli:
    "The per-turn cost (in thousandths of a dollar) that counts as a full cost penalty. Default 500 = $0.50.",
  identity_working_profile_injection_enabled:
    "Let the auto-learned working profile reach the system prompt. Off by default. Still respects the field allow-list.",

  // Dogfood (memory-quality logging)
  dogfood_enabled:
    "Log memory-related tool calls to a file so you can review retrieval quality over time. On by default.",
  dogfood_log_dir: "Where the dogfood logs are written. Blank uses the default directory.",
  dogfood_commit_raw:
    "Keep the raw query text in dogfood logs. Off by default — off stores only a hash, so committed logs never leak text.",
  dogfood_inline_tagging:
    "Return a small tagging hint after memory tool calls that a UI can render. Off by default.",
  dogfood_autolabel_repair:
    "Mark a prior memory result a miss when your next turn is a correction. Off by default.",
  dogfood_autolabel_continuation:
    "Mark a prior memory result a hit when your next turn just carries the task on. Off by default.",
  dogfood_autolabel_repeat_question:
    "Mark a memory result a miss when you ask the same thing again in the same month. Off by default.",
};
