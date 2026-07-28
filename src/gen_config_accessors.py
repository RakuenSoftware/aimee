#!/usr/bin/env python3
"""Generate one accessor per config_t field, so nothing outside the config
module ever has to name the type.

config_t is ~750 KB and its shape is a config-module implementation detail. Every
caller outside that module must ask for the value it wants:

    int  config_<name>(void)          scalars
    const char *config_<name>(void)   char[] fields

Both read a single field out of the live snapshot under a reader pin
(config_field in config.c), so a caller pays for one field, not a 750 KB copy.
String accessors return a thread-local buffer valid until the next call to the
SAME accessor on that thread; callers that keep the value copy it.

Regenerate: make config-accessors   (output is checked in, like the other gen_*)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HDR = ROOT / "src" / "modules" / "config" / "config.h"
# Sharded: line-check caps a file at 2500 lines and ~500 accessors overflow it.
# Generated code is still code — the cap exists so no single file becomes
# unreviewable, and a generator is not a reason to opt out.
SHARDS = 8
OUT_C = [ROOT / "src" / "modules" / "config" / f"config_accessors_{i}.c" for i in range(SHARDS)]
OUT_H = ROOT / "src" / "modules" / "config" / "config_accessors.h"

# Fields the config module owns outright: aggregates with no scalar reading, and
# anything a caller must not reach through a flat accessor.
SKIP = {"agents", "agent_count"}

SCALARS = {
    "int": "int",
    "unsigned int": "unsigned int",
    "long": "long",
    "double": "double",
    "float": "float",
    "size_t": "size_t",
    "time_t": "time_t",
    "int64_t": "int64_t",
    "uint32_t": "uint32_t",
}


def existing_functions(text):
    """Names already declared as functions in config.h.

    A hand-written config_<name>() is authoritative — it usually applies
    precedence or defaulting the raw field does not (config_embedding_command
    resolves request > config > env > builtin). Generating a same-named raw
    field reader would both collide at compile time and, if it did not, silently
    bypass that logic."""
    return set(re.findall(r"\b(config_[a-z0-9_]+)\s*\(", text))


STRUCT_ARRAYS = {
    "mcp_clients": ("config_mcp_client_t", "CONFIG_MCP_MAX_CLIENTS", "mcp_client"),
    "cron_jobs": ("cron_job_t", "CRON_JOBS_MAX", "cron_job"),
    "trigger_rules": ("trigger_rule_t", "TRIGGER_RULES_MAX", "trigger_rule"),
    "dispositions": ("config_disposition_t", "CONFIG_MAX_DISPOSITIONS", "disposition"),
}


def struct_fields(text, tname):
    """Scalar and char[] members of a config element struct, at its top level."""
    # Find the CLOSING `} tname;` then walk back to its matching brace. A
    # non-greedy `typedef struct {(.*?)} tname;` starts at the FIRST typedef in
    # the file and swallows every struct in between, which pulled a `key` member
    # out of an unrelated type and generated an offsetof that does not compile.
    close = re.search(r"\}\s*" + tname + r"\s*;", text)
    if not close:
        return []
    depth, i = 0, close.start()
    while i >= 0:
        if text[i] == "}":
            depth += 1
        elif text[i] == "{":
            depth -= 1
            if depth == 0:
                break
        i -= 1
    if i < 0:
        return []
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", text[i + 1:close.start()], flags=re.S)
    out, depth = [], 0
    for line in body.splitlines():
        l = line.strip()
        o, c = l.count("{"), l.count("}")
        top = depth == 0 and o == 0
        depth += o - c
        if not top:
            continue
        fm = re.fullmatch(r"(int|long|double|float|size_t|time_t|char)\s+([a-z_][a-z0-9_]*)"
                          r"\s*(\[[^\]]+\])?\s*;", l)
        if not fm:
            continue
        base, name, arr = fm.groups()
        if base == "char" and arr:
            out.append(("string", name, arr))
        elif base != "char" and not arr:
            out.append(("scalar", name, base))
    return out


def members():
    text = HDR.read_text()
    taken = existing_functions(text)
    start = text.index("typedef struct config")
    body = text[start : text.index("} config_t;", start)]
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)
    out = []
    # Only members at the TOP level of config_t. The struct contains nested
    # anonymous structs whose members (arg_count, name, command, max_tokens, ...)
    # are not config_t fields at all — generating offsetof() for those is a
    # compile error, and a same-named accessor would be a lie about what it
    # reads. Track brace depth: depth 1 is config_t itself.
    depth = 0
    for line in body.splitlines():
        line = line.strip()
        opens, closes = line.count("{"), line.count("}")
        at_top = depth == 1 and opens == 0
        depth += opens - closes
        if not at_top:
            continue
        m = re.match(
            r"^(unsigned\s+int|int64_t|uint32_t|size_t|time_t|unsigned|int|long|double|float|char)"
            r"\s+([a-z_][a-z0-9_]*)\s*(\[[^;]*\])?\s*;$",
            line,
        )
        if not m:
            continue
        base, name, arr = m.groups()
        base = "unsigned int" if base == "unsigned" else base
        if name in SKIP or f"config_{name}" in taken:
            continue
        if base == "char":
            if arr and arr.count("[") == 1:
                out.append(("string", name, arr))
            elif arr and arr.count("[") == 2:
                out.append(("string2d", name, arr))
            # char[][] (string arrays) need an indexed accessor; not generated here.
        elif base in SCALARS and not arr:
            out.append(("scalar", name, SCALARS[base]))
    return out


def main():
    ms = members()
    strings = [m for m in ms if m[0] == "string"]
    string2d = [m for m in ms if m[0] == "string2d"]
    scalars = [m for m in ms if m[0] == "scalar"]

    banner = (
        "/* Auto-generated by src/gen_config_accessors.py — do not edit directly.\n"
        " *\n"
        " * One accessor per config_t field. config_t is ~750 KB and its layout is a\n"
        " * config-module secret; callers outside the module ask for the value they\n"
        " * want and never name the type. Each reads a single field out of the live\n"
        " * snapshot under a reader pin, so a caller pays for one field rather than a\n"
        " * whole-struct copy.\n"
        " */\n"
    )

    h = [banner, "#ifndef DEC_CONFIG_ACCESSORS_H", "#define DEC_CONFIG_ACCESSORS_H 1", ""]
    h.append("/* Scalars. Return 0 when no config can be read (fail closed). */")
    for _, name, ctype in scalars:
        h.append(f"{ctype} config_{name}(void);")
    h.append("")
    h.append(
        "/* char[] fields. The returned pointer is a thread-local buffer valid until\n"
        " * the next call to the SAME accessor on this thread; copy to retain. Never\n"
        " * NULL — an unreadable config yields \"\". */"
    )
    for _, name, _arr in strings:
        h.append(f"const char *config_{name}(void);")
    h.append("")
    h.append(
        "/* Setters. Each is load-modify-save: the config module owns the struct,\n"
        " * so a caller changing one value does not need to materialise it. Returns\n"
        " * 0 on success, -1 if the config could not be read or written.\n"
        " *\n"
        " * A caller setting SEVERAL fields pays a save per call. That is the honest\n"
        " * cost of not handing out the struct; batch changes belong behind a\n"
        " * purpose-named config function rather than a loop of setters. */"
    )
    for _, name, ctype in scalars:
        h.append(f"int config_set_{name}({ctype} value);")
    for _, name, _arr in strings:
        h.append(f"int config_set_{name}(const char *value);")
    h.append("")
    h.append(
        "/* char[][] fields: one row per call. Returns \"\" for an out-of-range\n"
        " * index, so a caller that loops past the count gets empty rather than\n"
        " * reading adjacent memory. Same thread-local lifetime as above. */"
    )
    for _, name, _arr in string2d:
        h.append(f"const char *config_{name}(int index);")
    h += ["", "#endif /* DEC_CONFIG_ACCESSORS_H */", ""]
    OUT_H.write_text("\n".join(h))

    c = [
        banner,
        '#include <stddef.h>',
        '#include <stdio.h>',
        '#include <string.h>',
        "",
        '#include "config.h"',
        '#include "config_accessors.h"',
        "",
        "/* Defined in config.c: reads one field out of the pinned snapshot, falling",
        " * back to a heap-loaded config when no snapshot is live. */",
        "int config_field_read(size_t offset, size_t size, void *dst);",
        "",
    ]
    blocks = []
    for _, name, ctype in scalars:
        blocks.append([
            f"{ctype} config_{name}(void)",
            "{",
            f"   {ctype} v = 0;",
            f"   config_field_read(offsetof(config_t, {name}), sizeof(v), &v);",
            "   return v;",
            "}",
            "",
        ])
    for _, name, arr in strings:
        blocks.append([
            f"const char *config_{name}(void)",
            "{",
            f"   static _Thread_local char buf{arr};",
            "   buf[0] = 0;",
            f"   config_field_read(offsetof(config_t, {name}), sizeof(buf), buf);",
            "   buf[sizeof(buf) - 1] = 0;",
            "   return buf;",
            "}",
            "",
        ])
    for _, name, arr in string2d:
        rows = arr[1:arr.index("]")]
        inner = arr[arr.index("]") + 2:-1]
        blocks.append([
            f"const char *config_{name}(int index)",
            "{",
            f"   static _Thread_local char buf[{inner}];",
            "   buf[0] = 0;",
            f"   if (index < 0 || index >= ({rows}))",
            "      return buf;",
            f"   config_field_read(offsetof(config_t, {name}) + (size_t)index * sizeof(buf),",
            "                     sizeof(buf), buf);",
            "   buf[sizeof(buf) - 1] = 0;",
            "   return buf;",
            "}",
            "",
        ])

    for _, name, ctype in scalars:
        blocks.append([
            f"int config_set_{name}({ctype} value)",
            "{",
            "   config_t *cfg = calloc(1, sizeof(*cfg));",
            "   if (!cfg)",
            "      return -1;",
            "   int rc = config_load(cfg);",
            "   if (rc == 0)",
            "   {",
            f"      cfg->{name} = value;",
            "      rc = config_save(cfg);",
            "   }",
            "   free(cfg);",
            "   return rc;",
            "}",
            "",
        ])
    for _, name, arr in strings:
        blocks.append([
            f"int config_set_{name}(const char *value)",
            "{",
            "   config_t *cfg = calloc(1, sizeof(*cfg));",
            "   if (!cfg)",
            "      return -1;",
            "   int rc = config_load(cfg);",
            "   if (rc == 0)",
            "   {",
            f"      snprintf(cfg->{name}, sizeof(cfg->{name}), \"%s\", value ? value : \"\");",
            "      rc = config_save(cfg);",
            "   }",
            "   free(cfg);",
            "   return rc;",
            "}",
            "",
        ])

    # Per-element accessors for the struct arrays. Without these, anything
    # reading an MCP client or a cron job had to hold a config_t.
    hdr_text = HDR.read_text()
    struct_decls = []
    for field, (tname, cap, prefix) in sorted(STRUCT_ARRAYS.items()):
        for kind, mname, extra in struct_fields(hdr_text, tname):
            fn = f"config_{prefix}_{mname}"
            if kind == "string":
                struct_decls.append(f"const char *{fn}(int index);")
                blocks.append([
                    f"const char *{fn}(int index)",
                    "{",
                    f"   static _Thread_local char buf[sizeof(((config_t *)0)->{field}[0].{mname})];",
                    "   buf[0] = 0;",
                    f"   if (index < 0 || index >= ({cap}))",
                    "      return buf;",
                    f"   config_field_read(offsetof(config_t, {field}) +",
                    f"                         (size_t)index * sizeof(((config_t *)0)->{field}[0]) +",
                    f"                         offsetof({tname}, {mname}),",
                    "                     sizeof(buf), buf);",
                    "   buf[sizeof(buf) - 1] = 0;",
                    "   return buf;",
                    "}",
                    "",
                ])
            else:
                struct_decls.append(f"{extra} {fn}(int index);")
                blocks.append([
                    f"{extra} {fn}(int index)",
                    "{",
                    f"   {extra} v = 0;",
                    f"   if (index < 0 || index >= ({cap}))",
                    "      return v;",
                    f"   config_field_read(offsetof(config_t, {field}) +",
                    f"                         (size_t)index * sizeof(((config_t *)0)->{field}[0]) +",
                    f"                         offsetof({tname}, {mname}),",
                    "                     sizeof(v), &v);",
                    "   return v;",
                    "}",
                    "",
                ])
    if struct_decls:
        hdr_lines = OUT_H.read_text().splitlines()
        i = hdr_lines.index("#endif /* DEC_CONFIG_ACCESSORS_H */")
        hdr_lines[i:i] = [
            "",
            "/* Struct-array elements: one member of one element per call. Bounds-checked",
            " * like the char[][] accessors — an out-of-range index yields 0 or \"\".",
            " * These exist so a caller reading an MCP client or a cron job does not have",
            " * to hold a config_t to reach it. */",
        ] + struct_decls + [""]
        OUT_H.write_text("\n".join(hdr_lines) + "\n")

    # Shard whole accessor blocks, never mid-function: splitting on a line count
    # produced a file ending inside a function body and one starting with a bare
    # `return`, which the compiler caught but which no line-based split can avoid.
    preamble = c
    per = (len(blocks) + SHARDS - 1) // SHARDS
    for i, path in enumerate(OUT_C):
        chunk = blocks[i * per : (i + 1) * per]
        lines = list(preamble)
        for b in chunk:
            lines += b
        path.write_text("\n".join(lines) + "\n")

    print(
        f"gen_config_accessors: wrote {len(scalars)} scalar + {len(strings)} string "
        f"+ {len(string2d)} indexed accessor(s) across {SHARDS} shard(s)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
