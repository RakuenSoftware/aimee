# Proposal: Context pre-injection and confidence-gated retrieval for the model ingresses

- **State:** done
- **Completed:** 2026-06-09
- **Author:** JBailes

## Shipped

The context pre-injection ingress work is implemented.

- `ingress_preinject_enabled` gates server-side context pre-injection for the
  model ingresses, and `ingress_preinject_assembly_budget` bounds the final
  envelope.
- The HTTP layer supports per-request disable with `x-aimee-preinject: 0`, which
  is used by benchmark and rollout workflows without changing the wire shape.
- OpenAI/Codex ingress paths call `ingress_preinject_build()` and prepend a
  compact `<aimee-context>` envelope through the existing instruction/system
  prompt seam when enabled.
- The envelope points follow-up exploration at Aimee surfaces:
  `find_symbol`, `lsp_references`, `ast_grep_search`, `search_graph`,
  `get_context_block`, and `memory_get`.
- `memory_get` is exposed as an MCP tool over the existing
  `kb_client_memory_get()` / `memory.get` chain, so preview handles are
  id-addressable.
- `find_symbol` now includes symbol body spans when known (`line-line_end`) so a
  `file::symbol` reference can be read at function granularity rather than whole
  file granularity.
- Claude/Codex setup extends the installed guidance to prefer Aimee indexed tools
  over raw grep/read for codebase exploration.
- Claude Code hook installation includes:
  - `UserPromptSubmit` for per-turn context injection.
  - `PreCompact` for post-compaction re-prime.
  - `PreToolUse` attention guard for read/edit/destructive tool calls.
- The attention guard keeps a compact per-session file attention log, blocks
  hard-destructive operations on high-attention files, honors `AIMEE_GUARD=0`,
  and redirects recursive raw scans toward Aimee's indexed tools.
- `ingress_max_raw_scans` is a configurable cap for raw recursive scan allowance;
  it defaults to `0` when Aimee MCP tools are available.
- `bench/ingress_token_bench.py` and `bench/ingress_prompts.txt` provide the
  Codex ingress token A/B harness.
- The graph-derived code-health audit split is shipped separately in
  `docs/proposals/done/code-health-audit.md`.

## Verification Notes

Verified in-tree evidence:

- `src/server/ingress_preinject.c`
- `src/server/openai_chat.c`
- `src/server/server_http.c`
- `src/server/server_mcp.c`
- `src/client_integrations.c`
- `src/cli_session_start.c`
- `src/cli_attention_guard.c`
- `src/headers/index.h`
- `bench/ingress_token_bench.py`
- `src/tests/test_ingress_preinject.c`
- `src/tests/test_attention_guard.c`
- `src/tests/test_mcp_tools_golden.inc`
