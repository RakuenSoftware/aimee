#!/usr/bin/env python3
"""Generate CLI + configuration reference docs from the canonical source tables.

Two committed outputs (regenerate with `make -C src docs-gen`):
  docs/gen/cli-commands.md  : every `aimee` CLI command + subcommands, from the
                               command catalogue the server serves
                               (src/server/cli_command_defs_data.h).
  docs/gen/configuration.md : every config key from the metadata shipped by the
                               external pure-Go config module.

The point is completeness: these are derived from the same tables the binary
uses, so they cannot silently drift from the implementation the way hand-written
lists do.
"""
import re
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
GEN = ROOT / "docs" / "gen"

# ─── CLI commands (src/server/cli_command_defs_data.h) ───────────────────────
# Each entry: {"name", "description", CLIENT_TIER_X, hidden_flag, subcmd_or_NULL}
# where subcmd is a (possibly multi-line, concatenated) C string of lines like
#   "  sub   description\n"

TIER_LABEL = {"CORE": "Core", "ADVANCED": "Advanced", "ADMIN": "Admin"}


def _c_strings(blob):
    """Concatenate adjacent C string literals, unescaping \\n and \\t."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', blob)
    s = "".join(parts)
    return s.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"')


_CLI_ROWS_TEXT = None  # plant-test override; None means read the real file


def parse_cli_commands():
    text = (_CLI_ROWS_TEXT if _CLI_ROWS_TEXT is not None
            else (SRC / "server" / "cli_command_defs_data.h").read_text(encoding="utf-8"))
    # Each entry begins with {"<name>", and ends at the matching `},` at the
    # entry's top level. Split on the entry-start sentinel instead of brace
    # counting (the subcmd strings contain no braces).
    entries = []
    # normalize: drop the file's leading comment
    body = text[text.index('{"'):]
    # Strip block comments BETWEEN entries. A chunk is matched from its start,
    # so a comment in front of an entry made the match fail -- and the failure
    # was a `continue`, so the entry vanished from the docs and the "Total
    # commands" line counted the survivors and looked right. Adding an ordinary
    # explanatory comment above a row silently deleted that command's reference
    # page.
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    # split into entries on `},\n` boundaries that precede a new `{"`
    raw = re.split(r'\},\s*(?=\{")', body)
    skipped = []
    for chunk in raw:
        m = re.match(r'\s*\{\s*"([^"]+)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*AIMEE_CMD_TIER_(\w+)\s*,\s*(\d+)\s*,\s*(.*)$',
                     chunk, re.S)
        if not m:
            # The table's NULL sentinel is the one chunk that legitimately does
            # not parse. Anything else is a row this generator could not read,
            # and dropping it silently is how a command loses its documentation
            # without anyone being told.
            if chunk.strip() and not re.match(r'\s*\{\s*NULL', chunk):
                skipped.append(chunk.strip()[:120])
            continue
        name, desc, tier, hidden, rest = m.groups()
        subs = None if rest.strip().startswith("NULL") else _c_strings(rest).strip("\n")
        entries.append({"name": name, "desc": desc, "tier": tier,
                        "hidden": hidden == "1", "subs": subs})
    if skipped:
        raise SystemExit(
            "gen-reference-docs: could not parse "
            f"{len(skipped)} command row(s); they would have been dropped from "
            "the reference silently:\n  "
            + "\n  ".join(skipped))
    return entries


def render_cli(entries):
    out = ["# CLI Command Reference",
           "",
           "> Auto-generated from `src/server/cli_command_defs_data.h` by `scripts/gen-reference-docs.py`.",
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


# ─── Config: metadata owned by the external pure-Go module ────────────────

CFG_TYPE = {"CFG_STRING": "string", "CFG_BOOL": "bool", "CFG_INT": "int", "CFG_FLOAT": "float",
            "CFG_ECON_TIER": "string (off\\|safe\\|aggressive)"}

# Curated one-line descriptions for the CLI-settable keys (the `aimee config set`
# surface). A key in the generated table with no entry here renders "n/a" and is
# counted as undescribed so the gap is visible (see render_config).
CFG_KEY_DESC = {
    "kb_pdf_tier": "Structured-PDF pipeline preset: off (plain pdftotext, default) | basic (ingest+vector) | full (all stages).",
    "kb_curator_tier": "KB curator pipeline preset: off | lite (core extract+index) | full (all stages, default).",

    "audit_action_enabled": "Publish governed tool-action audit rows (default on); disabling it creates an audit coverage gap.",
    "code_trust_actuation_enabled": "Use earned code-graph trust lessons only as an equal-score retrieval tiebreak (default off).",
    "guardrails_semantic_mode": "Semantic guardrail mode: off, dry_run, advisory, or enforce.",
    "kb_client_bearer_token": "Vault-backed server-to-KB bearer; reads are redacted; restart required.",
    "kb_client_url": "Remote aimee-kb API base URL used by aimee-server; restart required.",
    "kb_curator_extract_code_workers": "Parallel curator code-extraction workers, bounded to the synthesis service slot count.",
    "kb_curator_extract_docs_workers": "Parallel curator document-extraction workers, bounded to the synthesis service slot count.",
    "kb_evidence_embed_enabled": "Drain evidence-index operations into evidence vectors.",
    "kb_mode": "Setup/deploy mode: local starts a KB, remote connects to kb_client_url.",
    "verify_cmd": "Command run after a delegated fix to verify it.",
    "verify_prompt": "Prompt template given to the cross-verification delegate.",
    "verify_role": "Delegate role used for cross-verification.",
    "wfe_proposals_autoscan_enabled": "Automatically scan watched proposal directories; off requires explicit trigger.fire.",

    "aimee_with_llamacpp": "Whether THIS IMAGE bundles llama.cpp (\"1\" on the "
    "aimee-kb-*-llm variants). Set by the Dockerfile, not by an operator: it is a fact "
    "about the running image, and the setup wizard reads it to decide whether the local "
    "synthesis models can be offered at all.",
    "synthesis_endpoint": "The ONE synthesis endpoint, remote or loopback. Empty means "
    "synthesis is off, which is supported - embedding, search, recall and indexing "
    "never call it. On a *-llm image the container entrypoint sets this to loopback "
    "itself after starting the bundled model.",
    "synthesis_model": "Synthesis model. On a *-llm image this selects the bundled model "
    "to fetch and serve (gemma-4-E2B-it or gemma-4-E4B-it); otherwise it is the model "
    "label sent to the configured endpoint.",
    "synthesis_api_key": "Bearer token for the synthesis endpoint (blank for a keyless "
    "or loopback endpoint).",
    "synthesis_thinking": "Let the synthesis model think before answering (default on). "
    "It measured positive-to-neutral everywhere it was tried. Global rather than "
    "per-stage, and the operator's call: turn it off only for a model that reasons past "
    "its output budget without answering.",

    "kb_curator_cross_repo_graph_enabled": "Resolve and maintain cross-repository dependency edges.",
    "kb_curator_custom_stages": "JSON definitions that recompose vetted curator operations with bounded budgets.",
    "kb_curator_detect_contradictions_enabled": "Link claims that disagree on the same subject and attribute.",
    "kb_curator_extract_code_enabled": "Extract typed curator artifacts from indexed code.",
    "kb_curator_extract_docs_enabled": "Extract typed curator artifacts from documents.",
    "kb_curator_index_claims_enabled": "Embed claim subject/attribute/value records.",
    "kb_curator_index_code_unit_enabled": "Embed extracted code-unit artifacts.",
    "kb_curator_index_narrative_enabled": "Embed summaries and synthesis narratives.",
    "kb_curator_link_artifacts_enabled": "Link related document, entity, claim, and code artifacts.",
    "kb_curator_projection_graph_enabled": "Publish the typed code projection graph for changed projects.",
    "kb_curator_promote_entity_enabled": "Promote well-supported entities one step up the scope lattice.",
    "kb_curator_resolve_entities_enabled": "Resolve proposed mentions against canonical entities.",
    "kb_curator_stage_order": "Comma-separated curator stage order; invalid dependency order falls back safely.",
    "kb_curator_synthesize_enabled": "Create evidence-backed topic synthesis with the configured reasoning tier.",
    "kb_curator_user_presets": "JSON array of operator-defined curator stage presets.",

    "autonomous": "Legacy mode flag. The current Go WFE records run mode at admission but does not branch scheduler behavior on it; human gates always park.",
    "economizer": "Context economizer tier: `off` (verbatim passthrough), `safe` (default; Anthropic prompt caching + lossless, freeze-guarded reduction), or `aggressive` (adds lossy compression + live OpenAI-side mutation; Anthropic context is never mutated). See docs/features/economizer.md.",
    "cache_aware_rewrite_enabled": "Rewrite prompts to align with the provider's prompt cache.",
    "cache_min_chars": "Minimum prompt size (chars) before cache-shaping applies.",
    "cache_shaping_enabled": "Enable prompt cache-shaping.",
    "extended_thinking_enabled": (
        "Ask for extended thinking on aimee's OWN Anthropic requests (default off). "
        "Sends the adaptive thinking config, and only to a model whose capabilities "
        "report that it accepts it -- a model nobody has reported that for is left "
        "alone rather than sent a shape the provider would reject. Off by default "
        "because thinking tokens are billed: enabling it changes spend, not just "
        "visibility."
    ),
    "delegate_graph_context_enabled": "Prepend a structural code-graph context block (callers/dependencies of files a delegate task references) to the delegate prompt (advisory, fail-open, default off).",
    "memory_md_retire": "Retire the agent file-memory surface into aimee (default on): a Write under ~/.claude/projects/<slug>/memory/<name>.md is intercepted into aimee's db1 and the .md is never materialized; session-start skips .md hydration. Set false for the legacy re-materialized .md mirrors.",
    "claude_model": "Default Claude model (empty = CLI default).",
    "gateway_prevent_subagents": "Gateway strips subagent-spawning tools (Task/Agent/etc.) from proxied requests so the served model cannot spawn subagents. Default off.",
    "gateway_pin_model": "Gateway forces the proxied /v1/messages served model to the configured primary's model, overriding the client-requested model. Default off (the passthrough honors the client model); enable for single-model Anthropic-compatible shims.",
    "cost_reward_enabled": "Factor token cost into the reward signal.",
    "cost_reward_lambda_pct": "Cost-penalty weight (percent) in the reward.",
    "cost_reward_ref_usd_milli": "Reference cost (USD-milli) normalizing the cost reward.",
    "client_integrations_enabled": "Auto-register aimee (MCP server, hooks, slash commands) into detected AI-tool user configs: Claude Code (~/.claude), Gemini, Copilot, Codex. Default-ON; set false, or export AIMEE_NO_CLIENT_INTEGRATIONS, to keep aimee out of every tool's global config and wire a single project by hand.",
    "cross_verify": "Enable cross-model verification of outputs.",
    "wfe_live_forge_enabled": "Gate for the autonomous live forge (default-ON). When off, the forge provider is not registered and every forge op fails closed, so an autonomous run can never open or merge a real PR. Even on, each op re-checks this flag and the merge-target rail.",
    "css_style_graph_enabled": "Enable the CSS migration assistant's style-graph write path during indexing.",
    "code_cochange_git_enabled": "Mine git history at `index scan` time into co_edited edges (files that change together in a commit), which blast radius already reads. Incremental and idempotent via a per-project HEAD marker; bulk commits (>25 code files) are skipped. Default on.",
    "css_render_command": "Render backend for the #4-full computed-style oracle: a command reading {html,css} JSON on stdin and writing a computed-style snapshot JSON on stdout (run an isolated headless-browser sidecar).",
    "db2_url": "Vault-backed DB2 connection URL; reads are redacted and writes bypass YAML.",
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
    "embedder_command": "Command that produces embeddings (overrides the endpoint).",
    "embedder_dims": "Embedding vector width. Leave unset for a bundled embedder - it "
    "declares its own width and the kb derives it (pinned > recorded > probed). REQUIRED "
    "for an external endpoint, whose width cannot be derived; valid to 4000, the DB2 "
    "column ceiling. A ONE-WAY DOOR once anything is embedded: DB2 records the width and "
    "refuses to start on drift.",
    "embedder_url": "External embedder endpoint. A non-empty value IS the external "
    "embedder; empty means the model baked into this image variant (bekko-a25m at 384, "
    "or nomic-v2 at 768 on the -nomic images).",
    "embedder_model": "Embedder identity. Written for a bundled model too, not just an "
    "external one: it is the registry key pooling and prefixes resolve from, and the "
    "value recorded against the corpus.",
    "embedder_api_key": "Bearer token for an external embedder endpoint (blank if it "
    "needs none).",
    "fidelity_check_enabled": "Run the answer-fidelity judge on terminal-text turns "
    "(default off; requires kb_evidence_emit_enabled + ingress_preinject_enabled).",
    "guardrail_mode": "Guardrail enforcement mode: approve (default; a tool call needs approval, so an unattended delegate is blocked), prompt, or deny.",
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
    "prompt + history; the context-economizer (aggressive tier) compresses older results to keep "
    "history bounded.",
    "require_session_worktree": "Fail closed on mutating ops outside an aimee-managed worktree "
    "(session-isolation guard; default off).",
    "subagent_ban_enabled": "Prevent provider-native sub-agent tools when an aimee delegate is "
    "available, and install the matching client guardrails (default on).",
    "require_aimee_memory": "Block agent writes to external file-based agent-memory stores "
    "(~/.claude/projects/<slug>/memory/...) and redirect durable memories into aimee's memory "
    "system via `aimee memory store` (default on).",
    "require_aimee_git": "Block a delegate from running `git` or `gh` in a shell (reads "
    "included) and redirect git/forge work to aimee's `git_*` tools, which execute on "
    "aimee-server where the forge credential stays in-process; delegates are also spawned "
    "without git/gh credentials. Note the env strip also drops SSH_AUTH_SOCK (no agent-backed "
    "SSH to any host) and neuters the global/system git config (default on).",
    "delegate_sandbox_package_access": "Runtime package access for a `--network none` delegate. "
    "The delegate has no outside socket: the Go egress module proxies only its immutable package "
    "registry allowlist, pins the validated numeric destination, and logs the transfer. `proxy` "
    "(default) enables that narrow path; `off` permits only build-time/pre-baked packages. Legacy "
    "`gated` and `governance` values cannot widen the live proxy allowlist.",
    "delegate_sandbox_require_isolation": "Deprecated compatibility key. Its value is ignored: "
    "every delegate is created with no network, its complete network/mount/environment posture is "
    "verified after every start or resume, and any breach or unverifiable fact destroys the "
    "container and refuses the delegation. There is no host fallback.",
    "delegate_sandbox_learn_packages": "Learned toolchain for delegate sandboxes (default on). "
    "aimee captures the apt packages a delegate installs inside its `--network none` sandbox, "
    "records them per project (git root), and pre-bakes the learned set into that project's next "
    "sandbox image build. It augments a declared `.aimee/project.yaml` from+packages spec, or "
    "synthesizing one (FROM the resolved base + the learned packages) when none is declared. "
    "Best-effort: a learned build that fails falls back to the un-augmented image. The first "
    "delegate turn after a new package is learned pays a one-time image build.",
    "ingress_preinject_assembly_budget": "Token budget for ingress context pre-injection.",
    "ingress_preinject_enabled": "Enable `<aimee-context>` pre-injection on ingress "
    "(memory/code preview envelope on primary ingress turns; default on).",
    "code_context_mode": "Task-conditioned code packet rollout mode: `off` disables packet "
    "retrieval, `observe` retrieves and validates without changing model-visible bytes, and `on` "
    "injects a bounded current-project packet on first/new-task turns (default `on`).",
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
    "ingress_trusted_proxy_secret": "Vault-backed shared secret for a trusted ingress proxy; reads are redacted.",
    "ingress_usage_accounting_enabled": "Account token usage on ingress requests.",
    "integrity_dry_run": "Run integrity checks without enforcing.",
    "integrity_enabled": "Enable the integrity gate.",
    "kb_api_bearer_token": "Vault-backed bearer for the aimee-kb API; reads expose configured state only.",
    "kb_api_http_port": "HTTP port the aimee-kb API listens on.",
    "kb_evidence_emit_enabled": "Emit evidence records from KB ingest.",
    "kb_fusion_mode": "KB retrieval fusion mode: rrf (default), static_alpha, or dynamic_alpha.",
    "session_worktree_base": "What a new primary session's branch+worktree is cut from. Order: configured -> "
                              "remote default -> main -> master. Values: remote_default (default), "
                              "local_default, current (opt-in only, never a fallback), or an explicit "
                              "ref. Env: AIMEE_SESSION_WORKTREE_BASE.",
    "pr_base_mode": "What a PR opened from a session targets. feature (default): the session's "
                    "durable feature branch aimee/feat/<slug>, so a feature's slices accumulate on "
                    "one branch and reach the default branch through a single reviewed PR. "
                    "default_branch: every PR aims at the repo's default branch. Any other value "
                    "fails closed rather than guessing a base.",
    "feature_auto_promote": "When every PR on a feature branch has merged, automatically open the "
                            "feature -> default-branch PR as a draft (default on). Off leaves "
                            "promotion to an explicit action.",
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
    "(resolves like embedder_command; AIMEE_OCR_URL env fallback).",
    "kb_mining_enabled": "Enable background KB mining.",
    "kb_mining_min_poll_s": "Minimum interval (s) between KB mining polls.",
    "kb_search_max_results": "Default max results for KB search.",
    "learning_implicit_citation_continuation": "Implicit-learning signal: citation on continuation.",
    "learning_implicit_citation_repair": "Implicit-learning signal: citation on repair.",
    "learning_implicit_repeat_question": "Implicit-learning signal: repeated question.",
    "learning_implicit_repeated_correction": "Implicit-learning signal: repeated correction.",
    "learning_implicit_retrieval_outcome": "Bridge continuation/repair autolabels into retrieval outcomes (memory + ranker).",
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
    "memory_rerank_mode": "Reranker mode.",
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
    "default_persona": "Persona a fresh primary session starts as, and the persona draft roundtable panelists author with when none is set (default 'engineer').",
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
    "economizer": "Context economizer tier (a single string: `off` | `safe` | `aggressive`). off = verbatim passthrough; safe (default) = Anthropic prompt caching + lossless, freeze-guarded reduction; aggressive = adds lossy tool-body compression + live OpenAI-side gateway mutation. Anthropic context is never mutated at any tier. The `{enabled, aggressive}` object form is deprecated. See docs/features/economizer.md.",
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
    "memory": "Memory subsystem; most children (recall, lifecycle, …) are nested objects with their own keys.",
    "memory_maintenance": "Memory maintenance scheduling.",
    "memory_negation": "Negation handling in memory.",
    "memory_query_expansion": "Recall query expansion.",
    "memory_recall_lanes": "Per-lane recall floors / caps.",
    "memory_rewrite": "Recall query rewriting.",
    "memory_window": "Memory-window neighbour expansion.",
    "model_meta": "Model metadata + capability routing.",
    "otel": "OpenTelemetry export.",
    "reasoning_cap": "Reasoning-effort cap.",
    "retry": "Provider retry / backoff.",
    "rewind": "Auto-snapshot / rewind.",
    "roundtable": "Roundtable pipeline thresholds, caps, gates, and turns.",
    "routing": "Capability-gated delegate routing: whether to gate seat choice on "
               "capability before cost, and whether to prefer free local seats.",
    "sandbox": "Tool sandbox (paths, network, mode).",
    "script": "Script-tool allowlist.",
    "search": "Web-search backend (Tavily / SearXNG).",
    "session": "Session / worktree limits.",
    "skills": "Skill subsystem (capability, dispatch, eval, review; nested objects).",
    "transport": "Transport tweaks (cache-aware rewrite).",
    "trigger": "Trigger listener (auth, concurrency).",
    "trigger_rules": "Trigger rule definitions (array of objects).",
    "workspaces": "Workspace definitions (array of objects).",
}


_CONFIG_METADATA = None


def _config_metadata():
    """Load documentation metadata from the pinned config module dependency."""
    global _CONFIG_METADATA
    if _CONFIG_METADATA is not None:
        return _CONFIG_METADATA
    override = os.environ.get("AIMEE_CONFIG_MODULE_DIR")
    if override:
        metadata_path = (Path(override) / "server-go" / "modules" /
                         "config" / "metadata.json")
    else:
        # Listing a module with `go list -m` may legitimately return an empty
        # Dir until its zip has already been downloaded.  Resolve the package
        # instead: package loading downloads the pinned dependency and returns
        # the exact directory containing the metadata we consume.
        result = subprocess.run(
            ["go", "list", "-mod=mod", "-f", "{{.Dir}}",
             "github.com/RakuenSoftware/aimee-module-config/server-go/modules/config"],
            cwd=ROOT / "server-go", check=True, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        resolved = result.stdout.strip()
        if not resolved:
            raise SystemExit(
                "gen-reference-docs: pinned config module was not downloaded "
                "and go list returned no module directory"
            )
        metadata_path = Path(resolved) / "metadata.json"
    try:
        _CONFIG_METADATA = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"gen-reference-docs: cannot read config module metadata: {exc}")
    if _CONFIG_METADATA.get("version") != 1:
        raise SystemExit("gen-reference-docs: unsupported config module metadata version")
    return _CONFIG_METADATA


def parse_config_fields():
    return [(field["key"], field["type"], field["group"])
            for field in _config_metadata()["fields"]]


# ─── Config: config-file (JSON) sections (src/config*.c) ──────────────────────
# Pattern: `<var> = cJSON_GetObjectItemCaseSensitive(root, "<section>")` then
# `cJSON_GetObjectItemCaseSensitive(<var>, "<key>")` for the section's keys.

ASSIGN_RE = re.compile(
    r'(\w+)\s*=\s*cJSON_GetObjectItemCaseSensitive\(\s*root\s*,\s*"([^"]+)"\s*\)')
CHILD_RE = re.compile(
    r'cJSON_GetObjectItemCaseSensitive\(\s*(\w+)\s*,\s*"([^"]+)"\s*\)')
# `cJSON_ArrayForEach(<item>, <arr>)`: element fields of an array-valued section
# are read off <item>; map <item> to the array's section so they're captured too.
FOREACH_RE = re.compile(r'cJSON_ArrayForEach\(\s*(\w+)\s*,\s*(\w+)\s*\)')


def parse_config_sections():
    metadata = _config_metadata()
    return ({name: set(keys) for name, keys in metadata["sections"].items()},
            set(metadata["flat"]))


def render_config(fields, sections, flat):
    out = ["# Configuration Reference",
           "",
           "> Auto-generated from the canonical source tables by "
           "`scripts/gen-reference-docs.py`: config keys from the pinned pure-Go config "
           "module, env vars scanned from `getenv()` in `src/`, and the "
           "workflow catalog from `server-go/internal/wfe/catalog.go`. Do not edit by hand; run "
           "`make -C src docs-gen` to regenerate.",
           "",
           "This reference covers every configurable surface:",
           "",
           "1. **Config-store keys**: the `aimee config` keys + config-file sections (below).",
           "2. **Environment variables**: `AIMEE_*` runtime/deployment overrides.",
           "3. **External & provider environment**: provider keys, endpoints, proxy, editor.",
           "4. **Workflow engine**: workflow definition + custom-block (`blocks.yaml`) schema.",
           "5. **Other config files**: `agents.json`, toolsets, guardrails.",
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
           "Structured options (arrays, nested objects: e.g. `ensemble.reference_models`) "
           "are not CLI-settable; they are written into the config file under the "
           "sections listed at the end.",
           ""]

    runtime = [(k, t) for k, t, g in fields if g == "FGROUP_RUNTIME"]
    deploy = [(k, t) for k, t, g in fields if g == "FGROUP_DEPLOY"]
    advanced = [(k, t) for k, t, g in fields if g == "FGROUP_ADVANCED"]
    dev = [(k, t) for k, t, g in fields if g == "FGROUP_DEV"]

    undescribed = sorted(k for k, _ in runtime if k not in CFG_KEY_DESC)
    out.append(f"## CLI-settable keys ({len(runtime)})")
    out.append("")
    out.append("The everyday runtime surface. Deploy-time, advanced-tuning, and dev-only "
               "keys are still `aimee config set`-able but are filed into their own "
               "subsections below (and hidden from the Settings surface by default).")
    out.append("")
    out.append("| Key | Type | Description |")
    out.append("|-----|------|-------------|")
    for key, typ in sorted(runtime):
        out.append(f"| `{key}` | {typ} | {CFG_KEY_DESC.get(key, 'n/a')} |")
    out.append("")
    if undescribed:
        out.append("> **Undocumented** (add to `CFG_KEY_DESC` in gen-reference-docs.py): "
                   + ", ".join(f"`{k}`" for k in undescribed))
        out.append("")

    for title, blurb, group in (
        ("Deploy-time keys", "Consumed once by `config_emit_deploy_env` to stand up the "
         "managed sibling services (`aimee config deploy-env`); not read at runtime. Set at "
         "deploy, not tuned day-to-day.", deploy),
        ("Advanced tuning keys", "Expert scalars with sensible defaults; settable in the "
         "config file but off the everyday surface.", advanced),
        ("Dev-only keys", "Internal dogfood/QA knobs; not part of the user surface.", dev),
    ):
        if group:
            out.append(f"### {title} ({len(group)})")
            out.append("")
            out.append(blurb)
            out.append("")
            out.append("| Key | Type | Description |")
            out.append("|-----|------|-------------|")
            for key, typ in sorted(group):
                out.append(f"| `{key}` | {typ} | {CFG_KEY_DESC.get(key, 'n/a')} |")
            out.append("")

    out.append(f"## Config-file sections ({len(sections)})")
    out.append("")
    out.append("Set in the config JSON as `{\"<section>\": {\"<key>\": ...}}`. Keys "
               "are derived from the external config module metadata; a key shown "
               "as a bare name that is itself a nested object is noted in the section "
               "description (see *Coverage & limitations*).")
    out.append("")
    for sect in sorted(sections):
        keys = ", ".join(f"`{k}`" for k in sorted(sections[sect]))
        desc = SECTION_DESC.get(sect)
        lead = f"_{desc}_ Keys: " if desc else ""
        out.append(f"- **`{sect}`**: {lead}{keys}")
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
# the reference: keeping this gate honest is the whole point.

# Hardened offline binaries copy environment values before clearenv(); treat
# that local accessor exactly like getenv() so their deployment contract is
# not silently omitted from generated reference docs.
ENV_RE = re.compile(r'(?:getenv|copy_env)\(\s*"(AIMEE_[A-Z0-9_]+)"')

# Helpers that take the env var NAME as an argument and getenv() it internally.
# config_sidecar_endpoint is the OCR/TSR resolver: centralising those two reads
# moved "AIMEE_OCR_URL" / "AIMEE_TSR_URL" out of a literal getenv() call and into
# a parameter, and the scan above stopped seeing them -- two variables dropped
# out of the generated reference while still being read at runtime. That is the
# exact silent-omission this scan exists to prevent, so the shape is matched
# rather than the vars being hard-coded into ENV_DYNAMIC (which is for
# credentials with no getenv at all). Add a helper here when it takes an env
# name; the name must still appear as a literal at the call site.
ENV_BY_NAME_RE = re.compile(
    r'config_sidecar_endpoint\([^;]*?"(AIMEE_[A-Z0-9_]+)"', re.S)

# Credential names consumed by the generic first-boot sealer are intentionally
# not read through individual getenv() calls. Keep their deployment contract in
# the generated reference anyway.
ENV_DYNAMIC = {
    "AIMEE_FORGE_APP_PRIVATE_KEY",
    "AIMEE_FORGE_TOKEN",
    "AIMEE_KB_CONN",
    "AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE",
    "AIMEE_SERVER_TLS_PRIVATE_KEY",
    "AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY",
    "AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY",
    "AIMEE_VAULT_PKCS11_PIN",
    "AIMEE_WEBCHAT_USER",
    "AIMEE_WEBCHAT_PASSWORD",
    "AIMEE_WEBCHAT_USERS",
    "AIMEE_VAULT_ENV_OVERWRITE",
}

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
    "AIMEE_CAPTURE_DIR": ("Paths & assets", "Directory for best-effort event-bus capture sessions. Defaults to `AIMEE_HOME`; capture failure is exposed in health and recorded in the WORM ledger."),
    "AIMEE_DB1_PATH": ("Paths & assets", "SQLite database the DB1 module process opens. The module is a separate process and cannot read the config store, so it is told the path and refuses to start without it rather than guessing a default and serving a different, empty database. Set it whenever `db1_path` is overridden in the configuration; the container entrypoint otherwise defaults it to `<AIMEE_HOME>/aimee.db`, which is config's own default."),
    "AIMEE_INSTALL_PREFIX": ("Paths & assets", "Install prefix used to locate bundled assets and plugins."),
    "AIMEE_BUNDLED_SKILLS_DIR": ("Paths & assets", "Override directory for the bundled skills."),
    "AIMEE_TOOLSETS_CONFIG": ("Paths & assets", "Path to a toolsets config file (overrides the default tool allowlists)."),
    "AIMEE_GUARDRAILS_PATH": ("Paths & assets", "Path to the guardrails policy file."),
    "AIMEE_FORENSICS_DIR": ("Paths & assets", "Directory for shutdown-forensics dumps."),
    "AIMEE_PACK_DIR": ("Paths & assets", "Directory of memory profile packs."),
    "AIMEE_HARNESS_MEMORY_SCOPES": ("Paths & assets", "Path to the agent memory-surface registry config (default `<AIMEE_HOME>/harness_memory_scopes.conf`). Each `client:projects_root:memory_seg` line adds a new agent or overrides a built-in's paths for memory-write interception (writes are redirected into aimee's db1)."),
    "AIMEE_WORKSPACES_DIR": ("Paths & assets", "Root directory for mirrored/registered workspaces."),
    "AIMEE_MODELS_DEV_SNAPSHOT": ("Paths & assets", "Path to an offline models.dev catalog snapshot."),
    # Client & session
    "AIMEE_SERVER_URL": ("Client & session", "aimee-server endpoint the thin client connects to (UDS path or `tcp:host:port`)."),
    "AIMEE_SERVER_TOKEN": ("Client & session", "Bearer token presented to aimee-server over TCP."),
    "AIMEE_TRANSPORT_SERVER_KEEPALIVE_ENABLED": ("Client & session", "Resident HTTPS connection reuse. Defaults on; set to 0 to restore one request per connection."),
    "AIMEE_TRANSPORT_THINCLIENT_GZIP_ENABLED": ("Client & session", "Negotiated gzip for eligible buffered thin-client routes. Defaults off; set to 1 only for a measured remote link profile."),
    "AIMEE_API_ENDPOINT": ("Client & session", "Override the `/v1` API endpoint used by the client RPC layer."),
    "AIMEE_API_BEARER": ("Client & session", "Bearer token for the `/v1` API endpoint."),
    "AIMEE_SESSION_ID": ("Client & session", "Pre-set the session id (enables non-blocking session attach)."),
    "AIMEE_TUI_SESSION": ("Client & session", "Identifies the TUI session."),
    "AIMEE_ATTACH_ID": ("Client & session", "Presence attach id used when joining an existing session."),
    "AIMEE_HOOK_CLIENT": ("Client & session", "Identifies the calling hook client (e.g. claude/codex) for hook routing."),
    "AIMEE_NO_CLIENT_INTEGRATIONS": ("Client & session", "If set (to any value other than 0/false), aimee does not auto-register itself into detected AI-tool user configs (Claude Code, Gemini, Copilot, Codex). Overrides the client_integrations_enabled config; honored by the aimee binary and by install.sh/configure-hooks.sh."),
    "AIMEE_MODEL": ("Client & session", "Override the primary model for the session."),
    "AIMEE_EFFORT": ("Client & session", "Reasoning-effort hint for the session/model."),
    "AIMEE_MODE": ("Client & session", "Operating-mode override (e.g. interactive / autonomous)."),
    "AIMEE_PROFILE": ("Client & session", "Active working-profile name."),
    "AIMEE_ACTIVE_TOOLSET": ("Client & session", "Active toolset (tool allowlist) for the session."),
    "AIMEE_SESSION_START_VERBOSE": ("Client & session", "Verbose logging during session start."),
    # Server runtime
    "AIMEE_SERVER_HTTP_BIND": ("Server runtime", "TCP bind address for the server `/v1` HTTP listener (else UDS-only)."),
    "AIMEE_SERVER_STARTUP_FD": ("Server runtime", "Inherited fd for startup-readiness signalling (service launch)."),
    "AIMEE_API_REMOTE_WRITES": ("Server runtime", "Legacy value: `off`, `data`, or `full`. Still parsed, but no longer authorizes user writes; non-off values warn and feed `remote_writes.global_ignored`."),
    "AIMEE_API_MTLS": ("Server runtime", "Client-certificate mode: `off`, `optional`, or `required`. The managed server image defaults to `optional` so enrollment works before the durable roster promotes the listener to required."),
    "AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT": (
        "Server runtime",
        "Permit the operator-configured search backend (`search.searxng_url`) to resolve to a "
        "private, loopback, or link-local address. Off by default: every outbound fetch is "
        "validated and pinned, so a self-hosted SearXNG on a LAN address is refused unless this "
        "is set. Deliberately an environment variable rather than a config key, because "
        "`config.set` is reachable from inside the running system and pointing the search backend "
        "at a cloud metadata address would exfiltrate instance credentials through a tool that "
        "looks like search. Set to exactly `1`; any other value is off. Never widens fetches of "
        "model-supplied or search-result URLs, which stay denied.",
    ),
    "AIMEE_WEBCHAT_GIT": ("Server runtime", "Per-webuser webchat git surface: repo connect/clone, git ops (pull/commit/push/branch), per-host token + SSH-key credential intake, the workspace forge-token broker, project listing + session-dir resolution, and \"Sign in with GitHub\". It is on by default. Set the literal value 0 to disable the entire surface; all of those routes then return 503. Any other value leaves it on. Independent of AIMEE_WEBCHAT_EDITOR."),
    "AIMEE_WEBCHAT_EDITOR": ("Server runtime", "Per-webuser in-browser code-server editor (on by default; set to 0 to disable; needs a code-server binary, shipped by WITH_VSCODE images)."),
    "AIMEE_WEBCHAT_EDITOR_BIN": ("Server runtime", "Override path to the code-server binary used for the in-browser editor."),
    "AIMEE_WEBCHAT_EDITOR_IDLE_SECS": ("Server runtime", "Idle timeout in seconds before a per-webuser code-server editor is reaped. Default 1800 (30 min); positive values are clamped to [60, 604800]; 0 disables idle reaping; malformed/negative/overflow values fall back to the default. An actively-open editor is kept alive by the proxy keepalive, so it is not reaped mid-session."),
    "AIMEE_WEBCHAT_EDITOR_UID": ("Server runtime", "Dedicated service user the per-webuser code-server drops to (defence in depth; only honoured when aimee-server runs as root)."),
    "AIMEE_GITHUB_OAUTH_CLIENT_ID": ("Server runtime", "Client ID of a GitHub OAuth App for the webchat \"Sign in with GitHub\" button; populates the github.com git credential. Public. Overrides the built-in default baked in via oauth_defaults.h."),
    "AIMEE_GITHUB_OAUTH_CLIENT_SECRET": ("Server runtime", "First-boot transport for the GitHub OAuth App client secret. The entrypoint synchronously seals it into Vault and scrubs the environment before aimee-server starts; startup fails closed if custody cannot be established. Enables browser redirect sign-in; without it the button falls back to the device-code flow."),
    "AIMEE_GITLAB_OAUTH_CLIENT_ID": ("Server runtime", "Client ID of a GitLab OAuth application (device flow enabled) for the webchat \"Sign in with GitLab\" button on gitlab.com. Public. Overrides the built-in default baked in via oauth_defaults.h."),
    "AIMEE_DEPLOY_ENABLED": ("Server runtime", "Set to 1 to enable the server-orchestrated deploy: the setup wizard runs `docker compose up -d` for the managed sibling service (aimee-kb) via a mounted Docker socket. Off unless the deploy compose sets it."),
    "AIMEE_DEPLOY_COMPOSE_FILE": ("Server runtime", "Path to the managed compose file the server-orchestrated deploy runs (default /opt/aimee/deploy/aimee-managed.compose.yaml)."),
    "AIMEE_INGRESS_PROXY_SECRET": ("Server runtime", "First-boot transport for the shared secret authenticating trusted ingress identity headers. It is sealed into Vault and removed from the environment before the long-lived server starts."),
    "AIMEE_PARALLEL_MAX": ("Server runtime", "Maximum parallel agent fan-out."),
    "AIMEE_BACKGROUND_THREADS": ("Server runtime", "Background worker thread count."),
    "AIMEE_COMPUTE_THREADS": ("Server runtime", "Compute-pool thread count."),
    "AIMEE_SESSION_THREADS": ("Server runtime", "Per-session worker thread count."),
    "AIMEE_WORKTREE_GC": ("Server runtime", "Enable/disable delegate-worktree garbage collection."),
    "AIMEE_WORKTREE_GC_DAYS": ("Server runtime", "Age threshold (days) for worktree GC."),
    "AIMEE_SOCK": ("Server runtime", "Sandbox helper socket path."),
    # Knowledge base
    "SYNTHESIS_ENDPOINT": ("Knowledge base (aimee-kb)", "Synthesis endpoint (every curator stage, at {url}/v1). No longer selects an embedder: the kb embeds in-container, and EMBEDDER_URL points at an external embedder. See docs/SYNTHESIS_MODELS.md for what to put behind it and docs/KB_LLM_BACKENDS.md for the provider surface."),
    "SYNTHESIS_API_KEY": ("Managed KB and inference", "First-boot transport for the bearer aimee-kb presents to the external synthesis endpoint. aimee-kb synchronously seals it into Vault, scrubs the environment, and cleanly re-execs before serving; wizard-managed deploys generate the 256-bit value in Vault. This is separate from user/server bearers."),
    "SYNTHESIS_AUTH_REQUIRED": ("Managed KB and inference", "Set to 1 on wizard-managed KBs so synthesis clients refuse to contact the LLM when its bearer service identity is missing."),
    "AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE": ("Managed KB and inference", "Explicit first-boot migration/adoption transport for an existing wizard-managed LLM credential. aimee-server seals it into Vault and scrubs the environment before normal startup. Ordinary inherited SYNTHESIS_API_KEY is ignored by managed credential creation so stale child-service state cannot win. Must be a 32..512 character RFC 6750 b64token."),
    "AIMEE_OFFLINE_ALLOW_NO_SWAP_MLOCK_FALLBACK": (
        "Managed KB and inference",
        "Internal managed-authority switch: still attempts mlockall first, but when an unprivileged container cannot raise RLIMIT_MEMLOCK, permits the offline one-shot to continue only if the kernel reports no active swap. Operator-run custody tools leave this unset and retain mandatory mlockall.",
    ),
    "SYNTHESIS_MODEL": ("Knowledge base (aimee-kb)", "Model label sent to SYNTHESIS_ENDPOINT's chat endpoint (single-model gateways ignore it). Default 'aimee-synth'."),
    "EMBEDDER_URL": ("Knowledge base (aimee-kb)", "Embedder endpoint override (/embed, /embed_batch); takes precedence over SYNTHESIS_ENDPOINT for embedding."),
    "AIMEE_KB_API_URL": ("Knowledge base (aimee-kb)", "aimee-kb HTTP API base URL."),
    "AIMEE_KB_API_BEARER_TOKEN": ("Knowledge base (aimee-kb)", "First-boot transport for the aimee-kb API bearer token. Server and KB bootstrap paths seal it into Vault and remove it from the environment before long-lived service startup."),
    "AIMEE_KB_CLIENT_BEARER_TOKEN": ("Knowledge base (aimee-kb)", "Rotating bearer aimee-server presents on its mTLS connection to aimee-kb. Bootstrap seals the first-boot value into Vault and removes it from the environment; the client reads it live for every request."),
    "AIMEE_KB_CLIENT_OIDC_TOKEN": ("Knowledge base (aimee-kb)", "OIDC access token aimee-server presents as its third service-identity layer. Bootstrap seals it into Vault; the KB pins RS256, issuer, audience, typ=at+jwt, subject, and the subject-to-certificate service name."),
    "AIMEE_KB_CLIENT_PAM_USERNAME": ("Knowledge base (aimee-kb)", "Host account name for the third service-identity layer when KB OIDC is not configured. Bootstrap seals it into Vault; changing it requires a matching PAM account and service certificate enrollment."),
    "AIMEE_KB_CLIENT_PAM_PASSWORD": ("Knowledge base (aimee-kb)", "Rotating PAM password paired with AIMEE_KB_CLIENT_PAM_USERNAME. Bootstrap seals it into Vault, removes it from the environment, and the client reads it live for every request."),
    "AIMEE_KB_API_CA_BUNDLE": ("Knowledge base (aimee-kb)", "CA bundle path for verifying the aimee-kb TLS certificate."),
    "AIMEE_KB_CACHE_TTL_S": ("Knowledge base (aimee-kb)", "KB client cache TTL (seconds)."),
    "AIMEE_KB_CONN": (
        "Knowledge base (aimee-kb)",
        "First-boot KB connection string; sealed into the server Vault before long-lived startup.",
    ),
    "AIMEE_TRANSPORT_KB_POOL_ENABLED": ("Knowledge base (aimee-kb)", "Override server-to-KB mTLS connection pooling. The config default is on; set to 0 for one-shot connections."),
    "AIMEE_SERVER_ID": ("Knowledge base (aimee-kb)", "Registry identity used by the server mTLS heartbeat."),
    "AIMEE_SERVER_TEAM_ID": (
        "Knowledge base (aimee-kb)",
        "The team this server serves, from the same registry row as AIMEE_SERVER_ID. "
        "Required for per-user /v1 write authorization: unset, the server still starts "
        "and serves reads but denies every write with no_team_configured.",
    ),
    "AIMEE_KB_HTTP_BIND": ("Knowledge base (aimee-kb)", "aimee-kb HTTP listener bind address."),
    "AIMEE_KB_MTLS_HOST": (
        "Knowledge base (aimee-kb)",
        "Advertised mTLS hostname placed in the aimee-kb server certificate; the listener binds all interfaces.",
    ),
    "AIMEE_KB_MTLS_PORT": ("Knowledge base (aimee-kb)", "aimee-kb mTLS listener port."),
    # The two sidecar hops. Naming a sidecar is what makes the kb mint the mTLS
    # identities for it at startup, so these read as wiring rather than as a model
    # choice: EMBEDDER_MODEL says what to embed with, and is equally satisfied by an
    # external endpoint, while this says a container exists on the aimee network to
    # issue a certificate for. Leaving them unset is the supported external-provider
    # deployment, not a misconfiguration.
    "AIMEE_LLM_HOST": (
        "Knowledge base (aimee-kb)",
        "DNS name of the synthesis sidecar container. Setting it makes aimee-kb issue the "
        "mTLS identities for the kb -> aimee-llm hop into $AIMEE_HOME/synthesis-tls at "
        "startup, from the kb's own CA. Unset for an external or absent synthesis provider, "
        "which needs none of them. The sidecar refuses to start without this material.",
    ),
    "AIMEE_EMBEDDER_HOST": (
        "Knowledge base (aimee-kb)",
        "DNS name of the embedder sidecar container (aimee-embedder-a25m or "
        "aimee-embedder-nomic). Setting it makes aimee-kb issue the mTLS identities for the "
        "kb -> embedder hop into $AIMEE_HOME/embedder-tls at startup, independently of the "
        "synthesis hop. Unset for an external embedder reached over plain HTTPS, or when no "
        "embedder is deployed. The sidecar refuses to start without this material.",
    ),
    "AIMEE_KB_EMBED_ALL_FILES": (
        "Knowledge base (aimee-kb)",
        "Set to 1 to give EVERY indexed file a dense document vector, including source. "
        "Off by default because source files are already embedded by the code path, and "
        "embedding them a second time as prose was 82% of the doc-embedding token budget "
        "on a real corpus. Chunk rows are written either way, so lexical and FTS search "
        "over source is unaffected by this setting; only the redundant vector is skipped.",
    ),
    "AIMEE_EMBED_HTTP_TIMEOUT_MS": (
        "Knowledge base (aimee-kb)",
        "Deadline for one embedding HTTP call, default 180000. The previous hardcoded 30s "
        "was shorter than a cold model load plus a large batch, so the first request of a "
        "run could fail on a healthy embedder.",
    ),
    "AIMEE_KB_READ_TIMEOUT_MS": (
        "Knowledge base (aimee-kb)",
        "Deadline for a single KB read issued by the client, in milliseconds.",
    ),
    "AIMEE_KB_SCAN_TIMEOUT_MS": (
        "Knowledge base (aimee-kb)",
        "Deadline for a code-index scan request, in milliseconds. Scans are queued and "
        "drained by the ingest workers, so this bounds the REQUEST rather than the work.",
    ),
    "AIMEE_KB_EMIT_ENROLL": ("Knowledge base (aimee-kb)", "Emit a client enrollment token on KB start."),
    "AIMEE_KB_EMIT_SCOPE": ("Knowledge base (aimee-kb)", "Scope for the emitted enrollment token."),
    "AIMEE_KB_OIDC_ISSUER": ("Knowledge base (aimee-kb)", "OIDC issuer for KB API auth."),
    "AIMEE_KB_OIDC_AUDIENCE": ("Knowledge base (aimee-kb)", "OIDC audience for KB API auth."),
    "AIMEE_KB_OIDC_JWKS_FILE": ("Knowledge base (aimee-kb)", "OIDC JWKS file for KB API auth."),
    "AIMEE_KB_OIDC_SCOPE_CLAIM": ("Knowledge base (aimee-kb)", "OIDC claim carrying the scope."),
    "AIMEE_KB_OIDC_SCOPE_KIND": ("Knowledge base (aimee-kb)", "OIDC scope-kind interpretation."),
    # Relying-party profile for the per-user /v1 write login (proposal
    # per-user-remote-writes-authz.md §3). Setting the client id is what enables
    # the login front end; the client SECRET is deliberately absent from the
    # environment, being vault-custodied and read only at the code exchange.
    "AIMEE_KB_OIDC_LOGIN_CLIENT_ID": (
        "Knowledge base (aimee-kb)",
        "OIDC relying-party client id; setting it enables the per-user login front end.",
    ),
    "AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL": (
        "Knowledge base (aimee-kb)",
        "IdP authorization endpoint the login redirects to (https only).",
    ),
    "AIMEE_KB_OIDC_LOGIN_TOKEN_URL": (
        "Knowledge base (aimee-kb)",
        "IdP token endpoint for the code exchange (https, default port only).",
    ),
    "AIMEE_KB_OIDC_LOGIN_REDIRECT_URI": (
        "Knowledge base (aimee-kb)",
        "This kb's OIDC callback URL (https, or http on loopback only).",
    ),
    "AIMEE_KB_OIDC_LOGIN_SCOPE": (
        "Knowledge base (aimee-kb)",
        "Space-delimited OIDC scopes for the login request; defaults to openid.",
    ),
    "AIMEE_VECTOR_KB_BATCH_SIZE": ("Knowledge base (aimee-kb)", "Embedding batch size for KB vector ingest."),
    # Database & vectors
    "AIMEE_DB2_URL": ("Database & vectors", "Postgres (DB2) connection URL for the KB store."),
    "AIMEE_TEST_DB2_TEMPLATE_URL": (
        "Database & vectors",
        "Test-only. Postgres template database the DB2 test shim clones per test process, so unit "
        "tests run against the real engine instead of the sqlite shim (which translates DB2's SQL "
        "rather than executing it). Build the template with `make db2-test-template` and the suite "
        "with `make unit-tests-pg`; unset, tests use the sqlite shim as before. Read only by test "
        "binaries; no production code path consults it.",
    ),
    "AIMEE_DB2_STATEMENT_TIMEOUT_MS": (
        "Database & vectors",
        "Per-connection `statement_timeout` in ms. Defaults to the pool's stuck-lease "
        "ceiling (`DB2_POOL_HOLD_CEILING_MS`, 300000), because a statement must not "
        "outlive the duration that defines a lease as stuck. The pool can report such a "
        "lease but cannot reclaim it. The value must be canonical decimal digits with no "
        "sign, surrounding whitespace or leading zero. Exactly `0` disables the bound. This is a "
        "deliberate opt-out for genuinely long work. Every other spelling of zero "
        "(`00`, `+0`, `-0`, ` 0`) is treated as malformed. Anything malformed or "
        "out-of-range falls back to the default and never to unlimited, so no typo can "
        "silently remove the bound.",
    ),
    "AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS": (
        "Database & vectors",
        "Per-connection `idle_in_transaction_session_timeout` in ms, defaulting to the "
        "same pool stuck-lease ceiling (`DB2_POOL_HOLD_CEILING_MS`, 300000). "
        "`statement_timeout` bounds a STATEMENT, so a unit of work that opens a "
        "transaction and then stalls before its next statement is invisible to it and "
        "holds its pool member indefinitely. This measured at about 4.5 hours against a "
        "five-minute ceiling. Postgres ends such a backend itself, so the stalled thread "
        "unwinds and the lease is returned without a restart. Same value grammar as "
        "`AIMEE_DB2_STATEMENT_TIMEOUT_MS`; exactly `0` opts out, independently of the "
        "statement bound.",
    ),
    "EMBEDDER_DIMS": ("Database & vectors", "Embedding dimension (drives halfvec column sizing)."),
    "AIMEE_PGVEC_SLOW_QUERY_MS": ("Database & vectors", "Slow-query log threshold (ms) for the pgvector transport."),
    # Memory
    "AIMEE_MEMORY_CITATIONS_MODE": ("Memory", "Citation rendering mode for memory recall."),
    "AIMEE_MEMORY_CITATIONS_STRIP_UNVERIFIED": ("Memory", "Strip unverified citations from recall output."),
    "AIMEE_MEMORY_COGNIFY_ASYNC_ENABLED": ("Memory", "Enable the async cognify pipeline."),
    "AIMEE_MEMORY_COREF_MODE": ("Memory", "Coreference-resolution mode."),
    "AIMEE_MEMORY_MAINTENANCE_TRIGGER_INSERTS": ("Memory", "Inserts before a maintenance cycle triggers."),
    "AIMEE_MEMORY_MAINTENANCE_TRIGGER_SECS": ("Memory", "Seconds before a maintenance cycle triggers."),
    "AIMEE_MEMORY_PAGERANK_RELATIONS": ("Memory", "Relation types included in memory PageRank."),
    "AIMEE_MEMORY_RERANK_MODE": ("Memory", "Reranker mode."),
    "AIMEE_EMBEDDERS_FILE": ("Knowledge base (aimee-kb)", "Path to the embedder registry the server reads for GET /v1/embedders (the setup wizard's embedder picker). Defaults to /opt/aimee/embedders.json, then scripts/embedders.json in a source checkout. The same file the in-container embedder reads, so one declaration drives the picker, the loading and the serving flags."),
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
    "AIMEE_FORWARDER_PORT": (
        "Delegates & backends",
        "Loopback port the in-sandbox aimee-forwarder listens on (default 3129); set by "
        "aimee when it starts the forwarder in a proxy-mode delegate container.",
    ),
    "AIMEE_FORWARDER_SOCK": (
        "Delegates & backends",
        "UNIX socket the in-sandbox aimee-forwarder bridges to (default "
        "/run/aimee/aimee-http.sock, the bound aimee UDS).",
    ),
    "AIMEE_DOCKER_WORKDIR": ("Delegates & backends", "Docker delegate-backend working directory."),
    "AIMEE_SSH_BIN": ("Delegates & backends", "SSH delegate-backend binary."),
    "AIMEE_OPENCODE_BIN": ("Delegates & backends", "opencode CLI frontend binary."),
    # Forge
    "AIMEE_FORGE_API_BASE": ("Forge (GitHub App / tokens)", "Forge API base URL."),
    "AIMEE_FORGE_APP_ID": ("Forge (GitHub App / tokens)", "GitHub App id for minting forge tokens."),
    "AIMEE_FORGE_APP_INSTALLATION_ID": ("Forge (GitHub App / tokens)", "GitHub App installation id."),
    "AIMEE_FORGE_APP_PRIVATE_KEY": ("Forge (GitHub App / tokens)", "GitHub App private-key PEM accepted only as first-boot transport; it is sealed into Vault and filesystem paths are rejected."),
    "AIMEE_FORGE_SCOPE": ("Forge (GitHub App / tokens)", "Scope for the minted forge token."),
    "AIMEE_FORGE_TOKEN": ("Forge (GitHub App / tokens)", "First-boot static forge token. aimee-server seals it into the server Vault and unsets it before serving; subsequent boots read only from Vault."),
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
    "AIMEE_WORKFLOW_REPO": ("Workflow engine", "Legacy C workflow fallback for a local repository. The Go WFE uses the repository admitted with each work item."),
    "AIMEE_WORKFLOW_BASE": ("Workflow engine", "Legacy C workflow fallback for the freeze/diff base. It does not set the Go WFE integration branch."),
    "AIMEE_AUTONOMY_PANEL_RETRIES": ("Workflow engine", "Legacy C scheduler budget for retrying transient roundtable parks. It does not configure the Go WFE scheduler."),
    "AIMEE_DEFAULT_BRANCH": ("Workflow engine", "Legacy C workflow override for default-branch resolution. The Go WFE derives branch authority from the admitted repository and checkout."),
    # Git verify / MCP
    "AIMEE_VERIFY_PARALLEL": ("Git verify / MCP", "Run `aimee git verify` steps in parallel."),
    "AIMEE_VERIFY_LOCK_FILE": (
        "Git verify / MCP",
        "Override the host-wide file lock that serializes complete repository verification runs.",
    ),
    "AIMEE_EXEC_PIPE_TIMEOUT_MS": (
        "Agents & delegates",
        "How long a sidecar subprocess (embed, cognify, rewrite, css render, "
        "oauth token, guardrails) may run before it is killed, in ms. Default 120000. "
        "Bounds the pathological case, not normal latency: an unbounded wait here parks "
        "the calling request thread permanently when a sidecar hangs instead of exiting, "
        "which has taken a kb offline while it still accepted connections. On expiry the "
        "child's whole process GROUP is killed, because the immediate child is /bin/sh "
        "and the work is its child.",
    ),
    "AIMEE_VERIFY_STEP_TIMEOUT_MS": ("Git verify / MCP", "Per-step timeout (ms) for git verify."),
    "AIMEE_MCP_CWD": ("Git verify / MCP", "Working-directory hint for MCP git-root resolution."),
    "AIMEE_MCP_TOOL_PROFILE": ("Git verify / MCP", "MCP tools/list presentation profile: 'core'/'lean' (default: Tier-0 high-frequency tools only, with find_tools/describe_tool reaching the rest) or 'full' (present every tool upfront)."),
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
    "AIMEE_ALLOW_MAIN_CHECKOUT": ("Delegates & backends", "Allow an explicitly authorized delegate path to use the main checkout instead of a managed worktree."),
    "AIMEE_API_BEARER_TOKEN": ("Server runtime", "Optional first-boot bearer for the public server API listener. It is synchronously sealed into Vault and removed from the process environment before services start; when omitted, the server mints a random Vault-only primary."),
    "AIMEE_SERVER_TLS_PRIVATE_KEY": ("Server runtime", "Optional PEM private-key content for first boot. The key is synchronously sealed into Vault and the environment is scrubbed; only the public server certificate may remain on disk."),
    "AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY": ("Server runtime", "PEM private-key content for the management listener, accepted only as first-boot transport and synchronously sealed into Vault before the server starts."),
    "AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY": ("Server runtime", "PEM private-key content for the management-status client, accepted only as first-boot transport and synchronously sealed into Vault before the server starts."),
    "AIMEE_SERVER_MGMT_TLS_KEY": ("Server runtime", "Forbidden legacy private-key file setting. Startup rejects it; use AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY as first-boot Vault input."),
    "AIMEE_SERVER_MGMT_STATUS_CLIENT_KEY": ("Server runtime", "Forbidden legacy private-key file setting. Startup rejects it; use AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY as first-boot Vault input."),
    "AIMEE_WEBCHAT_USER": ("Server runtime", "Optional first-boot webchat username paired with AIMEE_WEBCHAT_PASSWORD. The bootstrap record is sealed into Vault and removed from the environment before runtime-web starts."),
    "AIMEE_WEBCHAT_PASSWORD": ("Server runtime", "Optional first-boot webchat password paired with AIMEE_WEBCHAT_USER. The bootstrap record is sealed into Vault and removed from the environment before runtime-web starts."),
    "AIMEE_WEBCHAT_USERS": ("Server runtime", "Optional first-boot webchat account registry. It is sealed into Vault and removed from the environment before runtime-web starts."),
    "AIMEE_VAULT_ENV_OVERWRITE": ("Server runtime", "First-boot control flag allowing supplied credential values to replace existing Vault records. It is not itself a credential."),
    "AIMEE_AUTONOMY_BASE": ("Workflow engine", "Legacy C workflow integration-branch fallback. The Go WFE uses the branch checked out when it admits the repository."),
    "AIMEE_PR_BASE_MODE": ("Workflow engine", "What a pr.open with no explicit base targets: the run's feature branch (default) or, when set to default_branch, the autonomous base. The server exports the configured pr_base_mode into this variable at startup, so `aimee config set pr_base_mode` stays the one operator knob; the workflow engine reads it from the environment because that module is deliberately config-free."),
    "AIMEE_AUTONOMY_MAX_ACTIVE_PER_PRINCIPAL": ("Workflow engine", "Maximum active autonomous work items for one authenticated principal."),
    "AIMEE_AUTONOMY_MAX_USD": ("Workflow engine", "Default USD ceiling for an autonomous work item; 0 disables this default ceiling."),
    "AIMEE_AUTONOMY_SUBMIT_RATE_PER_MIN": ("Workflow engine", "Autonomous-submission rate limit per principal."),
    "AIMEE_AUTONOMY_SUBMIT_WINDOW_SECS": ("Workflow engine", "Window used by the autonomous-submission rate limiter."),
    "AIMEE_AUTONOMY_USD_PER_SEC": ("Workflow engine", "Fallback spend estimator for autonomous admission when exact provider cost is unavailable."),
    "AIMEE_CI_WEBHOOK_SECRET": ("Workflow engine", "HMAC secret authenticating inbound CI webhook events."),
    "AIMEE_CLIENT_TYPE": ("Client & session", "Calling client type used for integration-specific request shaping."),
    "AIMEE_CODEX_REFRESH_SKEW": ("Delegates & backends", "Seconds before Codex OAuth expiry at which the server refreshes the token."),
    "AIMEE_CODE_INDEX_SOURCE": ("Knowledge base (aimee-kb)", "Source label recorded for code-index ingestion."),
    "AIMEE_DB2_EVAL_URL": ("Database & vectors", "Separate DB2 URL used by evaluation harnesses; never the production default. The harness applies the DB2 schema into the named database: into its public schema when that schema is empty, otherwise into a throwaway schema beside it. Either way the copy is dropped on close, so point this at a disposable server."),
    "AIMEE_DB2_POOL_SIZE": ("Database & vectors", "DB2 connection-pool size override."),
    "AIMEE_DELEGATE_MAX_INFLIGHT": ("Delegates & backends", "Process-wide maximum number of admitted delegate attempts."),
    "AIMEE_DELEGATE_SANDBOX": ("Delegates & backends", "Enable the configured delegate sandbox backend."),
    "AIMEE_DIM_PROBE_BUDGET_MS": ("Database & vectors", "Time budget for probing an embedder's output dimension."),
    "AIMEE_IR_PATH": ("Diagnostics & misc", "Diagnostic path for recording canonical request IR."),
    "AIMEE_IR_RESP_PATH": ("Diagnostics & misc", "Diagnostic path for recording canonical response IR."),
    "AIMEE_IR_SHADOW": ("Diagnostics & misc", "Run the canonical IR path in comparison/shadow mode."),
    "AIMEE_IR_STREAM_RELAY": ("Diagnostics & misc", "Enable the canonical streaming-response relay."),
    "AIMEE_KB_HARDENED": ("Knowledge base (aimee-kb)", "Require the hardened KB custody and transport posture at startup."),
    "AIMEE_KB_JWKS_MANIFEST_ROOT_CUSTODY_ID": ("Knowledge base (aimee-kb)", "Custody identifier for the key that signs JWKS manifests."),
    "AIMEE_KB_JWKS_PUBLICATION_HWM_CUSTODY_ID": ("Knowledge base (aimee-kb)", "Custody identifier for the JWKS publication high-water mark."),
    "AIMEE_KB_JWKS_PUBLISH_DSN": ("Knowledge base (aimee-kb)", "Provisioning database URL used by the JWKS publisher; secret."),
    "AIMEE_KB_MGMT_STATUS_SECONDARY_LEAF_PIN": ("Knowledge base (aimee-kb)", "Secondary TLS leaf pin accepted during management-status certificate rotation."),
    "AIMEE_KB_MTLS_MAX_CONNECTIONS": ("TLS & networking", "Maximum concurrent connections accepted by the KB mTLS listener."),
    "AIMEE_KB_OIDC_MAX_TOKEN_AGE": ("Knowledge base (aimee-kb)", "Maximum accepted age in seconds for a KB OIDC token."),
    "AIMEE_KB_RUNTIME_UID": ("Knowledge base (aimee-kb)", "Numeric runtime user allowed to receive the management token-authority socket."),
    "AIMEE_KB_STATUS_BIND": ("Knowledge base (aimee-kb)", "Bind address for the management-status authority."),
    "AIMEE_KB_STATUS_DSN": ("Knowledge base (aimee-kb)", "Runtime database URL used by the management-status authority; secret."),
    "AIMEE_KB_STATUS_PORT": ("Knowledge base (aimee-kb)", "Listen port for the management-status authority."),
    "AIMEE_KB_STATUS_PROVISION_DSN": ("Knowledge base (aimee-kb)", "Provisioning database URL used to initialize management-status state; secret."),
    "AIMEE_KB_STATUS_TLS_CA": ("TLS & networking", "CA file used by the management-status authority."),
    "AIMEE_KB_STATUS_TLS_CERT": ("TLS & networking", "TLS certificate for the management-status authority."),
    "AIMEE_KB_STATUS_TLS_KEY": ("TLS & networking", "TLS private key for the management-status authority; secret."),
    "AIMEE_KB_TOKEN_AUTHORITY_DSN": ("Knowledge base (aimee-kb)", "Database URL used by the management token authority; secret."),
    "AIMEE_KB_TOKEN_AUTHORITY_SOCKET_GID": ("Knowledge base (aimee-kb)", "Group allowed to connect to the management token-authority socket."),
    "AIMEE_KB_TOKEN_ROOTS_PROVISION_DSN": ("Knowledge base (aimee-kb)", "Provisioning database URL used to initialize token roots; secret."),
    "AIMEE_KB_TOKEN_ROOT_CUSTODY_ID": ("Knowledge base (aimee-kb)", "Custody identifier for the management token signing root."),
    "AIMEE_KB_VAULT_OPERATOR_ENABLED": ("Knowledge base (aimee-kb)", "Enable the dedicated KB vault-operator runtime."),
    "AIMEE_KB_VAULT_ORCHESTRATOR_URL": ("Knowledge base (aimee-kb)", "Operator-configured vault orchestrator endpoint."),
    "AIMEE_MGMT_STATUS_KEY_ID": ("Server runtime", "Identifier of the management-status verification key."),
    "AIMEE_MGMT_STATUS_PUBLIC_KEY": ("Server runtime", "Hex-encoded Ed25519 key used to verify management-status staples."),
    "AIMEE_MODULE_ROUNDTABLE": ("Server runtime", "Enable the optional roundtable module; invalid values fail closed to off."),
    "AIMEE_OCR_URL": ("Knowledge base (aimee-kb)", "Structured-PDF OCR sidecar endpoint."),
    "AIMEE_OAUTH_RUNTIME_DIR": ("Paths & assets", "Private directory for transient OAuth callback/session state; it must not be used for durable credentials."),
    "AIMEE_ORCH_DELEGATES": ("Workflow engine", "Enable delegate resource use by the orchestration plane."),
    "AIMEE_ORCH_WORKFLOWS": ("Workflow engine", "Enable workflow orchestration surfaces."),
    "AIMEE_PANEL_SEAT_WAIT_SECS": ("Workflow engine", "Maximum wait for a roundtable seat to acquire an eligible agent."),
    "AIMEE_PRIMARY_CLI_INGESTOR": ("Client & session", "Name of the primary CLI integration that owns session-ingest events."),
    "AIMEE_PROJECT_ID": ("Client & session", "Explicit project identity for an integration request."),
    "AIMEE_RUNTIME_DIR": ("Paths & assets", "Private runtime directory for sockets, temporary credentials, and process state."),
    "AIMEE_SANDBOX_HOST_MOUNTS": ("Delegates & backends", "Operator allowlist of host mounts available to the sandbox backend."),
    "AIMEE_SERVER_MGMT_BIND": ("Server runtime", "Bind address that enables the server management listener when its full TLS configuration is present."),
    "AIMEE_SERVER_MGMT_ISSUER": ("Server runtime", "Expected issuer for management-plane bearer identities."),
    "AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE": ("Server runtime", "Root-owned trust bundle used to verify signed management JWKS publications."),
    "AIMEE_SERVER_MGMT_STATUS_SECONDARY_LEAF_PIN": ("Server runtime", "Secondary management-status TLS leaf pin accepted during certificate rotation."),
    "AIMEE_STAGE_GOVERNANCE": ("Server runtime", "Enable the governance stage in the canonical request pipeline."),
    "AIMEE_STAGE_MEMORY": ("Server runtime", "Enable the memory stage in the canonical request pipeline."),
    "AIMEE_TEST_PG_URL": ("Diagnostics & misc", "PostgreSQL URL used only by live test harnesses."),
    "AIMEE_TLS_CLIENT_P12_PASS": ("TLS & networking", "Password for an explicitly provisioned client PKCS#12 bundle; secret."),
    "AIMEE_TLS_CN": ("TLS & networking", "Common Name used when generating a local TLS certificate."),
    "AIMEE_TLS_EXTRA_SAN": ("TLS & networking", "Additional subject-alternative names for a generated TLS certificate."),
    "AIMEE_TSR_URL": ("Knowledge base (aimee-kb)", "Structured-PDF table-recognition sidecar endpoint."),
    "AIMEE_VAULT_KMS_HELPER": ("Server runtime", "Executable implementing the configured external KMS wrap/unwrap contract."),
    "AIMEE_VAULT_KMS_HWM_DOMAIN": ("Server runtime", "Domain separator for KMS high-water-mark signatures."),
    "AIMEE_VAULT_KMS_HWM_PUBKEY": ("Server runtime", "Public key used to verify KMS high-water-mark records."),
    "AIMEE_VAULT_KMS_KEY_ID": ("Server runtime", "External KMS key identifier used for vault wrapping."),
    "AIMEE_VAULT_PKCS11_LABEL": ("Server runtime", "PKCS#11 object label used for vault custody."),
    "AIMEE_VAULT_PKCS11_MODULE": ("Server runtime", "Path to the PKCS#11 provider module."),
    "AIMEE_VAULT_PKCS11_PIN": ("Server runtime", "PKCS#11 user PIN accepted only as first-boot transport; it is synchronously sealed into Vault, scrubbed from the environment, and loaded from Vault only when the HSM session opens."),
    "AIMEE_VAULT_PKCS11_SLOT": ("Server runtime", "PKCS#11 slot identifier used for vault custody."),
    "AIMEE_VAULT_TPM2_BLOB_PATH": ("Server runtime", "Path to the sealed TPM 2 vault-key blob."),
    "AIMEE_VAULT_TPM2_NV_INDEX": ("Server runtime", "TPM 2 NV index used for anti-rollback state."),
    "AIMEE_VAULT_TPM2_TCTI": ("Server runtime", "TPM 2 TCTI selector."),
    "AIMEE_WFE_ENGINE": ("Workflow engine", "Workflow runtime selector; current server images require `go`."),
    "AIMEE_WFE_HTTP_SOCKET": ("Workflow engine", "Unix socket for the Go workflow control plane."),
    "AIMEE_WFE_WORKTREE_GC_GRACE_SECS": ("Workflow engine", "Grace period before an unowned workflow worktree can be collected."),
    "AIMEE_WITNESS_CADENCE_TEST_S": ("Diagnostics & misc", "Test-only override that shortens witness checkpoint and verification cadence."),
    "AIMEE_WITNESS_HARNESS_ROLE": ("Diagnostics & misc", "Restricted PostgreSQL role used by the witness live harness."),
    "AIMEE_WORKFLOW_AUTONOMOUS_ROUTER": ("Workflow engine", "Enable automatic scheduling of admitted autonomous work items."),
    "AIMEE_WORKFLOW_BRANCH": ("Workflow engine", "Explicit workflow feature branch for a compatibility or test runner."),
    "AIMEE_WORKFLOW_ENFORCE_STAGE": ("Workflow engine", "Require runner requests to match the persisted workflow stage."),
    "AIMEE_WORKFLOW_LEASE_TTL_SECS": ("Workflow engine", "Lifetime of a workflow execution lease before recovery may reclaim it."),
}


def parse_env_vars():
    """Every AIMEE_* env var read outside src/tests/ (test-only vars excluded)."""
    found = set()
    for f in sorted(SRC.rglob("*")):
        if f.suffix not in (".c", ".h", ".inc") or "/tests/" in f.as_posix():
            continue
        text = f.read_text(encoding="utf-8", errors="ignore")
        for m in ENV_RE.finditer(text):
            found.add(m.group(1))
        for m in ENV_BY_NAME_RE.finditer(text):
            found.add(m.group(1))
    return found | ENV_DYNAMIC


def render_env(found):
    out = ["## Environment variables",
           "",
           f"The binaries read {len(found)} `AIMEE_*` environment variables (scanned "
           "from `getenv()` in `src/`, excluding tests, plus the generic first-boot "
           "credential inputs). Depending on the setting, these "
           "variables either override config-store values or provide fallbacks when no "
           "explicit config value is present. Module-activation variables use fallback "
           "semantics; deployment and runtime wiring variables commonly override stored "
           "values. A credential may enter through an environment variable only as "
           "first-boot transport (for example, a Kubernetes Secret): startup seals it "
           "into Vault, scrubs the environment, verifies custody, and fails closed before "
           "any long-lived service starts. Credentials are never runtime environment or "
           "config-file storage.",
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
        out.append("> These are read by the code but have no description yet: the "
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
    "LISTEN_FDS", "LISTEN_PID",
}
# provider keys resolved via per-agent api_key_env (not getenv literals): added so
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
    "SYNTHESIS_API_KEY": ("Provider credentials", "Bearer credential used by the generic llm-chat sidecar; prefer the vault or a secret command."),
    "SYNTHESIS_ENDPOINT": ("Provider endpoints", "OpenAI-compatible base URL used by the generic llm-chat sidecar."),
    "SYNTHESIS_MODEL": ("Provider endpoints", "Model requested by the generic llm-chat sidecar."),
    "SYNTHESIS_CA_FILE": ("Provider endpoints", "CA that verifies the synthesis sidecar's certificate on the kb -> aimee-llm hop. REPLACES the system trust store for that endpoint, so set it only for a sidecar the kb's own CA issued."),
    "SYNTHESIS_CERT_FILE": ("Provider endpoints", "Client certificate the kb presents to the synthesis sidecar, whose terminator requires one. Offered only to the host:port `SYNTHESIS_ENDPOINT` names."),
    "SYNTHESIS_KEY_FILE": ("Provider endpoints", "Private key for `SYNTHESIS_CERT_FILE`."),
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
           "`api_key_env`). Provider API keys are credentials: their environment variable "
           "names are overridable per agent, but values are accepted only as first-boot "
           "transport, sealed into Vault, and scrubbed before services start. Standard OS variables (`HOME`, "
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


# ─── Workflow engine config (src/modules/workflows/) ───────────────────────────────────

def parse_block_catalog():
    text = (ROOT / "server-go" / "internal" / "wfe" / "catalog.go").read_text(
        encoding="utf-8")
    start = text.index("var BuiltinBlocks = []BlockDefinition{")
    body = text[start:text.index("\n}", start)]
    cat = []
    for line in body.splitlines():
        name = re.search(r'Name:\s*"([^"]+)"', line)
        if not name:
            continue
        produces = re.search(r'Produces:\s*"([^"]+)"', line)
        accepts = re.search(r'Accepts:\s*\[\]string\{([^}]*)\}', line)
        ports = re.search(r'InputPorts:\s*\[\]string\{([^}]*)\}', line)
        required = re.search(r'RequiredPorts:\s*\[\]string\{([^}]*)\}', line)
        required_params = re.search(r'RequiredParams:\s*\[\]string\{([^}]*)\}', line)

        def strings(match):
            return re.findall(r'"([^"]+)"', match.group(1)) if match else []

        cat.append({
            "name": name.group(1),
            "produces": produces.group(1) if produces else "none",
            "accepts": strings(accepts),
            "ports": strings(ports),
            "required": strings(required),
            "required_params": strings(required_params),
            "requires_input": "RequiresInput: true" in line,
        })
    return cat


def parse_engine_default_rounds():
    engine = (ROOT / "server-go" / "internal" / "engine" / "engine.go").read_text(
        encoding="utf-8")
    rounds = re.search(r'const\s+defaultMax\s*=\s*(\d+)', engine)
    return rounds.group(1) if rounds else "?"


def render_workflow(catalog, default_rounds):
    out = ["## Workflow engine",
           "",
           "Workflows are block-composed YAML definitions under "
           "`$AIMEE_HOME/workflows/<name>.yaml`, authored with the `aimee workflow` "
           "CLI or the web visual composer and saved in canonical form. The Go "
           "workflow engine owns execution. A run is a durable work item pinned to "
           "a definition version.",
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
           "| Block | Required input ports and accepted artifacts | Produces |",
           "|-------|---------------------------------------------|----------|"]
    for block in catalog:
        required = set(block["required"])
        if not block["ports"]:
            inputs = "none"
        else:
            if block["requires_input"] and not required:
                port_names = [" or ".join(f"`{p}`" for p in block["ports"])
                              + " (one required)"]
            else:
                port_names = [f"`{p}`{' (required)' if p in required else ' (optional)'}"
                              for p in block["ports"]]
            accepts = ", ".join(f"`{a}`" for a in block["accepts"])
            inputs = f"{', '.join(port_names)}; accepts {accepts}"
        if block["required_params"]:
            inputs += "; requires param " + ", ".join(
                f"`{p}`" for p in block["required_params"])
        out.append(f"| `{block['name']}` | {inputs} | `{block['produces']}` |")
    out += [
        "",
        "### Block parameters (`params:`)",
        "",
        "- **Review panels:** `gate.roundtable` requires `roundtable`. Its optional "
        "`panel.required`, `panel.eligible`, and `quorum` fields select the seats. "
        "Quorum must be between one and the configured persona count.",
        "- **Human gates:** `gate.human` parks until the browser or API records an "
        "approve or reject decision. The current record is a hashed approval artifact "
        "and lifecycle transition, not a cryptographic principal signature.",
        "- **Loop budgets:** `max_rounds` limits repeated execution of one node. "
        "Blocks also read parameters such as `workflow`, `max_children`, `base`, "
        "`persona`, `focus`, and trigger workspace settings.",
        "",
        "### Custom blocks: `$AIMEE_HOME/workflows/blocks.yaml`",
        "",
        "Operator-owned and refused if it is a symlink or group/world-writable. "
        "It adds blocks to the catalog above:",
        "",
        "```yaml",
        "allow_command: false       # opt-in gate for command blocks",
        "command_timeout_ms: 60000  # bounded timeout for command blocks",
        "blocks:",
        "  - name: <block-name>     # must not shadow a built-in or duplicate",
        "    consumes: <artifact>   # input artifact type, or none (a source)",
        "    produces: branch|none  # custom blocks cannot mint verdict/approval/pr",
        "    executor: command|delegate",
        "    command: [ /abs/path/to/tool, arg1, ... ]  # command executor, no shell",
        "    command_sha256: <hex>  # digest of the executable",
        "    persona: <name>        # executor: delegate",
        "    prompt: <text>         # executor: delegate",
        "```",
        "",
        "### Run-level controls (not in the definition)",
        "",
        "- **Per-node loop cap**: `params.max_rounds` bounds retries for a node "
        f"that loops through `on_fail` (default `{default_rounds}`). Exhaustion parks "
        "the run with `retry_limit` or a more specific convergence reason. The "
        "retired `max_iters` and `on_max` fields are ignored by the Go engine.",
        "- **Cost cap**: an optional per-work-item USD ceiling is set when the run is "
        "created. The engine parks the run when cumulative cost reaches it.",
        "- **Trigger mode**: `interactive` or `autonomous` is recorded at admission. "
        "The current Go scheduler advances both the same way, so use `gate.human` or "
        "manual pause for an approval boundary.",
        "",
        "### Workflow environment overrides",
        "",
        "`AIMEE_WFE_RUNNER_URL` and `AIMEE_WFE_RUNNER_SOCKET` select a compatibility "
        "runner. `AIMEE_AUTONOMY_CONCURRENCY` supplies the startup fallback for global "
        "scheduler concurrency; live `autonomy.*` configuration then controls the "
        "running service. Legacy C variables are identified in the environment table.",
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
    "default_delegate": "Delegate this agent hands work to when a task arrives with no "
    "explicit delegate. Lets a project pin its own worker without every caller passing "
    "--delegate.",
    "endpoint": "Provider endpoint URL.",
    "backend": "Execution backend (http / cli / ssh / docker).",
    "api_key": "Inline API key (prefer `api_key_env` or the vault).",
    "api_key_env": "Env var name holding the agent's API key.",
    "access_token": "Static auth token for the endpoint.",
    "refresh_token": "OAuth refresh token for the endpoint.",
    "exp": "OAuth token expiry as a Unix timestamp.",
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
    "price_in_per_mtok": "Input price, USD per million tokens. Overrides the catalog price.",
    "price_out_per_mtok": "Output price, USD per million tokens. Overrides the catalog price.",
    "price_cached_per_mtok": "Cached-input price, USD per million tokens. Overrides the catalog price.",
    "tier_price_exempt": "Reason this agent is exempt from the `cost_tier`-vs-price lint (e.g. a flat-rate seat whose per-token price is not meaningful).",
    "catalog_provider": "Catalog vendor key used for model lookups (`anthropic`, `openai`, …), when it differs from the wire `provider`.",
    "registration": "Name of the provider registration this agent was expanded from. Set automatically; used to prefer same-provider peers during fallback.",
    "models": "Provider-general registration: the models to expand into individual routable agents. Omit to expand every routable model the catalog lists.",
    "max_scope": "Largest task scope this agent may be given (`bounded` or `whole_task`). Routing never relaxes this.",
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
    "is_server_hosted": "Whether the provider session is hosted by the aimee server.",
    "primary_only": "Restrict this agent to primary sessions; do not use it for delegates.",
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
    # DELIBERATELY the routing module only. An agent object is also parsed by
    # src/modules/vault/agent_credentials.c, which reads the credential-bearing
    # fields -- and the vault is an attack surface, so generated public docs do
    # not enumerate what it holds or name the file that reads it. Operators who
    # need those field names have `aimee agent setup`, which prompts for them.
    f = SRC / "modules" / "routing" / "agent_config.c"
    if not f.exists():
        return set()
    return set(AGENT_FIELD_RE.findall(f.read_text(encoding="utf-8")))


def render_config_files(agent_fields):
    out = ["## Other configuration files",
           "",
           "Beyond the config store, aimee reads a few standalone JSON/policy files "
           "(paths under `$AIMEE_HOME` unless an env override is set).",
           "",
           "### `agents.json`: agent / model definitions",
           "",
           "`{\"default_agent\": \"<name>\", \"agents\": [ {<agent>}, … ]}`. Each agent "
           "object's non-credential fields (credential fields are vault-held and "
           "deliberately not enumerated here):",
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
        "### Toolsets: `AIMEE_TOOLSETS_CONFIG` (or the config `toolsets` map)",
        "",
        "Named tool allowlists. `{\"toolsets\": {\"<name>\": { … }}}`; each toolset:",
        "",
        "- `tools` / `allowed_tools`: the tool names the set permits.",
        "- `include`: inherit another toolset's tools.",
        "- `script`: script-tool configuration for the set.",
        "",
        "### Guardrails: `AIMEE_GUARDRAILS_PATH`",
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
        "covers the scalar/keyed config surface but has known blind spots. They are listed "
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
        "- **Separate config files**: `agents.json`, toolsets, guardrails, and "
        "custom workflow blocks (`blocks.yaml`) / workflow definitions are documented "
        "in their own sections above. Per-agent field set is scanned from "
        "`agent_config.c`; the guardrails *policy* is behavioral (path classification "
        "+ pre-tool enforcement), with its tunables exposed as config keys.",
        "",
        "If the scan ever finds a config var with no description, it is emitted under "
        "an **Undocumented** heading in the relevant section, so a new option cannot "
        "silently bypass this reference.",
    ]).rstrip() + "\n"


def _require(what: str, rows, source: str):
    """Refuse to emit a document a parser came back empty for.

    Every extractor here scrapes C source with a regex, so a rename on the C
    side does not break the parse -- it silently matches NOTHING, and the
    generator writes a hollow document and exits 0. That is exactly what
    happened when the CLI command rows moved and their tier enum was renamed
    from CLIENT_TIER_ to AIMEE_CMD_TIER_: docs/gen/cli-commands.md went from 812
    lines to 8 with no error, and docs-gen-check would have reported it as an
    ordinary content change because all it does is diff the output against git.

    An empty extraction is never a legitimate state for any of these tables, so
    treat it as the failure it is and name the source file, which is where the
    rename will be.
    """
    if not rows:
        raise SystemExit(
            f"gen-reference-docs: FAIL - parsed 0 {what} from {source}. "
            "The extractor matched nothing, which usually means the C side was "
            "renamed. Fix the pattern rather than committing an empty document."
        )
    return rows



def _plant_test():
    """Prove the generator REFUSES a source it cannot parse.

    Every extractor here scrapes C with a regex, so a rename on the C side does
    not break the parse -- it matches nothing, and the generator writes a hollow
    document and exits 0. That is not hypothetical: when the CLI rows moved and
    their tier enum was renamed, docs/gen/cli-commands.md went from 812 lines to
    8 with no error, and docs-gen-check would have reported it as an ordinary
    content change.

    Plant that exact regression -- rename the tier token -- and require a
    failure.
    """
    global _CLI_ROWS_TEXT
    real = (SRC / "server" / "cli_command_defs_data.h").read_text(encoding="utf-8")
    _CLI_ROWS_TEXT = real.replace("AIMEE_CMD_TIER_", "PLANTED_RENAMED_TIER_")
    try:
        _require("CLI commands", parse_cli_commands(), "planted source")
    except SystemExit:
        print("gen-reference-docs: plant-test ok (unparseable source refused)")
        return 0
    finally:
        _CLI_ROWS_TEXT = None
    print("gen-reference-docs: PLANT FAIL - a source that parsed to nothing did NOT "
          "fail; the generator would emit an empty document", file=sys.stderr)
    return 1


def main():
    if "--plant-test" in sys.argv:
        return _plant_test()
    check = "--check" in sys.argv
    GEN.mkdir(parents=True, exist_ok=True)
    cli = render_cli(_require("CLI commands", parse_cli_commands(),
                              "src/server/cli_command_defs_data.h"))
    fields = _require("config fields", parse_config_fields(), "external config module")
    sections, flat = parse_config_sections()
    _require("config sections", sections, "external config module")
    # a key that is a CLI-settable scalar (or a section name) is not also a stray
    # "other top-level" key: subtract both so nothing is double-listed.
    flat = flat - {k for k, _, _ in fields} - set(sections)
    cfg = render_config(fields, sections, flat)
    cfg = (cfg.rstrip() + "\n\n"
           + render_env(_require("env vars", parse_env_vars(), "src/")).rstrip() + "\n\n"
           + render_external_env(_require("external env vars", parse_external_env(),
                                          "src/")).rstrip() + "\n\n"
           + render_workflow(_require("workflow blocks", parse_block_catalog(), "src/"),
                             parse_engine_default_rounds()).rstrip() + "\n\n"
           + render_config_files(_require("agent fields", parse_agent_fields(),
                                          "src/")).rstrip() + "\n\n"
           + render_limitations())
    targets = {GEN / "cli-commands.md": cli, GEN / "configuration.md": cfg}

    if check:
        stale = [p.name for p, want in targets.items()
                 if not p.exists() or p.read_text(encoding="utf-8") != want]
        if stale:
            print(f"gen-reference-docs: STALE: run scripts/gen-reference-docs.py: {stale}")
            return 1
        print("gen-reference-docs: ok (cli-commands.md, configuration.md in sync)")
        return 0

    for p, want in targets.items():
        p.write_text(want, encoding="utf-8")
        print(f"gen-reference-docs: wrote {p.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
