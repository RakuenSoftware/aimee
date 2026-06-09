#!/usr/bin/env python3
"""Generate CLI + configuration reference docs from the canonical source tables.

Two committed outputs (regenerate with `make -C src docs-gen`):
  docs/gen/cli-commands.md   — every `aimee` CLI command + subcommands, from the
                               client help table (src/cli_help_data.h).
  docs/gen/configuration.md  — every config key: the `aimee config get/set`
                               scalar allowlist (src/config_fields.c) plus the
                               config-file (JSON) sections parsed by src/config*.c.

The point is completeness: these are derived from the same tables the binary
uses, so they cannot silently drift from the implementation the way hand-written
lists do.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
GEN = ROOT / "docs" / "gen"

# ─── CLI commands (src/cli_help_data.h) ──────────────────────────────────────
# Each entry: {"name", "description", CLIENT_TIER_X, hidden_flag, subcmd_or_NULL}
# where subcmd is a (possibly multi-line, concatenated) C string of lines like
#   "  sub   description\n"

TIER_LABEL = {"CORE": "Core", "ADVANCED": "Advanced", "ADMIN": "Admin"}


def _c_strings(blob):
    """Concatenate adjacent C string literals, unescaping \\n and \\t."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', blob)
    s = "".join(parts)
    return s.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"')


def parse_cli_commands():
    text = (SRC / "cli_help_data.h").read_text(encoding="utf-8")
    # Each entry begins with {"<name>", and ends at the matching `},` at the
    # entry's top level. Split on the entry-start sentinel instead of brace
    # counting (the subcmd strings contain no braces).
    entries = []
    # normalize: drop the file's leading comment
    body = text[text.index('{"'):]
    # split into entries on `},\n` boundaries that precede a new `{"`
    raw = re.split(r'\},\s*(?=\{")', body)
    for chunk in raw:
        m = re.match(r'\s*\{\s*"([^"]+)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*CLIENT_TIER_(\w+)\s*,\s*(\d+)\s*,\s*(.*)$',
                     chunk, re.S)
        if not m:
            continue
        name, desc, tier, hidden, rest = m.groups()
        subs = None if rest.strip().startswith("NULL") else _c_strings(rest).strip("\n")
        entries.append({"name": name, "desc": desc, "tier": tier,
                        "hidden": hidden == "1", "subs": subs})
    return entries


def render_cli(entries):
    out = ["# CLI Command Reference",
           "",
           "> Auto-generated from `src/cli_help_data.h` by `scripts/gen-reference-docs.py`.",
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


# ─── Config: CLI-settable scalars (src/config_fields.c) ───────────────────────

CFG_TYPE = {"CFG_STRING": "string", "CFG_BOOL": "bool", "CFG_INT": "int", "CFG_FLOAT": "float"}


def parse_config_fields():
    # Each entry is `{"<key>", offsetof(...), <size>, <flag>, CFG_<TYPE>}`. The
    # offsetof/sizeof macros embed commas, so match the key (first string before
    # offsetof) and the type (CFG_* before the closing brace) positionally — they
    # are 1:1 in source order.
    text = (SRC / "config_fields.c").read_text(encoding="utf-8")
    # Bound to the config_fields[] initializer, then parse each `{...}` entry as a
    # unit (split on `},`) so the key and its CFG_* type are paired within one
    # entry — robust to CFG_* uses in helper functions below the table.
    start = text.index("config_fields[] = {")
    text = text[start:text.index("\n};", start)]
    fields = []
    for chunk in text.split("},"):
        km = re.search(r'"([a-z0-9_]+)"\s*,\s*offsetof', chunk)
        tm = re.search(r'(CFG_\w+)', chunk)
        if km and tm:
            fields.append((km.group(1), CFG_TYPE.get(tm.group(1), tm.group(1))))
    return fields


# ─── Config: config-file (JSON) sections (src/config*.c) ──────────────────────
# Pattern: `<var> = cJSON_GetObjectItemCaseSensitive(root, "<section>")` then
# `cJSON_GetObjectItemCaseSensitive(<var>, "<key>")` for the section's keys.

ASSIGN_RE = re.compile(
    r'(\w+)\s*=\s*cJSON_GetObjectItemCaseSensitive\(\s*root\s*,\s*"([^"]+)"\s*\)')
CHILD_RE = re.compile(
    r'cJSON_GetObjectItemCaseSensitive\(\s*(\w+)\s*,\s*"([^"]+)"\s*\)')


def parse_config_sections():
    sections = {}   # section name -> sorted set of keys
    flat = set()    # top-level scalar keys read straight off root
    for cfile in sorted(SRC.glob("config*.c")):
        text = cfile.read_text(encoding="utf-8")
        var_to_section = {}
        for m in ASSIGN_RE.finditer(text):
            var, sect = m.group(1), m.group(2)
            if var == "root":
                continue
            var_to_section[var] = sect
        # collect child keys per section-var
        used_as_parent = set()
        for m in CHILD_RE.finditer(text):
            parent, key = m.group(1), m.group(2)
            used_as_parent.add(parent)
            if parent in var_to_section:
                sections.setdefault(var_to_section[parent], set()).add(key)
        # a (root,"X") whose var is never used as a parent is a flat top key
        for var, sect in var_to_section.items():
            if var not in used_as_parent:
                flat.add(sect)
    # don't double-list a name that is both a section and a stray flat read
    flat -= set(sections)
    return sections, flat


def render_config(fields, sections, flat):
    out = ["# Configuration Reference",
           "",
           "> Auto-generated from `src/config_fields.c` and the `src/config*.c` "
           "parsers by `scripts/gen-reference-docs.py`. Do not edit by hand; run "
           "`make -C src docs-gen` to regenerate.",
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
           "Structured options (arrays, nested objects — e.g. `ensemble.reference_models`) "
           "are not CLI-settable; they are written into the config file under the "
           "sections listed at the end.",
           ""]

    out.append(f"## CLI-settable keys ({len(fields)})")
    out.append("")
    out.append("| Key | Type |")
    out.append("|-----|------|")
    for key, typ in sorted(fields):
        out.append(f"| `{key}` | {typ} |")
    out.append("")

    out.append(f"## Config-file sections ({len(sections)})")
    out.append("")
    out.append("Set in the config JSON as `{\"<section>\": {\"<key>\": ...}}`. Keys "
               "are derived from the section parsers in `src/config*.c`.")
    out.append("")
    for sect in sorted(sections):
        keys = ", ".join(f"`{k}`" for k in sorted(sections[sect]))
        out.append(f"- **`{sect}`** — {keys}")
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


def main():
    check = "--check" in sys.argv
    GEN.mkdir(parents=True, exist_ok=True)
    cli = render_cli(parse_cli_commands())
    cfg = render_config(parse_config_fields(), *parse_config_sections())
    targets = {GEN / "cli-commands.md": cli, GEN / "configuration.md": cfg}

    if check:
        stale = [p.name for p, want in targets.items()
                 if not p.exists() or p.read_text(encoding="utf-8") != want]
        if stale:
            print(f"gen-reference-docs: STALE — run scripts/gen-reference-docs.py: {stale}")
            return 1
        print("gen-reference-docs: ok (cli-commands.md, configuration.md in sync)")
        return 0

    for p, want in targets.items():
        p.write_text(want, encoding="utf-8")
        print(f"gen-reference-docs: wrote {p.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
