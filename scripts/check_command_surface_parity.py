#!/usr/bin/env python3
"""Report where aimee's capability surfaces disagree.

Capability surface is declared independently in several hand-maintained tables.
Nothing derives one from another, so they drift, and the drift is invisible until
an agent asks for something that is not there. Found on 2026-08-11: `memory_get`
reachable from the CLI and registered for MCP dispatch but absent from the surface
an agent is SHOWN; `get_context_block` marked native="core,..." in one table while
missing from the other list called core; and standing guidance naming three tools
no external client could call.

This prints the divergence so the port onto src/command_registry.c has a work
list, and so new divergence is visible rather than latent.

Deliberately a REPORT, not yet a gate. Failing the build today would fail it on
~200 pre-existing divergences, which teaches everyone to pass --ignore. It becomes
a gate per-group as each group is ported: once `memory` routes from the registry,
`--require memory` fails on any memory command missing from a surface.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CLI = ROOT / "src/cli_v1_routes.c"
MCP_DISPATCH = ROOT / "src/server/server_mcp_call_table.c"
MCP_PROFILE = ROOT / "src/modules/protocols/mcp/mcp_tool_profile.c"
GUIDANCE = ROOT / "src/headers/aimee_session_guidance.h"


def read(p):
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def cli_routes():
    """rpc_routes[] entries: {"group", "verb", "group.verb", ...}."""
    s = read(CLI)
    start = s.find("} rpc_routes[] = {")
    if start == -1:
        return {}
    body = s[start:s.find("\n};", start)]
    out = {}
    for m in re.finditer(r'\{"([a-z0-9_]+)",\s*"([a-z0-9_]*)",\s*"([a-z0-9_.]+)"', body):
        group, verb, method = m.groups()
        out[method] = (group, verb)
    return out


def mcp_dispatch():
    """mcp_tool_table[] entries: tool name -> native toolset string (or None)."""
    s = read(MCP_DISPATCH)
    start = s.find("} mcp_tool_table[] = {")
    if start == -1:
        return {}
    body = s[start:s.find("\n};", start)]
    out = {}
    for m in re.finditer(r'\{"([A-Za-z0-9_]+)",\s*[A-Za-z0-9_]+,\s*(NULL|"[^"]*")', body):
        name, native = m.groups()
        out[name] = None if native == "NULL" else native.strip('"')
    return out


def mcp_core():
    """MCP_CORE_TOOLS[] -- what an external MCP client is SHOWN in tools/list."""
    s = read(MCP_PROFILE)
    start = s.find("MCP_CORE_TOOLS[] = {")
    if start == -1:
        return set()
    body = s[start:s.find("\n};", start)]
    names = set(re.findall(r'"([A-Za-z0-9_]+)"', body))
    # Entries written as macros resolve via agent_code_capabilities.h.
    caps = read(ROOT / "src/headers/agent_code_capabilities.h")
    for macro in re.findall(r"(AIMEE_CODE_[A-Z_]+)", body):
        mm = re.search(rf'#define\s+{macro}\s+"([A-Za-z0-9_=]+)"', caps)
        if mm:
            names.add(mm.group(1))
    return names


def guidance_names(core):
    """Tool names the standing guidance tells an agent to call."""
    s = read(GUIDANCE)
    block = s[s.find("#define AIMEE_GUIDANCE_EXPLORE_WITH_LINE"):]
    block = block[:block.find("#define AIMEE_GUIDANCE_BLOCK")] if block else ""
    names = set()
    caps = read(ROOT / "src/headers/agent_code_capabilities.h")
    for macro in re.findall(r"(AIMEE_CODE_TOOL_[A-Z_]+)", block):
        mm = re.search(rf'#define\s+{macro}\s+"([A-Za-z0-9_]+)"', caps)
        if mm:
            names.add(mm.group(1))
    # Bare literals: any lowercase identifier that is a known tool-ish word.
    for lit in re.findall(r"\b([a-z][a-z0-9_]{3,})\b", block):
        if lit in core or lit in ("lsp_references", "get_context_block", "memory_get",
                                  "memory_recall", "search_docs", "search_memory"):
            names.add(lit)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--require", action="append", default=[],
                    help="group that MUST be consistent; exits non-zero if not")
    args = ap.parse_args()

    cli = cli_routes()
    disp = mcp_dispatch()
    core = mcp_core()
    guide = guidance_names(core)

    print(f"CLI routes (rpc_routes[]):        {len(cli)}")
    print(f"MCP dispatch (mcp_tool_table[]):  {len(disp)}")
    print(f"MCP shown (MCP_CORE_TOOLS[]):     {len(core)}")
    print()

    # The defect that started this: guidance naming what an agent cannot see.
    unfollowable = sorted(n for n in guide if n not in core)
    print(f"guidance names not in the shown surface: {len(unfollowable)}")
    for n in unfollowable:
        where = "registered for dispatch" if n in disp else "NOT REGISTERED ANYWHERE"
        print(f"  {n:24s} {where}")
    print()

    # Dispatchable but never shown: reachable only via find_tools/describe_tool/
    # call_tool, which mcp_tool_profile.c records agents will not pay for.
    hidden = sorted(n for n in disp if n not in core)
    print(f"MCP tools registered but not shown: {len(hidden)}")
    print(f"  {', '.join(hidden[:12])}{' ...' if len(hidden) > 12 else ''}")
    print()

    # CLI groups whose verbs have no MCP presence at all, under either spelling.
    groups = {}
    for method, (group, verb) in cli.items():
        # Spellings actually in use are inconsistent -- memory.recall is
        # `memory_recall` but memory.search is `search_memory`, verb-first. A
        # mechanical mapping cannot tell "absent" from "spelled backwards", which
        # is itself the thing the registry fixes, so try both orders and do not
        # count a reversed hit as missing.
        flat = f"{group}_{verb}" if verb else group
        rev = f"{verb}_{group}" if verb else group
        seen = flat in disp or rev in disp or method.replace(".", "_") in disp or group in disp
        groups.setdefault(group, []).append((method, seen))
    missing_groups = {g: [m for m, s in v if not s] for g, v in groups.items()}
    missing_groups = {g: v for g, v in missing_groups.items() if v}
    total_missing = sum(len(v) for v in missing_groups.values())
    print(f"CLI routes with no MCP counterpart: {total_missing} across {len(missing_groups)} groups")
    for g in sorted(missing_groups)[:10]:
        print(f"  {g:16s} {len(missing_groups[g]):3d}  e.g. {missing_groups[g][0]}")

    rc = 0
    for group in args.require:
        bad = missing_groups.get(group, [])
        if bad:
            print(f"\nFAIL: group '{group}' is required consistent, but these have no MCP "
                  f"counterpart: {bad}")
            rc = 1
        else:
            print(f"\nok: group '{group}' is consistent across surfaces")
    return rc


if __name__ == "__main__":
    sys.exit(main())
