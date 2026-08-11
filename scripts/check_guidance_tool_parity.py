#!/usr/bin/env python3
"""Every tool the standing guidance names must be on the surface an agent is SHOWN.

This is a gate, not a report, because it is decidable and it has exactly one
correct answer: guidance that names a tool the agent cannot see is advice it
cannot follow.

It did not hold. aimee_session_guidance.h named lsp_references, get_context_block
and memory_get; all three are registered in mcp_tool_table but none is in
MCP_CORE_TOOLS, so no external MCP client was shown them. Reaching one costs
find_tools -> describe_tool -> call_tool, and mcp_tool_profile.c already records
that agents will not pay that: "A tool the agent cannot afford to reach is a tool
it does not have." Measured consequence: a gateway benchmark cell used aimee for
nothing -- zero MCP calls, zero CLI calls -- and did all eight of its steps with
find/cat/sed/grep.

Two things named "core" is what hid it. get_context_block is marked
native="core,review_indexed" in mcp_tool_table (aimee's own agents' toolset) while
missing from MCP_CORE_TOOLS (what an external client sees). Passing one and
failing the other reads as fine at a glance.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GUIDANCE = ROOT / "src/headers/aimee_session_guidance.h"
PROFILE = ROOT / "src/modules/protocols/mcp/mcp_tool_profile.c"
CAPS = ROOT / "src/headers/agent_code_capabilities.h"


def read(p):
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def resolve_macros(text, caps, tools_only=False):
    """Expand AIMEE_CODE_* macros to the literal names they define.

    tools_only restricts to AIMEE_CODE_TOOL_*. The others name command ARGUMENTS --
    AIMEE_CODE_INDEX_COMMAND_HYBRID is "hybrid", an argument to `index`, not a tool
    -- and counting one as a tool reports a failure that is not one."""
    out = set()
    pattern = r"(AIMEE_CODE_TOOL_[A-Z_]+)" if tools_only else r"(AIMEE_CODE_[A-Z_]+)"
    for macro in re.findall(pattern, text):
        m = re.search(rf'#define\s+{macro}\s+"([A-Za-z0-9_=]+)"', caps)
        if m:
            out.add(m.group(1))
    return out


def shown_tools():
    s = read(PROFILE)
    start = s.find("MCP_CORE_TOOLS[] = {")
    if start == -1:
        sys.exit("check_guidance_tool_parity: MCP_CORE_TOOLS not found")
    body = s[start:s.find("\n};", start)]
    names = set(re.findall(r'"([A-Za-z0-9_]+)"', body))
    return names | resolve_macros(body, read(CAPS))


def guidance_tools(shown):
    s = read(GUIDANCE)
    start = s.find("#define AIMEE_GUIDANCE_EXPLORE_WITH_LINE")
    end = s.find("#define AIMEE_GUIDANCE_BLOCK")
    if start == -1 or end == -1:
        sys.exit("check_guidance_tool_parity: guidance macros not found")
    # Only the string literals, so the explanatory comments above each macro do not
    # count as advice -- they legitimately NAME the tools that were wrong.
    block = "\n".join(ln for ln in s[start:end].splitlines()
                      if not ln.lstrip().startswith(("*", "/*")))
    names = resolve_macros(block, read(CAPS), tools_only=True)
    for lit in re.findall(r'"([^"]*)"', block):
        for word in re.findall(r"\b([a-z][a-z0-9_]{3,})\b", lit):
            # A word is a tool reference if it is shown, or if it looks like one of
            # the known tool spellings; ordinary prose words are not.
            if word in shown or word.count("_") >= 1:
                names.add(word)
    return names


def main():
    shown = shown_tools()
    named = guidance_tools(shown)
    missing = sorted(n for n in named if n not in shown)

    print(f"guidance names {len(named)} tool(s); {len(shown)} are shown in tools/list")
    if not missing:
        print("check_guidance_tool_parity: ok (every named tool is on the shown surface)")
        return 0
    print("check_guidance_tool_parity: FAILED", file=sys.stderr)
    for n in missing:
        print(f"  '{n}' is named in the guidance but is NOT in MCP_CORE_TOOLS", file=sys.stderr)
    print("\nEither add it to MCP_CORE_TOOLS -- a deliberate change to what aimee presents\n"
          "by default -- or stop naming it. Do not leave the agent advice it cannot follow.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
