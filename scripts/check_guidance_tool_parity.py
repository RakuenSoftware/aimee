#!/usr/bin/env python3
"""Every tool the standing guidance names must be CALLABLE AS WRITTEN.

A gate, not a report: guidance naming something an agent cannot call is advice it
cannot follow, and there is exactly one correct answer.

WHY THIS CHECKS CALLABILITY AND NOT LIST MEMBERSHIP. The first version of this
script compared the guidance against MCP_CORE_TOOLS -- one hand-maintained list
against another -- and that is not the same question. Proof, from trying it:
adding "get_context_block" to MCP_CORE_TOOLS made this check pass while changing
nothing an agent sees, because there is no flat tool by that name at all. It is a
FAMILY MEMBER, reachable only as `recall command=context_block`. The gate written
to prevent "guidance names an uncallable tool" would have waved through exactly
that bug.

So the callable set is:
  - every tool in MCP_CORE_TOOLS (shown in tools/list), and
  - nothing else. A family MEMBER is not callable by its own name; the family is.

When a named tool turns out to be a family member, the failure says how to write
it instead, because "not callable" is unhelpful when the capability exists and
only the spelling is wrong -- which was the actual situation for
get_context_block, memory_get and lsp_references.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GUIDANCE = ROOT / "src/headers/aimee_session_guidance.h"
PROFILE = ROOT / "src/modules/protocols/mcp/mcp_tool_profile.c"
FAMILIES = ROOT / "src/modules/protocols/mcp/mcp_tools_extended.c"
CAPS = ROOT / "src/headers/agent_code_capabilities.h"


def read(p):
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def resolve_macros(text, caps, tools_only=False):
    """Expand AIMEE_CODE_* macros to the literals they define.

    tools_only restricts to AIMEE_CODE_TOOL_*: the others name command ARGUMENTS
    (AIMEE_CODE_INDEX_COMMAND_HYBRID is "hybrid", an argument to index, not a
    tool), and counting one as a tool reports a failure that is not one."""
    out = set()
    pattern = r"(AIMEE_CODE_TOOL_[A-Z_]+)" if tools_only else r"(AIMEE_CODE_[A-Z_]+)"
    for macro in re.findall(pattern, text):
        m = re.search(rf'#define\s+{macro}\s+"([A-Za-z0-9_=]+)"', caps)
        if m:
            out.add(m.group(1))
    return out


def shown_tools():
    """MCP_CORE_TOOLS -- what an external MCP client is SHOWN in tools/list."""
    s = read(PROFILE)
    start = s.find("MCP_CORE_TOOLS[] = {")
    if start == -1:
        sys.exit("check_guidance_tool_parity: MCP_CORE_TOOLS not found")
    body = s[start:s.find("\n};", start)]
    return set(re.findall(r'"([A-Za-z0-9_]+)"', body)) | resolve_macros(body, read(CAPS))


def family_members():
    """member tool name -> "family command=value" that reaches it.

    The table is {family, param, description, {{value, tool}, ...}}, so a member
    is reachable only through its family. Parsed rather than hardcoded: a list of
    families kept by hand here would be one more thing to drift."""
    s = read(FAMILIES)
    out = {}
    for fam, block in re.findall(
            r'\{"([a-z_]+)",\s*"command",\s*"(?:[^"\\]|\\.)*",\s*\{(.*?)\{NULL, NULL\}\}\}',
            s, re.S):
        for value, tool in re.findall(r'\{"([a-z_]+)",\s*"([a-z_]+)"\}', block):
            out.setdefault(tool, f"{fam} command={value}")
    return out


def guidance_tools(shown):
    s = read(GUIDANCE)
    start = s.find("#define AIMEE_GUIDANCE_EXPLORE_WITH_LINE")
    end = s.find("#define AIMEE_GUIDANCE_BLOCK")
    if start == -1 or end == -1:
        sys.exit("check_guidance_tool_parity: guidance macros not found")
    # String literals only: the explanatory comments legitimately NAME the tools
    # that were wrong, and must not be read as advice.
    block = "\n".join(ln for ln in s[start:end].splitlines()
                      if not ln.lstrip().startswith(("*", "/*")))
    names = resolve_macros(block, read(CAPS), tools_only=True)
    for lit in re.findall(r'"([^"]*)"', block):
        for word in re.findall(r"\b([a-z][a-z0-9_]{3,})\b", lit):
            if word in shown or word.count("_") >= 1:
                names.add(word)
    return names


def main():
    listed = shown_tools()
    families = family_members()
    # A family member is FOLDED OUT of the flat tools/list, so listing it in
    # MCP_CORE_TOOLS does not make it callable -- the family is the tool. Putting
    # "get_context_block" in that list and naming it in the guidance is exactly the
    # slip an earlier version of this gate waved through: both lists agreed, and an
    # agent still had nothing by that name to call. Callable means IN THE FLAT LIST.
    shown = listed - set(families)
    named = guidance_tools(shown)
    bad = sorted(n for n in named if n not in shown)

    print(f"guidance names {len(named)} tool(s); {len(shown)} callable by name, "
          f"{len(families)} reachable only through a family")
    if not bad:
        print("check_guidance_tool_parity: ok (every named tool is callable as written)")
        return 0

    print("check_guidance_tool_parity: FAILED", file=sys.stderr)
    for n in bad:
        if n in families:
            print(f"  '{n}' is NOT callable by that name -- it is a family member. "
                  f"Write it as '{families[n]}'.", file=sys.stderr)
        else:
            print(f"  '{n}' is named in the guidance but is not shown in tools/list "
                  f"and is not reachable through a family.", file=sys.stderr)
    print("\nAn agent cannot follow advice it cannot act on. Either name the callable\n"
          "form, or add the tool to MCP_CORE_TOOLS -- a deliberate change to what aimee\n"
          "presents by default.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
