#!/usr/bin/env python3
"""check-native-tool-parity: stop aimee's own agents falling behind aimee's MCP surface.

aimee has two tool surfaces that grew apart:

  MCP    (server/server_mcp_call_table.c)  -> external clients: Claude Code, thin clients
  native (server/agent_tools.c)            -> aimee's OWN delegates and review panelists

Adding a tool to the MCP table gives it to everyone EXCEPT aimee's agents, silently.
That is not hypothetical. git_commit/git_push/git_pr were MCP-only, so the wfe
implement delegate's only route to land work was shelling out to `git` — the exact
thing require_aimee_git forbids. index_find_callers was MCP-only, so a review panel
asked "is this function still called?" had no tool that could answer and hedged
("unsafe absent a full audit of call sites") on a symbol with twelve callers one
query away. Both were found by running a delegate on hardware, months after the
drift, never by a test.

So: every MCP tool must either have a native equivalent, or be listed below as a
deliberate exception with a reason. New drift fails the build instead of waiting to
be discovered by an agent that quietly cannot do its job.

Usage:
  check-native-tool-parity.py --src-dir src
  check-native-tool-parity.py --src-dir src --plant-test   # prove it still catches one
"""
import argparse
import re
import sys
from pathlib import Path

# MCP tools that are deliberately NOT native, with the reason each is exempt.
# Adding a name here is a decision, not a formality: it asserts that aimee's own
# agents have no use for the capability. If an agent could plausibly want it, wire
# it natively instead.
EXEMPT = {
    # Session/gateway plumbing: about an external client's OWN session, meaningless
    # to an in-process agent that has no session to manage.
    "session_context_expand": "external client's session plumbing",
    "session_context_search": "external client's session plumbing",
    "session_context_status": "external client's session plumbing",
    "compact_context": "external client's context window, not the agent's",
    # Ensemble/delegate orchestration: an agent using these would spawn agents,
    # which the delegate-only rail exists to prevent.
}


# The drift that already existed when this check was written: 73 MCP tools
# aimee's own agents cannot call. This is a RATCHET, not an allowlist — the check
# fails if the list GROWS, and fails if an entry has quietly been fixed without
# being removed. Every name here is a capability aimee has and its agents lack;
# the list is meant to shrink to zero.
#
# Highest value first (what a delegate or reviewer actually needs):
#   index_blast_radius, ast_grep_search, search_graph, get_context_block,
#   index_structure, lsp_references, lsp_definition
# index_find_callers was the first removed — a review panel could not answer
# "is this still called?" without it.
BASELINE_DRIFT = {
    "advance_request",
    "autopilot",
    "code_span_get",
    "complete_prospective_memory",
    "create_epistemic_directive",
    "create_prospective_memory",
    "dashboard_metrics",
    "get_entity",
    "get_entity_edges",
    "get_episode",
    "get_help",
    "get_host",
    "get_identity",
    "index_graph_audit",
    "index_graph_diff",
    "index_graph_hubs",
    "index_graph_node",
    "index_graph_surprising",
    "index_hybrid",
    "index_lessons",
    "job_start",
    "job_status",
    "learning_propose",
    "learning_review",
    "list_attempts",
    "list_curiosity_items",
    "list_epistemic_directives",
    "list_facts",
    "list_hosts",
    "list_prospective_memories",
    "lsp_definition",
    "lsp_diagnostics",
    "lsp_references",
    "memory_alerts",
    "memory_ask",
    "memory_briefing",
    "memory_explain_match",
    "memory_fact_history",
    "memory_get",
    "memory_maintain",
    "memory_provenance",
    "memory_recall",
    "mutate",
    "payload_rewrite_status",
    "pdf_inspect_structure",
    "pdf_list_assets",
    "pdf_lookup_table",
    "pdf_open_asset",
    "pdf_open_neighbors",
    "pdf_open_page",
    "pdf_search_chunks",
    "preview_blast_radius",
    "record_attempt",
    "resolve_epistemic_directive",
    "roadmap_list",
    "roadmap_show",
    "rules_list",
    "rules_propose",
    "session_search",
    "set_primary_agent",
    "skill_manage",
    "store_workflow",
    "task_list",
    "upsert_persona",
    "upsert_role_template",
    "work_board",
    "work_list",
    "workflow_run",
}


def mcp_tools(src: Path) -> set:
    """Tool names in the MCP dispatch table: {"name", mcph_handler, native}."""
    f = src / "server" / "server_mcp_call_table.c"
    return set(re.findall(r'\{"([a-z_0-9]+)",\s*mcph_[a-z_0-9]+,', f.read_text()))


def mcp_derived_tools(src: Path) -> set:
    """MCP tools marked native: {"name", mcph_handler, "core,review_indexed"}.

    These need no separate native declaration — the server registers them from the
    MCP table at startup and the advert, schema, dispatch and toolset membership all
    derive from that one row. This is the merged surface; the sets below are the
    tools that predate it.
    """
    f = src / "server" / "server_mcp_call_table.c"
    return set(re.findall(r'\{"([a-z_0-9]+)",\s*mcph_[a-z_0-9]+,\s*"[a-z_,]+"\}', f.read_text()))


def native_tools(src: Path) -> set:
    """Everything aimee's own agents can call.

    Two sources, because the merge is incremental: tools still hand-declared in the
    native builtin table, plus tools derived from the MCP table's native column. The
    hand-declared set should shrink to the genuinely native-only tools (bash,
    read_file, edit_file — things no MCP client needs from aimee).
    """
    f = src / "server" / "agent_tools.c"
    body = f.read_text()
    m = re.search(r"g_builtin_tools\[\] = \{(.*?)\n\};", body, re.S)
    if not m:
        sys.exit("check-native-tool-parity: FAIL - could not find g_builtin_tools[]")
    hand_written = set(re.findall(r'\{"([a-z_0-9]+)",', m.group(1)))
    return hand_written | mcp_derived_tools(src)


def known_tools(src: Path) -> set:
    """The toolset name allowlist. A native tool missing here is pruned at runtime."""
    body = (src / "toolset.c").read_text()
    m = re.search(r"KNOWN_TOOLS\[\] = \{(.*?)NULL,?\s*\};", body, re.S)
    if not m:
        sys.exit("check-native-tool-parity: FAIL - could not find KNOWN_TOOLS[]")
    return set(re.findall(r'"([a-z_0-9]+)"', m.group(1)))


def check(src: Path, extra_mcp=None) -> int:
    mcp = mcp_tools(src) | set(extra_mcp or [])
    native = native_tools(src)
    known = known_tools(src)
    status = 0

    # NEW drift: an MCP tool that is neither native, nor exempt, nor pre-existing.
    missing = sorted(mcp - native - set(EXEMPT) - BASELINE_DRIFT)
    if missing:
        status = 1
        print(
            "check-native-tool-parity: FAIL - NEW MCP tools with no native equivalent.\n"
            "  External clients can call these; aimee's own delegates and review\n"
            "  panelists cannot. Pick one:\n"
            "    - add a native tool (see find_callers: builtin table + dispatch +\n"
            "      KNOWN_TOOLS + a toolset — all four, or it is silently uncallable);\n"
            "    - add the name to EXEMPT with the reason it is external-only.\n"
            "  Do NOT add it to BASELINE_DRIFT: that list records old debt and only shrinks.",
            file=sys.stderr,
        )
        for t in missing:
            print(f"    {t}", file=sys.stderr)

    # The ratchet: a baselined tool that is now native must leave the baseline, or
    # the list rots into a permanent excuse and stops meaning anything.
    fixed = sorted(BASELINE_DRIFT & native)
    if fixed:
        status = 1
        print(
            "\ncheck-native-tool-parity: FAIL - these are native now; remove them from\n"
            "  BASELINE_DRIFT in this script (the baseline only shrinks):",
            file=sys.stderr,
        )
        for t in fixed:
            print(f"    {t}", file=sys.stderr)

    gone = sorted(BASELINE_DRIFT - mcp - native)
    if gone:
        print(
            "check-native-tool-parity: note - BASELINE_DRIFT names no longer in the MCP "
            f"table (remove them): {', '.join(gone)}"
        )

    # A native tool absent from KNOWN_TOOLS is pruned from every toolset at
    # runtime with only a WARN — registered, advertised, and uncallable.
    # MCP-derived tools are exempt: they are not in the static KNOWN_TOOLS list by
    # design, because toolset_register_native_tool() adds them at startup. Needing
    # no entry here is the merge working — that hand-edit is one of the four that
    # had to agree, and the one that silently ate git_write's tools.
    unpruned = sorted(native - known - mcp_derived_tools(src))
    if unpruned:
        status = 1
        print(
            "\ncheck-native-tool-parity: FAIL - native tools missing from KNOWN_TOOLS.\n"
            "  toolset.c prunes any toolset entry naming an unknown tool, so a role\n"
            "  can never resolve these — they exist and cannot be called.",
            file=sys.stderr,
        )
        for t in unpruned:
            print(f"    {t}", file=sys.stderr)

    stale = sorted(set(EXEMPT) - mcp)
    if stale:
        print(
            "check-native-tool-parity: note - EXEMPT names no longer in the MCP table "
            f"(remove them): {', '.join(stale)}"
        )

    if status == 0:
        print(
            f"check-native-tool-parity: ok ({len(mcp)} MCP tools, {len(native)} native, "
            f"{len(EXEMPT)} exempt, {len(BASELINE_DRIFT)} pre-existing gaps to work off)"
        )
    return status


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src-dir", default="src")
    ap.add_argument(
        "--plant-test",
        action="store_true",
        help="inject a fake MCP-only tool and assert the check catches it",
    )
    args = ap.parse_args()
    src = Path(args.src_dir)

    if args.plant_test:
        # The check must FAIL on a planted MCP-only tool, or it is decoration.
        rc = check(src, extra_mcp=["planted_mcp_only_tool"])
        if rc == 0:
            print(
                "check-native-tool-parity: PLANT FAIL - a planted MCP-only tool was "
                "NOT caught; the check does not work",
                file=sys.stderr,
            )
            return 1
        print("check-native-tool-parity plant: ok (planted violation correctly detected)")
        return 0

    return check(src)


if __name__ == "__main__":
    sys.exit(main())
