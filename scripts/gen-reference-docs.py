#!/usr/bin/env python3
"""Generate CLI + configuration reference docs from the canonical source tables.

Two committed outputs (regenerate with `make -C src docs-gen`):
  docs/gen/cli-commands.md   — every `aimee` CLI command + subcommands, from the
                               client help table (src/cli_help_data.h).
  docs/gen/configuration.md  — every config key: the `aimee config get/set`
                               scalar allowlist (src/config_fields.c) plus the
                               config-file (JSON) sections parsed by src/config*.c.

The point is completeness: these are derived from the same tables the binary
uses, so they cannot silently drift from the implementation the way hand-written
lists do.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
GEN = ROOT / "docs" / "gen"

# ─── CLI commands (src/cli_help_data.h) ──────────────────────────────────────
# Each entry: {"name", "description", CLIENT_TIER_X, hidden_flag, subcmd_or_NULL}
# where subcmd is a (possibly multi-line, concatenated) C string of lines like
#   "  sub   description\n"

TIER_LABEL = {"CORE": "Core", "ADVANCED": "Advanced", "ADMIN": "Admin"}


def _c_strings(blob):
    """Concatenate adjacent C string literals, unescaping \\n and \\t."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', blob)
    s = "".join(parts)
    return s.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"')


def parse_cli_commands():
    text = (SRC / "cli_help_data.h").read_text(encoding="utf-8")
    # Each entry begins with {"<name>", and ends at the matching `},` at the
    # entry's top level. Split on the entry-start sentinel instead of brace
    # counting (the subcmd strings contain no braces).
    entries = []
    # normalize: drop the file's leading comment
    body = text[text.index('{"'):]
    # split into entries on `},\n` boundaries that precede a new `{"`
    raw = re.split(r'\},\s*(?=\{")', body)
    for chunk in raw:
        m = re.match(r'\s*\{\s*"([^"]+)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*CLIENT_TIER_(\w+)\s*,\s*(\d+)\s*,\s*(.*)$',
                     chunk, re.S)
        if not m:
            continue
        name, desc, tier, hidden, rest = m.groups()
        subs = None if rest.strip().startswith("NULL") else _c_strings(rest).strip("\n")
        entries.append({"name": name, "desc": desc, "tier": tier,
                        "hidden": hidden == "1", "subs": subs})
    return entries


def render_cli(entries):
    out = ["# CLI Command Reference",
           "",
           "> Auto-generated from `src/cli_help_data.h` by `scripts/gen-reference-docs.py`.",
           "> Do not edit by hand; run `make -C src docs-gen` to regenerate.",
           "",
           "`aimee` is a thin client: each command either runs a small local "
           "operation or forwards a typed request to `aimee-server`. Server-backed "
           "commands accept `--json` for machine-readable output. Run "
           "`aimee help <command>` for per-command help, or `aimee help --all` for "
           "every tier.",
           "",
           f"Total commands: {len(entries)}",
           ""]
    for tier in ("CORE", "ADVANCED", "ADMIN"):
        group = [e for e in entries if e["tier"] == tier]
        if not group:
            continue
        out.append(f"## {TIER_LABEL[tier]} commands")
        out.append("")
        for e in sorted(group, key=lambda e: e["name"]):
            out.append(f"### `aimee {e['name']}`")
            out.append("")
            out.append(e["desc"] + ".")
            out.append("")
            if e["subs"]:
                out.append("Subcommands:")
                out.append("")
                out.append("```")
                out.append(e["subs"])
                out.append("```")
                out.append("")
    return "\n".join(out).rstrip() + "\n"


# ─── Config: CLI-settable scalars (src/config_fields.c) ───────────────────────

CFG_TYPE = {"CFG_STRING": "string", "CFG_BOOL": "bool", "CFG_INT": "int", "CFG_FLOAT": "float"}

# Curated one-line descriptions for the CLI-settable keys (the `aimee config set`
# surface). A key in the generated table with no entry here renders "—" and is
# counted as undescribed so the gap is visible (see render_config).
CFG_KEY_DESC = {
    "autonomous": "Run autonomously (auto-advance preauthorized gates) vs interactive.",
    "cache_aware_rewrite_enabled": "Rewrite prompts to align with the provider's prompt cache.",
    "cache_min_chars": "Minimum prompt size (chars) before cache-shaping applies.",
    "cache_shaping_enabled": "Enable prompt cache-shaping.",
    "claude_cli_delegate_enabled": "Allow delegating to the local Claude CLI agent.",
    "delegate_graph_context_enabled": "Prepend a structural code-graph context block (callers/dependencies of files a delegate task references) to the delegate prompt (advisory, fail-open, default off).",
    "memory_md_retire": "Retire the agent file-memory surface into aimee (default on): a Write under ~/.claude/projects/<slug>/memory/<name>.md is intercepted into aimee's db1 and the .md is never materialized; session-start skips .md hydration. Set false for the legacy re-materialized .md mirrors.",
    "claude_model": "Default Claude model (empty = CLI default).",
    "gateway_prevent_subagents": "Gateway strips subagent-spawning tools (Task/Agent/etc.) from proxied requests so the served model cannot spawn subagents. Default off.",
    "gateway_pin_model": "Gateway forces the proxied /v1/messages served model to the configured primary's model, overriding the client-requested model. Default off (the passthrough honors the client model); enable for single-model Anthropic-compatible shims.",
    "cost_reward_enabled": "Factor token cost into the reward signal.",
    "cost_reward_lambda_pct": "Cost-penalty weight (percent) in the reward.",
    "cost_reward_ref_usd_milli": "Reference cost (USD-milli) normalizing the cost reward.",
    "cross_verify": "Enable cross-model verification of outputs.",
    "css_style_graph_enabled": "Enable the CSS migration assistant's style-graph write path during indexing.",
    "css_render_command": "Render backend for the #4-full computed-style oracle: a command reading {html,css} JSON on stdin and writing a computed-style snapshot JSON on stdout (run an isolated headless-browser sidecar).",
    "db2_url": "DB2 connection URL (aimee's vector / knowledge-base store).",
    "dedup_enabled": "Deduplicate near-identical responses.",
    "dedup_window_seconds": "Window (seconds) for response dedup.",
    "dogfood_autolabel_continuation": "Auto-label continuation turns for dogfood capture.",
    "dogfood_autolabel_repair": "Auto-label repair turns for dogfood capture.",
    "dogfood_autolabel_repeat_question": "Auto-label repeated-question turns.",
    "dogfood_commit_raw": "Commit raw (unredacted) dogfood transcripts.",
    "dogfood_enabled": "Capture sessions as dogfood training/eval data.",
    "dogfood_inline_tagging": "Inline-tag dogfood events during the session.",
    "dogfood_log_dir": "Directory for dogfood logs.",
    "ecomode": "Reduce background compute (eco mode).",
    "embedding_command": "Command that produces embeddings (overrides the endpoint).",
    "embedding_dim": "Embedding vector dimension.",
    "embedding_endpoint": "Embeddings provider endpoint URL.",
    "embedding_model": "Embeddings model name.",
    "fidelity_check_enabled": "Run the answer-fidelity judge on terminal-text turns "
    "(default off; requires kb_evidence_emit_enabled + ingress_preinject_enabled).",
    "guardrail_mode": "Guardrail enforcement mode (off / warn / block).",
    "code_hybrid_weight_code": "RRF weight for the lexical-code signal in /v1/code/hybrid (default 1.0; <=0 disables it).",
    "code_hybrid_weight_graph": "RRF weight for the structural call-graph signal in /v1/code/hybrid (default 1.0; <=0 disables it).",
    "code_hybrid_weight_vector": "RRF weight for the embedding-similarity signal in /v1/code/hybrid (default 1.0; <=0 disables it; auto-skips when no dim-matched embedder).",
    "code_hybrid_weight_memory": "RRF weight for the cross-session knowledge-graph signal in /v1/code/hybrid (default 1.0; <=0 disables it; symbol-anchored, empty without an entity graph).",
    "code_hybrid_rrf_k": "Reciprocal Rank Fusion rank constant k for /v1/code/hybrid (default 60).",
    "code_surprising_precision_floor": "§4 self-suppress: when the LLM-judge-sampled precision of surprising-link candidates falls below this floor, an unjudged /v1/code/graph/surprising request returns no candidates (default 0 = disabled).",
    "guardrails_blast_radius_advisory_enabled": "Surface a structural blast-radius advisory (graph-impacted files) before an edit (advisory, fail-open).",
    "guardrails_semantic_allow_ml_only_block": "Allow blocking on the ML classifier alone.",
    "guardrails_semantic_block_threshold": "Semantic score threshold to block.",
    "guardrails_semantic_command": "External semantic-guardrail classifier command.",
    "guardrails_semantic_dry_run": "Evaluate but don't enforce semantic guardrails.",
    "guardrails_semantic_enabled": "Enable the semantic guardrail classifier.",
    "guardrails_semantic_prompt_threshold": "Semantic score threshold for prompt-level flags.",
    "guardrails_semantic_warn_threshold": "Semantic score threshold to warn.",
    "identity_working_profile_injection_enabled": "Inject the working-profile identity into prompts.",
    "ingress_audit_async": "Audit ingress requests asynchronously.",
    "ingress_max_raw_scans": "Max raw-content scans per ingress request.",
    "code_span_max_lines": "Max line span the code_span_get recovery resolver returns per call "
    "(default 400).",
    "tool_output_max_bytes": "Per-result cap (bytes) on the model-visible tool output "
    "(read_file/bash/grep/glob/git_* results). 0 = built-in default (32768); any positive value is "
    "clamped to (0, 32768]. Set it lower to bound the bytes a single tool result adds to the "
    "prompt + history; the (default-off) context-economizer compresses older results to keep "
    "history bounded.",
    "require_session_worktree": "Fail closed on mutating ops outside an aimee-managed worktree "
    "(session-isolation guard; default off).",
    "ingress_preinject_assembly_budget": "Token budget for ingress context pre-injection.",
    "ingress_preinject_enabled": "Enable `<aimee-context>` pre-injection on ingress "
    "(memory/code preview envelope on primary ingress turns; default on).",
    "ingress_preinject_anthropic_enabled": "Inject the `<aimee-context>` envelope on the "
    "Anthropic-native /v1/messages passthrough too (default off).",
    "ingress_compress_enabled": "Enable ingress envelope compression: span-enrich code hits and "
    "fold code entries into recoverable `file:line` references (recover via code_span_get). "
    "Default on (~48% prompt reduction on code turns); turn off (or send `X-Aimee-Compress: 0`) "
    "for agentic ingress where the agent re-opens folded code so recovery round-trips can erase "
    "the saving.",
    "ingress_compress_min_chars": "Minimum code-snippet length (chars) before it is folded to a "
    "file:line reference (default 80).",
    "ingress_cache_placement_enabled": "Append the <aimee-context> envelope after the stable "
    "instructions prefix (not before) so provider prefix caches survive (default on).",
    "ingress_trusted_proxy_secret": "Shared secret authenticating a trusted ingress proxy.",
    "ingress_usage_accounting_enabled": "Account token usage on ingress requests.",
    "integrity_dry_run": "Run integrity checks without enforcing.",
    "integrity_enabled": "Enable the integrity gate.",
    "kb_api_bearer_token": "Bearer token for the aimee-kb API.",
    "kb_api_http_port": "HTTP port the aimee-kb API listens on.",
    "kb_evidence_emit_enabled": "Emit evidence records from KB ingest.",
    "kb_fusion_mode": "KB retrieval fusion mode: rrf (default), static_alpha, or dynamic_alpha.",
    "kb_fusion_static_alpha": "Lexical/dense blend weight (0-1) for the static_alpha fusion mode.",
    "kb_pdf_ingest_enabled": "Route PDF uploads through the structured geometry extractor "
    "(kb_doc_pdf) instead of plain pdftotext (default off).",
    "kb_pdf_vector_enabled": "Embed structured-PDF chunks into the isolated kb_pdf_embeddings "
    "relation and add the vector candidate leg to search_chunks (default off; degrades to "
    "lexical-only when the embedder is absent).",
    "kb_pdf_tsr_enabled": "Run the table-structure-recognition (TSR) sidecar at PDF ingest to turn "
    "table regions into structured kb_table_cells, surfaced via lookup_table (default off; "
    "degrades to text-only when the sidecar is absent).",
    "tsr_command": "TSR sidecar endpoint/command for structured-PDF table recognition (resolves "
    "like embedding_command; AIMEE_TSR_URL env fallback).",
    "kb_pdf_assets_enabled": "Render structured-PDF figure/table crops to the content-addressed "
    "blob store + kb_doc_assets at ingest, served via open_asset (default off; needs pdftoppm).",
    "kb_pdf_blob_dir": "Override the structured-PDF blob store root (default "
    "<kb-config-dir>/kb-blobs).",
    "kb_pdf_blob_recon_secs": "Interval (seconds) for the orphan-blob reconciliation sweep "
    "(default 3600; <=0 disables it).",
    "kb_pdf_blob_orphan_alarm_mb": "Warn when reclaimable orphan blob bytes exceed this many MB "
    "(default 1024; <=0 disables the alarm).",
    "kb_pdf_ocr_enabled": "OCR a scanned / no-text-layer PDF via the OCR sidecar at ingest so its "
    "text + geometry feed the normal citation path (default off; without it a scanned PDF is "
    "ingested asset-only).",
    "ocr_command": "OCR sidecar endpoint/command for structured-PDF scanned-page recognition "
    "(resolves like embedding_command; AIMEE_OCR_URL env fallback).",
    "kb_mining_enabled": "Enable background KB mining.",
    "kb_mining_min_poll_s": "Minimum interval (s) between KB mining polls.",
    "kb_search_max_results": "Default max results for KB search.",
    "learning_implicit_citation_continuation": "Implicit-learning signal: citation on continuation.",
    "learning_implicit_citation_repair": "Implicit-learning signal: citation on repair.",
    "learning_implicit_repeat_question": "Implicit-learning signal: repeated question.",
    "learning_implicit_repeated_correction": "Implicit-learning signal: repeated correction.",
    "learning_implicit_workflow_repetition": "Implicit-learning signal: workflow repetition.",
    "learning_max_commits_per_week": "Cap on learning-derived commits per week.",
    "learning_proposal_ttl_days": "TTL (days) for learning proposals.",
    "learning_router_enabled": "Enable the learning router.",
    "max_iterations": "Per-turn iteration cap for interactive chat (default 15).",
    "max_iterations_delegate": "Per-turn iteration cap for delegate sessions (default 25).",
    "memory_abstain_enabled": "Allow memory recall to abstain on low confidence.",
    "memory_abstain_gate": "Confidence gate for memory abstention.",
    "memory_bm25_weight": "BM25 (lexical) weight in hybrid memory recall.",
    "memory_chunk_min_confidence": "Minimum confidence to keep a memory chunk.",
    "memory_coref_mode": "Coreference-resolution mode for memory.",
    "memory_coref_window": "Coreference lookback window.",
    "memory_fetch_budget_base": "Base token budget for memory fetch.",
    "memory_fetch_budget_enabled": "Enable token-budgeted memory fetch.",
    "memory_fetch_budget_shape_aware": "Shape-aware memory fetch budgeting.",
    "memory_hard_negative_log": "Path to the hard-negative recall log file (empty = disabled).",
    "memory_improve_dedupe_enabled": "Dedupe during memory-improve.",
    "memory_improve_summarise_enabled": "Summarise during memory-improve.",
    "memory_kb_neighbour_expand": "Expand recall to KB neighbours.",
    "memory_maintenance_trigger_inserts": "Inserts before a maintenance cycle triggers.",
    "memory_maintenance_trigger_secs": "Seconds before a maintenance cycle triggers.",
    "memory_negation_enabled": "Detect/handle negation in memory.",
    "memory_profile_cards_enabled": "Maintain profile cards from observations.",
    "memory_profile_cards_min_obs": "Min observations before a profile card forms.",
    "memory_profile_cards_stale_secs": "Profile-card staleness (seconds).",
    "memory_query_expansion_k": "Number of expanded queries for recall.",
    "memory_query_expansion_mode": "Query-expansion mode.",
    "memory_rerank_command": "External reranker command.",
    "memory_rerank_enabled": "Enable cross-encoder reranking of recall.",
    "memory_rerank_mode": "Reranker mode.",
    "memory_rerank_top_k": "Top-K candidates to rerank.",
    "memory_rewrite_command": "External query-rewrite command.",
    "memory_rewrite_decompose": "Decompose queries during rewrite.",
    "memory_rewrite_enabled": "Enable query rewriting for recall.",
    "memory_rewrite_hyde": "Use HyDE (hypothetical-document) rewrite.",
    "memory_rewrite_max_subqueries": "Max sub-queries produced by rewrite.",
    "memory_scenes_enabled": "Cluster memories into scenes.",
    "memory_scenes_min_cluster_size": "Min cluster size for a scene.",
    "memory_scenes_top_m": "Top-M scenes to consider.",
    "memory_semantic_floor_scale": "Multiplier on the semantic-recall cosine floors (0 = auto-scale by the active embedder dimension; >0 pins it).",
    "memory_semantic_weight": "Semantic (vector) weight in hybrid recall.",
    "memory_window_radius": "Neighbour radius for memory-window expansion.",
    "openai_endpoint": "OpenAI-compatible endpoint URL.",
    "openai_key_cmd": "Command that prints the OpenAI API key.",
    "openai_model": "OpenAI model name.",
    "provider": "Default model provider.",
    "reasoning_cap_enabled": "Cap the model's reasoning effort.",
    "typed_facts_enabled": "Enable the typed-fact knowledge layer (master gate; default off).",
    "audit_worm_enabled": "Dual-write governed-action audit rows into the append-only, "
    "hash-chained WORM store alongside audit.log (default off).",
    "verify_cross_project": "Let `aimee git verify` span other projects.",
    "verify_enabled": "Master gate for `aimee git verify` (default off).",
    "virtual_context_assembly_budget": "Token budget for virtual-context assembly.",
    "virtual_context_enabled": "Enable virtual-context assembly.",
}

# One-line description per config-file section (what the section governs). Child
# keys are listed by name; deeply-nested sub-objects are noted in the description.
SECTION_DESC = {
    "aimee": "Core API/runtime settings.",
    "auxiliary": "Auxiliary (cheap/background) model used for side tasks.",
    "cache_shaping": "Prompt-cache shaping.",
    "charter": "Operating charter: values, constraints, safety axioms, tone.",
    "compact": "Transcript compaction thresholds.",
    "computer_use": "Computer-use (browser) tool settings.",
    "concurrency": "Per-model / per-provider concurrency limits.",
    "context": "Context-engine selection.",
    "cost_reward": "Cost-aware reward shaping.",
    "cron_jobs": "Scheduled job definitions (array of objects).",
    "cross_verify": "Cross-model output verification.",
    "db2": "DB2 / vector store settings.",
    "dedup": "Response deduplication.",
    "dogfood": "Session capture for dogfood data.",
    "ensemble": "Roundtable ensemble panel + aggregator.",
    "guardrails": "Semantic guardrail policy.",
    "identity": "Working-profile identity injection.",
    "ingress": "Ingress (proxy frontends) behavior.",
    "integrity": "Integrity gate.",
    "intelligence": "Intelligence subsystems (bandit, planner, ranking, reasoning) + their external commands; most children are nested objects.",
    "kb": "Knowledge-base client + curator / evidence / maintenance / mining (nested objects).",
    "learning": "Learning subsystem (router, implicit, embed, synthesize; nested objects).",
    "lsp_servers": "LSP server definitions (array of objects).",
    "mcp": "MCP integration (e.g. OSV).",
    "mcp_clients": "MCP client connections (array of objects).",
    "memory": "Memory subsystem; most children (recall, rerank, lifecycle, …) are nested objects with their own keys.",
    "memory_maintenance": "Memory maintenance scheduling.",
    "memory_negation": "Negation handling in memory.",
    "memory_query_expansion": "Recall query expansion.",
    "memory_recall_lanes": "Per-lane recall floors / caps.",
    "memory_rerank": "Recall reranking.",
    "memory_rewrite": "Recall query rewriting.",
    "memory_window": "Memory-window neighbour expansion.",
    "model_meta": "Model metadata + capability routing.",
    "otel": "OpenTelemetry export.",
    "reasoning_cap": "Reasoning-effort cap.",
    "retry": "Provider retry / backoff.",
    "rewind": "Auto-snapshot / rewind.",
    "roundtable": "Roundtable pipeline thresholds, caps, gates, and turns.",
    "sandbox": "Tool sandbox (paths, network, mode).",
    "script": "Script-tool allowlist.",
    "search": "Web-search backend (Tavily / SearXNG).",
    "session": "Session / worktree limits.",
    "skills": "Skill subsystem (capability, curator, dispatch, eval, manage, review; nested objects).",
    "transport": "Transport tweaks (cache-aware rewrite).",
    "trigger": "Trigger listener (auth, concurrency).",
    "trigger_rules": "Trigger rule definitions (array of objects).",
    "workspaces": "Workspace definitions (array of objects).",
}


def parse_config_fields():
    # Each entry is `{"<key>", offsetof(...), <size>, <flag>, CFG_<TYPE>}`. The
    # offsetof/sizeof macros embed commas, so match the key (first string before
    # offsetof) and the type (CFG_* before the closing brace) positionally — they
    # are 1:1 in source order.
    text = (SRC / "config_fields.c").read_text(encoding="utf-8")
    # Bound to the config_fields[] initializer, then parse each `{...}` entry as a
    # unit (split on `},`) so the key and its CFG_* type are paired within one
    # entry — robust to CFG_* uses in helper functions below the table.
    start = text.index("config_fields[] = {")
    text = text[start:text.index("\n};", start)]
    fields, seen = [], set()
    for chunk in text.split("},"):
        km = re.search(r'"([a-z0-9_]+)"\s*,\s*offsetof', chunk)
        tm = re.search(r'(CFG_\w+)', chunk)
        if km and tm and km.group(1) not in seen:  # a key may be registered twice
            seen.add(km.group(1))
            fields.append((km.group(1), CFG_TYPE.get(tm.group(1), tm.group(1))))
    return fields


# ─── Config: config-file (JSON) sections (src/config*.c) ──────────────────────
# Pattern: `<var> = cJSON_GetObjectItemCaseSensitive(root, "<section>")` then
# `cJSON_GetObjectItemCaseSensitive(<var>, "<key>")` for the section's keys.

ASSIGN_RE = re.compile(
    r'(\w+)\s*=\s*cJSON_GetObjectItemCaseSensitive\(\s*root\s*,\s*"([^"]+)"\s*\)')
CHILD_RE = re.compile(
    r'cJSON_GetObjectItemCaseSensitive\(\s*(\w+)\s*,\s*"([^"]+)"\s*\)')
# `cJSON_ArrayForEach(<item>, <arr>)` — element fields of an array-valued section
# are read off <item>; map <item> to the array's section so they're captured too.
FOREACH_RE = re.compile(r'cJSON_ArrayForEach\(\s*(\w+)\s*,\s*(\w+)\s*\)')


def parse_config_sections():
    sections = {}   # section name -> sorted set of keys
    flat = set()    # top-level scalar keys read straight off root
    for cfile in sorted(SRC.glob("config*.c")):
        text = cfile.read_text(encoding="utf-8")
        var_to_section = {}
        for m in ASSIGN_RE.finditer(text):
            var, sect = m.group(1), m.group(2)
            if var == "root":
                continue
            var_to_section[var] = sect
        # array iteration: the loop var inherits the array's section
        for m in FOREACH_RE.finditer(text):
            item, arr = m.group(1), m.group(2)
            if arr in var_to_section:
                var_to_section[item] = var_to_section[arr]
        # collect child keys per section-var
        used_as_parent = set()
        for m in CHILD_RE.finditer(text):
            parent, key = m.group(1), m.group(2)
            used_as_parent.add(parent)
            if parent in var_to_section:
                sections.setdefault(var_to_section[parent], set()).add(key)
        # a (root,"X") whose var is never used as a parent is a flat top key
        for var, sect in var_to_section.items():
            if var not in used_as_parent:
                flat.add(sect)
    # don't double-list a name that is both a section and a stray flat read
    flat -= set(sections)
    return sections, flat


def render_config(fields, sections, flat):
    out = ["# Configuration Reference",
           "",
           "> Auto-generated from the canonical source tables by "
           "`scripts/gen-reference-docs.py` — config keys from `src/config_fields.c` + "
           "`src/config*.c`, env vars scanned from `getenv()` in `src/`, and the "
           "workflow surface from `src/workflow/`. Do not edit by hand; run "
           "`make -C src docs-gen` to regenerate.",
           "",
           "This reference covers every configurable surface:",
           "",
           "1. **Config-store keys** — the `aimee config` keys + config-file sections (below).",
           "2. **Environment variables** — `AIMEE_*` runtime/deployment overrides.",
           "3. **External & provider environment** — provider keys, endpoints, proxy, editor.",
           "4. **Workflow engine** — workflow definition + custom-block (`blocks.yaml`) schema.",
           "5. **Other config files** — `agents.json`, toolsets, guardrails.",
           "",
           "CLI commands + flags are documented separately in "
           "[`cli-commands.md`](cli-commands.md).",
           "",
           "Configuration lives in the per-`AIMEE_HOME` config store. Scalar keys "
           "in the table below are settable from the CLI:",
           "",
           "```",
           "aimee config show                 # print the effective config",
           "aimee config get <key>            # read one value",
           "aimee config set <key> <value>    # set one value",
           "```",
           "",
           "Structured options (arrays, nested objects — e.g. `ensemble.reference_models`) "
           "are not CLI-settable; they are written into the config file under the "
           "sections listed at the end.",
           ""]

    undescribed = sorted(k for k, _ in fields if k not in CFG_KEY_DESC)
    out.append(f"## CLI-settable keys ({len(fields)})")
    out.append("")
    out.append("| Key | Type | Description |")
    out.append("|-----|------|-------------|")
    for key, typ in sorted(fields):
        out.append(f"| `{key}` | {typ} | {CFG_KEY_DESC.get(key, '—')} |")
    out.append("")
    if undescribed:
        out.append("> **Undocumented** (add to `CFG_KEY_DESC` in gen-reference-docs.py): "
                   + ", ".join(f"`{k}`" for k in undescribed))
        out.append("")

    out.append(f"## Config-file sections ({len(sections)})")
    out.append("")
    out.append("Set in the config JSON as `{\"<section>\": {\"<key>\": ...}}`. Keys "
               "are derived from the section parsers in `src/config*.c`; a key shown "
               "as a bare name that is itself a nested object is noted in the section "
               "description (see *Coverage & limitations*).")
    out.append("")
    for sect in sorted(sections):
        keys = ", ".join(f"`{k}`" for k in sorted(sections[sect]))
        desc = SECTION_DESC.get(sect)
        lead = f"_{desc}_ Keys: " if desc else ""
        out.append(f"- **`{sect}`** — {lead}{keys}")
    out.append("")

    if flat:
        out.append(f"## Other top-level config-file keys ({len(flat)})")
        out.append("")
        out.append("Scalar keys read directly from the config root (not via the CLI "
                   "allowlist above):")
        out.append("")
        out.append(", ".join(f"`{k}`" for k in sorted(flat)))
        out.append("")

    return "\n".join(out).rstrip() + "\n"


# ─── Environment variables (getenv("AIMEE_*") across src/, excluding tests) ────
# Every env var the binaries actually read. The scan is the completeness anchor;
# ENV_DESC supplies the (group, description) for each. A scanned var missing from
# ENV_DESC is surfaced under "Undocumented" so a new var can never silently slip
# the reference — keeping this gate honest is the whole point.

ENV_RE = re.compile(r'getenv\(\s*"(AIMEE_[A-Z0-9_]+)"')

# group order controls section order in the doc
ENV_GROUP_ORDER = [
    "Paths & assets", "Client & session", "Server runtime", "Knowledge base (aimee-kb)",
    "Database & vectors", "Memory", "Delegates & backends", "Forge (GitHub App / tokens)",
    "Gateway (voice / webhooks / push)", "Workflow engine", "Git verify / MCP",
    "Models", "TLS & networking", "Plugins", "Diagnostics & misc",
]

ENV_DESC = {
    # Paths & assets
    "AIMEE_HOME": ("Paths & assets", "Root of the per-user state/config store (config, DB1, `workflows/`, keys). Overrides the platform default."),
    "AIMEE_INSTALL_PREFIX": ("Paths & assets", "Install prefix used to locate bundled assets and plugins."),
    "AIMEE_BUNDLED_SKILLS_DIR": ("Paths & assets", "Override directory for the bundled skills."),
    "AIMEE_TOOLSETS_CONFIG": ("Paths & assets", "Path to a toolsets config file (overrides the default tool allowlists)."),
    "AIMEE_GUARDRAILS_PATH": ("Paths & assets", "Path to the guardrails policy file."),
    "AIMEE_FORENSICS_DIR": ("Paths & assets", "Directory for shutdown-forensics dumps."),
    "AIMEE_PACK_DIR": ("Paths & assets", "Directory of memory profile packs."),
    "AIMEE_HARNESS_MEMORY_SCOPES": ("Paths & assets", "Path to the agent memory-surface registry config (default `<AIMEE_HOME>/harness_memory_scopes.conf`). Each `client:projects_root:memory_seg` line adds a new agent or overrides a built-in's paths for memory interception/hydration."),
    "AIMEE_WORKSPACES_DIR": ("Paths & assets", "Root directory for mirrored/registered workspaces."),
    "AIMEE_MODELS_DEV_SNAPSHOT": ("Paths & assets", "Path to an offline models.dev catalog snapshot."),
    # Client & session
    "AIMEE_SERVER_URL": ("Client & session", "aimee-server endpoint the thin client connects to (UDS path or `tcp:host:port`)."),
    "AIMEE_SERVER_TOKEN": ("Client & session", "Bearer token presented to aimee-server over TCP."),
    "AIMEE_API_ENDPOINT": ("Client & session", "Override the `/v1` API endpoint used by the client RPC layer."),
    "AIMEE_API_BEARER": ("Client & session", "Bearer token for the `/v1` API endpoint."),
    "AIMEE_SESSION_ID": ("Client & session", "Pre-set the session id (enables non-blocking session attach)."),
    "AIMEE_TUI_SESSION": ("Client & session", "Identifies the TUI session."),
    "AIMEE_ATTACH_ID": ("Client & session", "Presence attach id used when joining an existing session."),
    "AIMEE_HOOK_CLIENT": ("Client & session", "Identifies the calling hook client (e.g. claude/codex) for hook routing."),
    "AIMEE_NO_AUTOSTART": ("Client & session", "If set, the client does not auto-start a local aimee-server."),
    "AIMEE_MODEL": ("Client & session", "Override the primary model for the session."),
    "AIMEE_EFFORT": ("Client & session", "Reasoning-effort hint for the session/model."),
    "AIMEE_MODE": ("Client & session", "Operating-mode override (e.g. interactive / autonomous)."),
    "AIMEE_PROFILE": ("Client & session", "Active working-profile name."),
    "AIMEE_ACTIVE_TOOLSET": ("Client & session", "Active toolset (tool allowlist) for the session."),
    "AIMEE_SESSION_START_VERBOSE": ("Client & session", "Verbose logging during session start."),
    # Server runtime
    "AIMEE_SERVER_HTTP_BIND": ("Server runtime", "TCP bind address for the server `/v1` HTTP listener (else UDS-only)."),
    "AIMEE_SERVER_STARTUP_FD": ("Server runtime", "Inherited fd for startup-readiness signalling (service launch)."),
    "AIMEE_API_REMOTE_WRITES": ("Server runtime", "Gate remote (TCP) write methods: `off` | `data` | `full`."),
    "AIMEE_WEBCHAT_GIT": ("Server runtime", "Per-webuser webchat git surface — repo connect/clone, git ops (pull/commit/push/branch), per-host token + SSH-key credential intake, the workspace forge-token broker, project listing + session-dir resolution, and \"Sign in with GitHub\" (on by default; set to the literal value 0 to disable the entire surface — all of those routes then return 503, e.g. for a chat/editor-only deployment; any other value leaves it on). Independent of AIMEE_WEBCHAT_EDITOR."),
    "AIMEE_WEBCHAT_EDITOR": ("Server runtime", "Per-webuser in-browser code-server editor (on by default; set to 0 to disable; needs a code-server binary, shipped by WITH_VSCODE images)."),
    "AIMEE_WEBCHAT_EDITOR_BIN": ("Server runtime", "Override path to the code-server binary used for the in-browser editor."),
    "AIMEE_WEBCHAT_EDITOR_IDLE_SECS": ("Server runtime", "Idle timeout in seconds before a per-webuser code-server editor is reaped. Default 1800 (30 min); positive values are clamped to [60, 604800]; 0 disables idle reaping; malformed/negative/overflow values fall back to the default. An actively-open editor is kept alive by the proxy keepalive, so it is not reaped mid-session."),
    "AIMEE_WEBCHAT_EDITOR_UID": ("Server runtime", "Dedicated service user the per-webuser code-server drops to (defence in depth; only honoured when aimee-server runs as root)."),
    "AIMEE_GITHUB_OAUTH_CLIENT_ID": ("Server runtime", "Client ID of a GitHub OAuth App (device flow enabled) for the webchat \"Sign in with GitHub\" button; populates the github.com git credential. Public, no secret needed."),
    "AIMEE_INGRESS_PROXY_SECRET": ("Server runtime", "Shared secret authenticating a trusted ingress proxy's identity headers."),
    "AIMEE_PARALLEL_MAX": ("Server runtime", "Maximum parallel agent fan-out."),
    "AIMEE_BACKGROUND_THREADS": ("Server runtime", "Background worker thread count."),
    "AIMEE_COMPUTE_THREADS": ("Server runtime", "Compute-pool thread count."),
    "AIMEE_SESSION_THREADS": ("Server runtime", "Per-session worker thread count."),
    "AIMEE_WORKTREE_GC": ("Server runtime", "Enable/disable delegate-worktree garbage collection."),
    "AIMEE_WORKTREE_GC_DAYS": ("Server runtime", "Age threshold (days) for worktree GC."),
    "AIMEE_SOCK": ("Server runtime", "Sandbox helper socket path."),
    # Knowledge base
    "AIMEE_LLM_URL": ("Knowledge base (aimee-kb)", "One knob: base URL of the aimee-llm container the kb calls for embedding (/embed), reranking (/rerank) AND synthesis (curator Tier-A + Tier-B at {url}/v1). The kb runs no model itself. AIMEE_EMBEDDER_URL/AIMEE_RERANKER_URL override per service. See docs/KB_LLM_BACKENDS.md."),
    "AIMEE_LLM_MODEL": ("Knowledge base (aimee-kb)", "Model label sent to AIMEE_LLM_URL's chat endpoint (single-model gateways ignore it). Default 'aimee-synth'."),
    "AIMEE_EMBEDDER_URL": ("Knowledge base (aimee-kb)", "Embedder endpoint override (/embed, /embed_batch); takes precedence over AIMEE_LLM_URL for embedding."),
    "AIMEE_KB_API_URL": ("Knowledge base (aimee-kb)", "aimee-kb HTTP API base URL."),
    "AIMEE_KB_API_BEARER_TOKEN": ("Knowledge base (aimee-kb)", "Bearer token for the aimee-kb API."),
    "AIMEE_KB_API_CA_BUNDLE": ("Knowledge base (aimee-kb)", "CA bundle path for verifying the aimee-kb TLS certificate."),
    "AIMEE_KB_CACHE_TTL_S": ("Knowledge base (aimee-kb)", "KB client cache TTL (seconds)."),
    "AIMEE_KB_CONN": ("Knowledge base (aimee-kb)", "KB connection string (mTLS transport)."),
    "AIMEE_KB_HTTP_BIND": ("Knowledge base (aimee-kb)", "aimee-kb HTTP listener bind address."),
    "AIMEE_KB_MTLS_HOST": ("Knowledge base (aimee-kb)", "aimee-kb mTLS listener host."),
    "AIMEE_KB_MTLS_PORT": ("Knowledge base (aimee-kb)", "aimee-kb mTLS listener port."),
    "AIMEE_KB_EMIT_ENROLL": ("Knowledge base (aimee-kb)", "Emit a client enrollment token on KB start."),
    "AIMEE_KB_EMIT_SCOPE": ("Knowledge base (aimee-kb)", "Scope for the emitted enrollment token."),
    "AIMEE_KB_OIDC_ISSUER": ("Knowledge base (aimee-kb)", "OIDC issuer for KB API auth."),
    "AIMEE_KB_OIDC_AUDIENCE": ("Knowledge base (aimee-kb)", "OIDC audience for KB API auth."),
    "AIMEE_KB_OIDC_JWKS_FILE": ("Knowledge base (aimee-kb)", "OIDC JWKS file for KB API auth."),
    "AIMEE_KB_OIDC_SCOPE_CLAIM": ("Knowledge base (aimee-kb)", "OIDC claim carrying the scope."),
    "AIMEE_KB_OIDC_SCOPE_KIND": ("Knowledge base (aimee-kb)", "OIDC scope-kind interpretation."),
    "AIMEE_VECTOR_KB_BATCH_SIZE": ("Knowledge base (aimee-kb)", "Embedding batch size for KB vector ingest."),
    # Database & vectors
    "AIMEE_DB2_URL": ("Database & vectors", "Postgres (DB2) connection URL for the KB store."),
    "AIMEE_EMBEDDING_DIM": ("Database & vectors", "Embedding dimension (drives halfvec column sizing)."),
    "AIMEE_PGVEC_SLOW_QUERY_MS": ("Database & vectors", "Slow-query log threshold (ms) for the pgvector transport."),
    # Memory
    "AIMEE_MEMORY_CITATIONS_MODE": ("Memory", "Citation rendering mode for memory recall."),
    "AIMEE_MEMORY_CITATIONS_STRIP_UNVERIFIED": ("Memory", "Strip unverified citations from recall output."),
    "AIMEE_MEMORY_COGNIFY_ASYNC_ENABLED": ("Memory", "Enable the async cognify pipeline."),
    "AIMEE_MEMORY_COREF_MODE": ("Memory", "Coreference-resolution mode."),
    "AIMEE_MEMORY_MAINTENANCE_TRIGGER_INSERTS": ("Memory", "Inserts before a maintenance cycle triggers."),
    "AIMEE_MEMORY_MAINTENANCE_TRIGGER_SECS": ("Memory", "Seconds before a maintenance cycle triggers."),
    "AIMEE_MEMORY_PAGERANK_RELATIONS": ("Memory", "Relation types included in memory PageRank."),
    "AIMEE_MEMORY_RERANK_FORCE_OFF": ("Memory", "Force the cross-encoder reranker off."),
    "AIMEE_MEMORY_RERANK_MODE": ("Memory", "Reranker mode."),
    "AIMEE_MEMORY_WEIGHT_PROFILE": ("Memory", "Recall scoring weight profile."),
    "AIMEE_NO_CACHE": ("Memory", "Disable the memory-assembly cache."),
    "AIMEE_CONTEXT_NO_KB": ("Memory", "Skip KB lookups during context assembly."),
    # Delegates & backends
    "AIMEE_DELEGATE_DEPTH": ("Delegates & backends", "Current delegation depth (recursion guard)."),
    "AIMEE_PARENT_DELEGATION_ID": ("Delegates & backends", "Parent delegation id (threading)."),
    "AIMEE_DELEGATE_HEARTBEAT_MONITOR": ("Delegates & backends", "Enable the delegate heartbeat monitor."),
    "AIMEE_DELEGATE_SOURCE_AUTHORITY": ("Delegates & backends", "Enable source-authority gating for delegate edits."),
    "AIMEE_DELEGATE_SOURCE_PATHS": ("Delegates & backends", "Allowed source paths for delegate edits."),
    "AIMEE_DELEGATE_WORKTREE_ROOT": ("Delegates & backends", "Root directory for delegate worktrees."),
    "AIMEE_DOCKER_BIN": ("Delegates & backends", "Docker delegate-backend binary."),
    "AIMEE_DOCKER_WORKDIR": ("Delegates & backends", "Docker delegate-backend working directory."),
    "AIMEE_SSH_BIN": ("Delegates & backends", "SSH delegate-backend binary."),
    "AIMEE_OPENCODE_BIN": ("Delegates & backends", "opencode CLI frontend binary."),
    # Forge
    "AIMEE_FORGE_API_BASE": ("Forge (GitHub App / tokens)", "Forge API base URL."),
    "AIMEE_FORGE_APP_ID": ("Forge (GitHub App / tokens)", "GitHub App id for minting forge tokens."),
    "AIMEE_FORGE_APP_INSTALLATION_ID": ("Forge (GitHub App / tokens)", "GitHub App installation id."),
    "AIMEE_FORGE_APP_PRIVATE_KEY": ("Forge (GitHub App / tokens)", "GitHub App private key (PEM or path)."),
    "AIMEE_FORGE_SCOPE": ("Forge (GitHub App / tokens)", "Scope for the minted forge token."),
    "AIMEE_FORGE_TOKEN": ("Forge (GitHub App / tokens)", "Static forge access token (bypasses App auth)."),
    # Gateway
    "AIMEE_GATEWAY_NTFY_BASE_URL": ("Gateway (voice / webhooks / push)", "ntfy push base URL."),
    "AIMEE_GATEWAY_NTFY_TOKEN": ("Gateway (voice / webhooks / push)", "ntfy push token."),
    "AIMEE_GATEWAY_STT_PROVIDER": ("Gateway (voice / webhooks / push)", "Speech-to-text provider."),
    "AIMEE_GATEWAY_STT_MODEL": ("Gateway (voice / webhooks / push)", "Speech-to-text model."),
    "AIMEE_GATEWAY_TTS_PROVIDER": ("Gateway (voice / webhooks / push)", "Text-to-speech provider."),
    "AIMEE_GATEWAY_TTS_BASE_URL": ("Gateway (voice / webhooks / push)", "Text-to-speech base URL."),
    "AIMEE_GATEWAY_TTS_MODEL": ("Gateway (voice / webhooks / push)", "Text-to-speech model."),
    "AIMEE_GATEWAY_TTS_VOICE": ("Gateway (voice / webhooks / push)", "Text-to-speech voice."),
    "AIMEE_GATEWAY_WEBHOOK_PORT": ("Gateway (voice / webhooks / push)", "Inbound webhook listener port."),
    "AIMEE_GATEWAY_WEBHOOK_SECRET": ("Gateway (voice / webhooks / push)", "Inbound webhook HMAC secret."),
    "AIMEE_GATEWAY_WEBHOOK_INSECURE": ("Gateway (voice / webhooks / push)", "Allow the webhook listener without TLS (dev)."),
    "AIMEE_GATEWAY_WEBHOOK_DELIVER_ONLY": ("Gateway (voice / webhooks / push)", "Webhook deliver-only mode (no reply path)."),
    # Workflow engine
    "AIMEE_WORKFLOW_REPO": ("Workflow engine", "Local repository directory the workflow engine operates on."),
    "AIMEE_WORKFLOW_BASE": ("Workflow engine", "Base branch for the engine's freeze/diff."),
    # Git verify / MCP
    "AIMEE_VERIFY_PARALLEL": ("Git verify / MCP", "Run `aimee git verify` steps in parallel."),
    "AIMEE_VERIFY_STEP_TIMEOUT_MS": ("Git verify / MCP", "Per-step timeout (ms) for git verify."),
    "AIMEE_MCP_CWD": ("Git verify / MCP", "Working-directory hint for MCP git-root resolution."),
    "AIMEE_MCP_TOOL_PROFILE": ("Git verify / MCP", "MCP tools/list presentation profile: 'core'/'lean' (default — Tier-0 high-frequency tools only, with find_tools/describe_tool reaching the rest) or 'full' (present every tool upfront)."),
    # Models
    "AIMEE_MODEL_CAPABILITY_OVERRIDES": ("Models", "Override model capability flags (reasoning/tools/vision/…)."),
    # TLS & networking
    "AIMEE_TLS_INSECURE": ("TLS & networking", "Disable TLS certificate verification (development only)."),
    "AIMEE_NET_DEBUG": ("TLS & networking", "Verbose network debug logging."),
    # Plugins
    "AIMEE_ENABLE_PROJECT_PLUGINS": ("Plugins", "Allow loading project-local plugins."),
    # Diagnostics & misc
    "AIMEE_ANTIPATTERNS_BYPASS": ("Diagnostics & misc", "Bypass the guardrail antipattern checks."),
    "AIMEE_LOG_LEVEL": ("Diagnostics & misc", "Log level: `error` | `warn` | `info` | `debug`."),
}


def parse_env_vars():
    """Every AIMEE_* env var read outside src/tests/ (test-only vars excluded)."""
    found = set()
    for f in sorted(SRC.rglob("*")):
        if f.suffix not in (".c", ".h", ".inc") or "/tests/" in f.as_posix():
            continue
        for m in ENV_RE.finditer(f.read_text(encoding="utf-8", errors="ignore")):
            found.add(m.group(1))
    return found


def render_env(found):
    out = ["## Environment variables",
           "",
           f"The binaries read {len(found)} `AIMEE_*` environment variables (scanned "
           "from `getenv()` in `src/`, excluding tests). They override config-store "
           "values and are mostly for deployment/runtime wiring. Secrets/tokens should "
           "be supplied via the environment or the credential vault, never committed.",
           ""]
    by_group = {}
    undocumented = []
    for v in sorted(found):
        if v in ENV_DESC:
            g, d = ENV_DESC[v]
            by_group.setdefault(g, []).append((v, d))
        else:
            undocumented.append(v)
    for g in ENV_GROUP_ORDER:
        rows = by_group.get(g)
        if not rows:
            continue
        out.append(f"### {g}")
        out.append("")
        out.append("| Variable | Description |")
        out.append("|----------|-------------|")
        for v, d in rows:
            out.append(f"| `{v}` | {d} |")
        out.append("")
    # any group present in ENV_DESC but not in the order list (defensive)
    for g in sorted(set(by_group) - set(ENV_GROUP_ORDER)):
        out.append(f"### {g}")
        out.append("")
        out.append("| Variable | Description |")
        out.append("|----------|-------------|")
        for v, d in by_group[g]:
            out.append(f"| `{v}` | {d} |")
        out.append("")
    if undocumented:
        out.append("### Undocumented (add to `ENV_DESC` in gen-reference-docs.py)")
        out.append("")
        out.append("> These are read by the code but have no description yet — the "
                   "generator surfaces them so the reference can't silently fall behind.")
        out.append("")
        out.append(", ".join(f"`{v}`" for v in undocumented))
        out.append("")
    return "\n".join(out).rstrip() + "\n"


# ─── External / provider environment (non-AIMEE_ getenv, OS-internal filtered) ─
# Third-party + standard env vars aimee honors. Provider API-key var NAMES are
# resolved per-agent via the agent's `api_key_env` (so the defaults below can be
# overridden); ANTHROPIC/GEMINI/GOOGLE keys are read through that indirection
# rather than as getenv() literals, so they are added explicitly.

EXT_RE = re.compile(r'getenv\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*\)')

# standard OS / platform vars aimee reads but does not define as configuration
EXT_OS_IGNORE = {
    "HOME", "PATH", "PWD", "TEMP", "TMP", "TMPDIR", "LOCALAPPDATA", "APPDATA",
    "USER", "USERNAME", "USERPROFILE", "SHELL", "PYTHONPATH", "LANG",
    "XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_RUNTIME_DIR",
}
# provider keys resolved via per-agent api_key_env (not getenv literals) — added so
# the reference lists them even though the static scan can't see them
EXT_DYNAMIC = {"ANTHROPIC_API_KEY", "GEMINI_API_KEY", "GOOGLE_API_KEY"}

EXT_GROUP_ORDER = ["Provider credentials", "Provider endpoints", "Reasoning effort",
                   "Network / proxy", "Editor", "Codex / Claude integration"]

EXT_DESC = {
    "OPENAI_API_KEY": ("Provider credentials", "OpenAI API key (default for OpenAI-family agents)."),
    "ANTHROPIC_API_KEY": ("Provider credentials", "Anthropic API key (read via the agent's `api_key_env`)."),
    "GEMINI_API_KEY": ("Provider credentials", "Google Gemini API key (read via the agent's `api_key_env`)."),
    "GOOGLE_API_KEY": ("Provider credentials", "Google API key fallback for Gemini (via `api_key_env`)."),
    "GEMINI_API_KEY_AUTH_MECHANISM": ("Provider credentials", "Selects the Gemini key auth mechanism."),
    "MISTRAL_API_KEY": ("Provider credentials", "Mistral API key."),
    "MINIMAX_API_KEY": ("Provider credentials", "MiniMax API key."),
    "OPENROUTER_API_KEY": ("Provider credentials", "OpenRouter API key."),
    "OLLAMA_HOST": ("Provider endpoints", "Ollama server host/URL for local models."),
    "LLAMA_HOST": ("Provider endpoints", "llama.cpp server host/URL."),
    "OPENAI_REASONING_EFFORT": ("Reasoning effort", "Reasoning-effort default for OpenAI-family models."),
    "CODEX_REASONING_EFFORT": ("Reasoning effort", "Reasoning-effort passed through the Codex frontend."),
    "HTTPS_PROXY": ("Network / proxy", "HTTPS proxy for outbound provider/API calls."),
    "NO_PROXY": ("Network / proxy", "Hosts excluded from proxying."),
    "EDITOR": ("Editor", "Editor invoked for interactive edits."),
    "VISUAL": ("Editor", "Editor invoked for interactive edits (preferred over `EDITOR`)."),
    "CODEX_HOME": ("Codex / Claude integration", "Codex home directory (Codex-frontend integration)."),
    "CODEX_MODEL": ("Codex / Claude integration", "Model the Codex frontend requests."),
    "CODEX_SANDBOX": ("Codex / Claude integration", "Codex sandbox mode."),
    "CODEX_CWD": ("Codex / Claude integration", "Working directory reported by the Codex frontend."),
    "CODEX_THREAD_ID": ("Codex / Claude integration", "Codex conversation/thread id."),
    "CLAUDE_SESSION_ID": ("Codex / Claude integration", "Claude Code session id when aimee runs as its backend."),
}


def parse_external_env():
    found = set()
    for f in sorted(SRC.rglob("*")):
        if f.suffix not in (".c", ".h", ".inc") or "/tests/" in f.as_posix():
            continue
        for m in EXT_RE.finditer(f.read_text(encoding="utf-8", errors="ignore")):
            v = m.group(1)
            if v.startswith("AIMEE_") or v in EXT_OS_IGNORE:
                continue
            found.add(v)
    return found | EXT_DYNAMIC


def render_external_env(found):
    out = ["## External & provider environment",
           "",
           "Standard and third-party environment variables aimee honors (scanned "
           "non-`AIMEE_*` `getenv()` reads, plus provider keys resolved via "
           "`api_key_env`). Provider API keys are credentials — prefer the credential "
           "vault; the env var is the per-provider fallback and its name is "
           "overridable per agent via `api_key_env`. Standard OS variables (`HOME`, "
           "`PATH`, `TMPDIR`, `XDG_*`, …) are used for their usual purposes and are "
           "not aimee configuration.",
           ""]
    by_group, undocumented = {}, []
    for v in sorted(found):
        if v in EXT_DESC:
            g, d = EXT_DESC[v]
            by_group.setdefault(g, []).append((v, d))
        else:
            undocumented.append(v)
    for g in EXT_GROUP_ORDER + sorted(set(by_group) - set(EXT_GROUP_ORDER)):
        rows = by_group.get(g)
        if not rows:
            continue
        out.append(f"### {g}")
        out.append("")
        out.append("| Variable | Description |")
        out.append("|----------|-------------|")
        for v, d in rows:
            out.append(f"| `{v}` | {d} |")
        out.append("")
    if undocumented:
        out.append("### Undocumented (add to `EXT_DESC`/`EXT_OS_IGNORE` in gen-reference-docs.py)")
        out.append("")
        out.append(", ".join(f"`{v}`" for v in undocumented))
        out.append("")
    return "\n".join(out).rstrip() + "\n"


# ─── Workflow engine config (src/workflow/) ───────────────────────────────────

ART = {f"WFE_ART_{k.upper()}": k for k in
       ("none", "proposal", "plan", "branch", "frozen_diff", "pr", "verdict", "approval")}
BLOCK_ENTRY_RE = re.compile(
    r'\{\s*WFE_BLK_\w+\s*,\s*"([^"]+)"\s*,\s*(WFE_ART_\w+)\s*,\s*\d+\s*,\s*\{([^}]*)\}')


def parse_block_catalog():
    text = (SRC / "workflow" / "wfe_def.c").read_text(encoding="utf-8")
    body = text[text.index("CATALOG[] = {"):text.index("\n};", text.index("CATALOG[] = {"))]
    cat = []
    for m in BLOCK_ENTRY_RE.finditer(body):
        name, produces, accepts_raw = m.groups()
        accepts = [ART[a] for a in re.findall(r'WFE_ART_\w+', accepts_raw)
                   if ART.get(a) and ART[a] != "none"]
        cat.append((name, ART.get(produces, produces), accepts))
    return cat


def parse_engine_consts():
    dfn = (SRC / "workflow" / "wfe_def.h").read_text(encoding="utf-8")
    auto = (SRC / "workflow" / "wfe_autonomy.h").read_text(encoding="utf-8")
    att = re.search(r'#define\s+WFE_DEFAULT_MAX_ITERS\s+(\d+)', dfn)
    ovr = re.search(r'#define\s+WFE_MAX_OVERRIDES\s+(\d+)', auto)
    return (att.group(1) if att else "?"), (ovr.group(1) if ovr else "?")


def render_workflow(catalog, consts):
    max_att, max_ovr = consts
    out = ["## Workflow engine",
           "",
           "Workflows are block-composed YAML definitions under "
           "`$AIMEE_HOME/workflows/<name>.yaml`, authored with the `aimee workflow` "
           "CLI or the web visual composer and saved in canonical form. A run is a "
           "DB1 work item pinned to a definition version.",
           "",
           "### Workflow definition schema",
           "",
           "```yaml",
           "name: <id>                 # workflow name",
           "start: <node-id>           # entry node (default: first node)",
           "nodes:",
           "  - id: <node-id>          # unique within the workflow",
           "    block: <block-name>    # a built-in or custom block (see catalog)",
           "    in:                    # typed input bindings (map: slot -> producer.output)",
           "      <slot>: <node-id>.<output>",
           "    params: { ... }        # block-specific params (see below)",
           "    next: <node-id>        # unconditional successor",
           "    on_pass: <node-id>     # gate verdict pass edge",
           "    on_fail: <node-id>     # gate verdict fail edge (loop-back)",
           "```",
           "",
           "### Built-in block catalog",
           "",
           "| Block | Produces | Accepts inputs |",
           "|-------|----------|----------------|"]
    for name, produces, accepts in catalog:
        acc = ", ".join(f"`{a}`" for a in accepts) if accepts else "_(source: none)_"
        out.append(f"| `{name}` | `{produces}` | {acc} |")
    out += [
        "",
        "### Block parameters (`params:`)",
        "",
        "- **`gate.roundtable`** — `panel.required` (list of required reviewer "
        "personas), `panel.eligible` (list of additional eligible personas), "
        "`quorum` (int; effective quorum is `max(2, quorum)` and at least the "
        "required-panel size).",
        "- **`gate.human`** — `policy: preauthorized` (auto-approve in autonomous "
        "mode) and/or `optional: true` (skippable). Without these, an autonomous run "
        "parks at the gate for a human.",
        "- Other blocks take no params today; unknown params are ignored by the "
        "validator.",
        "",
        "### Custom blocks — `$AIMEE_HOME/workflows/blocks.yaml`",
        "",
        "Operator-owned (refused if a symlink or group/world-writable). Adds blocks "
        "to the catalog above:",
        "",
        "```yaml",
        "allow_command: false       # opt-in gate for the `command` executor (no-shell, argv-only)",
        "blocks:",
        "  - name: <block-name>     # must not shadow a built-in or duplicate",
        "    consumes: <artifact>   # input artifact type, or none (a source)",
        "    produces: branch|none  # custom blocks may NOT mint verdict/approval/pr",
        "    executor: command|delegate",
        "    command: [ argv0, arg1, ... ]   # executor: command (run in the repo, no shell)",
        "    persona: <name>        # executor: delegate",
        "    prompt: <text>         # executor: delegate",
        "```",
        "",
        "### Run-level controls (not in the definition)",
        "",
        f"- **Per-stage loop cap** — a node that loops back via `on_fail` is retried at "
        f"most `max_iters` times (per-node param, default `{max_att}`); on the cap its "
        f"`on_max` policy resolves the loop: `human` parks (default), `fail` is a "
        f"terminal reject, `pass` forces the flow forward via `on_pass`/`next`.",
        f"- **Gate-override cap** — a parked human gate may be overridden at most "
        f"`{max_ovr}` times (`WFE_MAX_OVERRIDES`) before the run is forced terminal.",
        "- **Cost cap** — an optional per-work-item USD ceiling set at run creation "
        "(`work_item_max_cost_usd`); the engine parks the run when cumulative cost "
        "reaches it.",
        "- **Trigger / autonomy mode** — `interactive` vs `autonomous`, set when the "
        "run is created.",
        "",
        "### Workflow environment overrides",
        "",
        "`AIMEE_WORKFLOW_REPO` (repo the engine operates on) and "
        "`AIMEE_WORKFLOW_BASE` (base branch for freeze/diff) — see Environment "
        "variables above.",
    ]
    return "\n".join(out).rstrip() + "\n"


# ─── Separate config files (agents.json, toolsets) ────────────────────────────

AGENT_FIELD_DESC = {
    "agents": "Top-level: array of agent definitions.",
    "default_agent": "Top-level: name of the default agent.",
    "name": "Agent identifier.",
    "desc": "Human description of the agent.",
    "enabled": "Whether the agent is active.",
    "provider": "Provider name.",
    "model": "Model name.",
    "endpoint": "Provider endpoint URL.",
    "backend": "Execution backend (http / cli / ssh / docker).",
    "api_key": "Inline API key (prefer `api_key_env` or the vault).",
    "api_key_env": "Env var name holding the agent's API key.",
    "access_token": "Static auth token for the endpoint.",
    "auth_cmd": "Command that prints an auth token.",
    "auth_type": "Auth scheme (bearer / oauth / none).",
    "credentials": "Credential block / reference.",
    "tokens": "Token budget / accounting block.",
    "context_window": "Model context window (tokens).",
    "max_tokens": "Max output tokens.",
    "max_turns": "Max agent-loop turns.",
    "max_parallel": "Max concurrent calls to this agent.",
    "timeout_ms": "Per-call timeout (ms).",
    "cost_limit": "Per-agent cost cap (USD).",
    "cost_tier": "Cost-tier label for routing.",
    "auto_compact_pct": "Context % at which to auto-compact.",
    "context_warn_pct": "Context % at which to warn.",
    "stall_threshold": "Stall-detection threshold.",
    "roles": "Roles this agent serves (review, plan, …); `\"all\"` = every role.",
    "personas": "Personas this agent may be dispatched AS (engineer, architect, …); `\"all\"` or omitted = every persona.",
    "exec_roles": "Roles this agent may execute with tools.",
    "exec_system_prompt": "System prompt for exec/tool runs.",
    "tools_enabled": "Allow tool use for this agent.",
    "inject_respond_tool": "Inject the `respond` tool.",
    "middleware": "Per-agent middleware overrides (e.g. `context_window`, `max_tokens`).",
    "recommended_sampling": "Provider-recommended sampling parameters.",
    "extra_headers": "Extra HTTP headers for requests.",
    "fallback_model": "Fallback model on failure.",
    "fallback_chain": "Ordered fallback agent chain.",
    "session_reuse": "Reuse a session across calls.",
    "cli_cmd": "CLI command for a cli-backend agent.",
    "cli_kind": "CLI agent kind (claude / codex / opencode).",
    "cli_idle_timeout_ms": "Idle timeout (ms) for a CLI agent.",
    "ssh_entry": "SSH entry point (ssh backend).",
    "ssh_key": "SSH key path (ssh backend).",
    "user": "Remote user (ssh backend).",
    "target_host": "Target host (relay / tunnel).",
    "target_port": "Target port (relay / tunnel).",
    "host": "Target host.",
    "port": "Target port (relay / tunnel).",
    "ip": "Bind/target IP (relay / tunnel).",
    "cidr": "Allowed CIDR (relay / tunnel networking).",
    "hosts": "Allowed hosts (relay / tunnel).",
    "networks": "Allowed networks.",
    "network": "Network mode (backend sandbox).",
    "relay_key": "Relay auth key.",
    "relay_ssh": "SSH relay config.",
    "tunnel": "Tunnel config.",
    "tunnels": "Tunnel definitions.",
    "reconnect_delay": "Delay between reconnects (ms).",
    "max_reconnects": "Max reconnect attempts (streaming / relay).",
}

AGENT_FIELD_RE = re.compile(r'cJSON_GetObjectItem(?:CaseSensitive)?\(\s*\w+\s*,\s*"([a-z_]+)"\s*\)')


def parse_agent_fields():
    f = SRC / "server" / "agent_config.c"
    if not f.exists():
        return set()
    return set(AGENT_FIELD_RE.findall(f.read_text(encoding="utf-8")))


def render_config_files(agent_fields):
    out = ["## Other configuration files",
           "",
           "Beyond the config store, aimee reads a few standalone JSON/policy files "
           "(paths under `$AIMEE_HOME` unless an env override is set).",
           "",
           "### `agents.json` — agent / model definitions",
           "",
           "`{\"default_agent\": \"<name>\", \"agents\": [ {<agent>}, … ]}`. Each agent "
           "object's fields (scanned from `src/server/agent_config.c`):",
           "",
           "| Field | Description |",
           "|-------|-------------|"]
    undescribed = []
    for k in sorted(agent_fields):
        d = AGENT_FIELD_DESC.get(k)
        if d:
            out.append(f"| `{k}` | {d} |")
        else:
            undescribed.append(k)
    out.append("")
    if undescribed:
        out.append("> **Undocumented agent fields** (add to `AGENT_FIELD_DESC`): "
                   + ", ".join(f"`{k}`" for k in undescribed))
        out.append("")
    out += [
        "### Toolsets — `AIMEE_TOOLSETS_CONFIG` (or the config `toolsets` map)",
        "",
        "Named tool allowlists. `{\"toolsets\": {\"<name>\": { … }}}`; each toolset:",
        "",
        "- `tools` / `allowed_tools` — the tool names the set permits.",
        "- `include` — inherit another toolset's tools.",
        "- `script` — script-tool configuration for the set.",
        "",
        "### Guardrails — `AIMEE_GUARDRAILS_PATH`",
        "",
        "A policy file governing path read/write classification and pre-tool "
        "enforcement (antipattern blocking). It is a behavioral policy rather than a "
        "flat key schema; the tunable thresholds are exposed as the `guardrails` "
        "section + `guardrails_semantic_*` / `guardrail_mode` keys documented above.",
    ]
    return "\n".join(out).rstrip() + "\n"


def render_limitations():
    return "\n".join([
        "## Coverage & limitations",
        "",
        "This reference is generated by scanning the canonical source tables, which "
        "covers the scalar/keyed config surface but has known blind spots — listed "
        "here so a reader can tell *deliberately out of scope* from *not auto-derived*:",
        "",
        "- **Array/object element fields** are captured when the parser iterates with "
        "`cJSON_ArrayForEach` over a section's array; fields read through other access "
        "patterns (`cJSON_GetArrayItem`, indexing) or nested more than one object deep "
        "may appear only under their parent section name.",
        "- **Env vars built at runtime** (a name assembled with `snprintf`/concatenation "
        "and passed to `getenv(var)`) are not discoverable by the string-literal scan. "
        "Provider API-key vars are the known case and are handled via each agent's "
        "`api_key_env`; only the common defaults are listed.",
        "- **Compile-time `-D` defines** used as build-level configuration are not "
        "scanned (they are not runtime-overridable config).",
        "- **Separate config files** — `agents.json`, toolsets, guardrails, and "
        "custom workflow blocks (`blocks.yaml`) / workflow definitions are documented "
        "in their own sections above. Per-agent field set is scanned from "
        "`agent_config.c`; the guardrails *policy* is behavioral (path classification "
        "+ pre-tool enforcement), with its tunables exposed as config keys.",
        "",
        "If the scan ever finds a config var with no description, it is emitted under "
        "an **Undocumented** heading in the relevant section — so a new option cannot "
        "silently bypass this reference.",
    ]).rstrip() + "\n"


def main():
    check = "--check" in sys.argv
    GEN.mkdir(parents=True, exist_ok=True)
    cli = render_cli(parse_cli_commands())
    fields = parse_config_fields()
    sections, flat = parse_config_sections()
    # a key that is a CLI-settable scalar (or a section name) is not also a stray
    # "other top-level" key — subtract both so nothing is double-listed.
    flat = flat - {k for k, _ in fields} - set(sections)
    cfg = render_config(fields, sections, flat)
    cfg = (cfg.rstrip() + "\n\n"
           + render_env(parse_env_vars()).rstrip() + "\n\n"
           + render_external_env(parse_external_env()).rstrip() + "\n\n"
           + render_workflow(parse_block_catalog(), parse_engine_consts()).rstrip() + "\n\n"
           + render_config_files(parse_agent_fields()).rstrip() + "\n\n"
           + render_limitations())
    targets = {GEN / "cli-commands.md": cli, GEN / "configuration.md": cfg}

    if check:
        stale = [p.name for p, want in targets.items()
                 if not p.exists() or p.read_text(encoding="utf-8") != want]
        if stale:
            print(f"gen-reference-docs: STALE — run scripts/gen-reference-docs.py: {stale}")
            return 1
        print("gen-reference-docs: ok (cli-commands.md, configuration.md in sync)")
        return 0

    for p, want in targets.items():
        p.write_text(want, encoding="utf-8")
        print(f"gen-reference-docs: wrote {p.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
