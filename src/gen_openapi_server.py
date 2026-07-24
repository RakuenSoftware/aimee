#!/usr/bin/env python3
"""Embed api/openapi-server-v1.yaml as a C string constant for the
aimee-server /v1/openapi.json and /v1/openapi.yaml endpoints. Run via
`make server/openapi_data.h`.

The constant AIMEE_OPENAPI_SERVER_YAML_STR holds the raw YAML text. The HTTP
handler in server_http.c serves it directly; clients that need JSON can convert
client-side (the spec states its format in the `openapi:` field). This mirrors
src/gen_openapi.py, which does the same for the aimee-kb surface.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # repo root
SPEC = ROOT / "api" / "openapi-server-v1.yaml"
OUT = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "src" / "server" / "openapi_server_data.h")

MARKER = re.compile(r"^\s*# @aimee-(if|endif) ([A-Z][A-Z0-9_]*)\s*$")


def feature_segments(text: str):
    """Yield literal YAML and C preprocessor directives from feature markers."""
    literal = []
    active = None
    for line in text.splitlines(keepends=True):
        match = MARKER.match(line)
        if not match:
            literal.append(line)
            continue
        if literal:
            yield "literal", "".join(literal)
            literal = []
        kind, feature = match.groups()
        if kind == "if":
            if active is not None:
                raise SystemExit("gen_openapi_server: nested feature marker")
            active = feature
            yield "directive", f"#if {feature}\n"
        else:
            if active != feature:
                raise SystemExit(f"gen_openapi_server: unmatched endif for {feature}")
            yield "directive", "#endif\n"
            active = None
    if active is not None:
        raise SystemExit(f"gen_openapi_server: missing endif for {active}")
    if literal:
        yield "literal", "".join(literal)


text = SPEC.read_text(encoding="utf-8")
with open(OUT, "w", encoding="utf-8") as fh:
    fh.write("/* Auto-generated from api/openapi-server-v1.yaml — do not edit directly. */\n")
    fh.write('#include "aimee_features.h"\n')
    fh.write("static const char *AIMEE_OPENAPI_SERVER_YAML_STR __attribute__((unused)) =\n")
    for kind, value in feature_segments(text):
        if kind == "directive":
            fh.write(value)
        elif value:
            fh.write(f"    {json.dumps(value)}\n")
    fh.write("    ;\n")

print(f"gen_openapi_server: wrote {OUT}")
