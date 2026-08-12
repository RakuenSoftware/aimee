#!/usr/bin/env python3
"""An optional module's tool-ownership table must name tools that actually exist.

A gate, not a report. When an optional module is disabled, the server withholds
the tools that module owns -- but only the ones its ownership table NAMES. A name
that no longer matches a served tool silently turns the guard off for exactly the
tool it exists to guard, and nothing errors: the tool stays on tools/list, the
agent calls it, and the call comes back with a pause_reason instead of a result.

That is not hypothetical. roundtable's table listed "ensemble_review" long after
the served tool became "roundtable_review" (the old name survives only in file
names). With the module disabled the benchmark agent called roundtable_review
twice per cell, every cell, and got
{"status":"pending","pause_reason":"panel_unreachable"} -- two wasted round trips
per cell for a call that could never succeed. Every other part of the filtering
machinery was correct.

Checked as a gate rather than a unit test because the failure is a MISMATCH
BETWEEN TWO FILES that each look fine alone, which is the shape a rename produces
and the shape a single-file test cannot see.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OWNERSHIP = ROOT / "src/modules/roundtable/roundtable_activation.c"
REGISTRY = ROOT / "src/modules/protocols/mcp/mcp_tools.c"
EXTENDED = ROOT / "src/modules/protocols/mcp/mcp_tools_extended.c"
PROFILE = ROOT / "src/modules/protocols/mcp/mcp_tool_profile.c"


def read(path):
    return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""


def owned_exact_names():
    """The exact[] entries of every *_tool_available ownership table.

    Prefix tables are deliberately NOT checked: a prefix ("pipeline_") is a
    family pattern, not a claim that a specific tool exists."""
    text = read(OWNERSHIP)
    names = {}
    for fn, body in re.findall(r"int (\w*tool_available)\(const char \*\w+\)\s*\{(.*?)\n\}",
                               text, re.S):
        m = re.search(r"exact\[\]\s*=\s*\{(.*?)\}", body, re.S)
        if m:
            for name in re.findall(r'"([^"]+)"', m.group(1)):
                names.setdefault(name, fn)
    return names


def served_tool_names():
    """Every tool name the MCP surface can serve.

    Union of the three files that spell tool names, because a tool may be
    registered in one and only referenced in the others. A name found in ANY of
    them is real; the gate only fires when a name is in NONE."""
    names = set()
    for path in (REGISTRY, EXTENDED, PROFILE):
        names |= set(re.findall(r'"([a-z][a-z0-9_]{2,})"', read(path)))
    return names


def main():
    owned = owned_exact_names()
    if not owned:
        print("check_optional_tool_ownership: could not parse any ownership table",
              file=sys.stderr)
        return 1
    served = served_tool_names()

    missing = sorted((name, fn) for name, fn in owned.items() if name not in served)
    print(f"optional-tool ownership: {len(owned)} owned name(s) checked against "
          f"{len(served)} served tool name(s)")
    if not missing:
        print("check_optional_tool_ownership: ok (every owned tool name is a real tool)")
        return 0

    print("check_optional_tool_ownership: FAILED", file=sys.stderr)
    for name, fn in missing:
        print(f"  {fn}() claims to own '{name}', which is not a served tool name.",
              file=sys.stderr)
    print("\nAn ownership entry that names nothing withholds nothing: when the module is\n"
          "disabled the tool stays on tools/list and every call to it fails. Rename the\n"
          "entry to the served tool, or drop it if the tool is gone.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
