#!/usr/bin/env python3
"""Generate a Markdown API reference from api/openapi-v1.yaml.

Output: docs/gen/api-v1.md — a deterministic, dependency-light reference of
every /v1 endpoint (method, summary, parameters, request body, responses).
The output is committed; re-running on an unchanged spec is a byte-for-byte
no-op (the AC: "docs/gen built from OpenAPI in CI; build is a no-op on main").

Run via `make docs-gen` or directly:  python3 scripts/gen-api-docs.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "api" / "openapi-v1.yaml"
OUT = ROOT / "docs" / "gen" / "api-v1.md"

METHODS = ("get", "post", "put", "patch", "delete", "head", "options")


def ref_name(ref: str) -> str:
    """Turn '#/components/schemas/Foo' into 'Foo'."""
    return ref.rsplit("/", 1)[-1] if ref else ref


def schema_summary(schema: dict) -> str:
    """One-line description of a schema node."""
    if not isinstance(schema, dict):
        return ""
    if "$ref" in schema:
        return f"`{ref_name(schema['$ref'])}`"
    t = schema.get("type", "")
    if t == "array":
        items = schema.get("items", {})
        return f"array of {schema_summary(items) or 'item'}"
    if "enum" in schema:
        return f"{t or 'enum'} ({', '.join(str(e) for e in schema['enum'])})"
    return t or ""


def render_params(params: list) -> list[str]:
    lines: list[str] = []
    if not params:
        return lines
    lines.append("")
    lines.append("| Name | In | Required | Type | Description |")
    lines.append("|------|----|----------|------|-------------|")
    for p in params:
        name = p.get("name", "")
        loc = p.get("in", "")
        req = "yes" if p.get("required") else "no"
        typ = schema_summary(p.get("schema", {}))
        desc = (p.get("description", "") or "").replace("\n", " ").strip()
        lines.append(f"| `{name}` | {loc} | {req} | {typ} | {desc} |")
    return lines


def render_responses(responses: dict) -> list[str]:
    lines: list[str] = []
    if not responses:
        return lines
    lines.append("")
    lines.append("Responses:")
    lines.append("")
    for code in sorted(responses.keys(), key=str):
        body = responses[code] or {}
        desc = (body.get("description", "") or "").replace("\n", " ").strip()
        lines.append(f"- `{code}`: {desc}")
    return lines


def normalize_markdown(text: str) -> str:
    """Apply the mechanical project voice rules to source-derived prose."""
    return text.replace(" — ", ": ").replace("—", "-")


def main() -> int:
    with open(SPEC, "r", encoding="utf-8") as fh:
        spec = yaml.safe_load(fh)

    info = spec.get("info", {})
    title = info.get("title", "API")
    version = info.get("version", "")
    paths = spec.get("paths", {})

    out: list[str] = []
    out.append(f"# {title}: v{version}")
    out.append("")
    out.append(
        "> Auto-generated from `api/openapi-v1.yaml` by `scripts/gen-api-docs.py`. "
        "Do not edit by hand; run `make docs-gen` to regenerate."
    )
    out.append("")
    out.append(f"Total endpoints: {sum(1 for p in paths.values() for m in p if m in METHODS)}")
    out.append("")
    out.append("## Endpoints")
    out.append("")

    for path in sorted(paths.keys()):
        item = paths[path] or {}
        for method in METHODS:
            if method not in item:
                continue
            op = item[method] or {}
            summary = (op.get("summary", "") or "").strip()
            out.append(f"### `{method.upper()} /v1{path}`")
            out.append("")
            if summary:
                out.append(summary)
            desc = (op.get("description", "") or "").strip()
            if desc:
                out.append("")
                out.append(desc)
            # Parameters (path-level + operation-level)
            params = list(item.get("parameters", []) or []) + list(op.get("parameters", []) or [])
            out.extend(render_params(params))
            # Request body
            rb = op.get("requestBody")
            if rb:
                content = rb.get("content", {})
                ctypes = ", ".join(f"`{c}`" for c in content.keys())
                out.append("")
                out.append(f"Request body ({ctypes}).")
            out.extend(render_responses(op.get("responses", {})))
            out.append("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    text = normalize_markdown("\n".join(out).rstrip("\n") + "\n")
    # Idempotence: only write when changed (so `make docs-gen` is a no-op on main).
    if OUT.exists() and OUT.read_text(encoding="utf-8") == text:
        print(f"gen-api-docs: {OUT.relative_to(ROOT)} up to date (no-op)")
        return 0
    OUT.write_text(text, encoding="utf-8")
    print(f"gen-api-docs: wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
