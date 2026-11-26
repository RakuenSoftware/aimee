#!/usr/bin/env python3
"""The history gate's copy of the source-custody refusal equals the reply.

The MCP call table answers a source tool that issued no receipt with a fixed
reply, and the history custody gate admits exactly that reply without a
receipt (src/server/server_tool_source_unavailable.h). The call table is a
frozen LSP S1 file, so it keeps its own literal instead of including the
header. If the two ever differ, the gate refuses the reply again and one
failed memory_serve call ends the delegate with "failed to build request".
"""

from pathlib import Path
import ast
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "src/server/server_mcp_call_table.c"
HEADER = ROOT / "src/server/server_tool_source_unavailable.h"


def c_strings(fragment: str) -> str:
    """Concatenate the adjacent C string literals in a fragment."""
    pieces = re.findall(r'"((?:[^"\\\n]|\\.)*)"', fragment)
    if not pieces:
        raise AssertionError("no string literal found")
    return "".join(ast.literal_eval('"' + piece + '"') for piece in pieces)


class SourceCustodyRefusalText(unittest.TestCase):
    def test_gate_constant_equals_the_call_table_reply(self):
        table = TABLE.read_text()
        body = re.search(
            r"static cJSON \*mcph_source_unavailable\(struct mcp_call \*c\)\s*\{(.*?)\n\}",
            table, re.S)
        self.assertIsNotNone(body, "mcph_source_unavailable not found")
        reply = re.search(r"text_content\((.*?)\);", body.group(1), re.S)
        self.assertIsNotNone(reply, "the refusal reply is not a text_content literal")
        header = HEADER.read_text()
        define = re.search(
            r"#define AIMEE_TOOL_SOURCE_UNAVAILABLE_RESULT\s*\\\n(.*?)\n\n", header, re.S)
        self.assertIsNotNone(define, "AIMEE_TOOL_SOURCE_UNAVAILABLE_RESULT not found")
        self.assertEqual(c_strings(define.group(1)), c_strings(reply.group(1)))


if __name__ == "__main__":
    unittest.main()
