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

# These config_t members remain as process-memory compatibility fields, but
# their generated setters must write exclusively to Vault. The environment-form
# name is the canonical Vault/runtime-cache key.
SECRET_FIELDS = {
    "db2_url": "AIMEE_DB2_URL",
    "search_tavily_api_key": "AIMEE_SEARCH_TAVILY_API_KEY",
    "proxy_token": "AIMEE_PROXY_TOKEN",
    "ingress_trusted_proxy_secret": "AIMEE_INGRESS_PROXY_SECRET",
    "kb_api_bearer_token": "AIMEE_KB_API_BEARER_TOKEN",
    "telemetry_metrics_token": "AIMEE_TELEMETRY_METRICS_TOKEN",
    "kb_client_bearer_token": "AIMEE_KB_API_BEARER_TOKEN",
    "server_api_bearer_token": "AIMEE_API_BEARER_TOKEN",
    "trigger_auth_token": "AIMEE_TRIGGER_AUTH_TOKEN",
    "kb_curator_provider_api_key": "AIMEE_KB_CURATOR_PROVIDER_API_KEY",
    "kb_curator_tier_b_api_key": "AIMEE_KB_CURATOR_TIER_B_API_KEY",
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
    # A declaration is a logical statement, not a physical line. clang-format
    # wraps a long one across lines, and matching per-line silently skipped
    # those: identity_working_profile_injection_fields got no indexed accessor
    # for no reason other than its [CONFIG_...][CONFIG_...] not fitting on one
    # line, while the charter arrays beside it did. A field losing its accessor
    # because of formatting is a silent hole in the encapsulation surface, so
    # accumulate until the ';' and match the joined statement.
    depth = 0
    pending = ""
    for line in body.splitlines():
        line = line.strip()
        opens, closes = line.count("{"), line.count("}")
        at_top = depth == 1 and opens == 0
        depth += opens - closes
        if not at_top:
            pending = ""
            continue
        # Preprocessor lines are not part of any declaration and do not end in
        # ';'. Accumulating one swallows the field declared after it — that is
        # how compact_enabled, worktree_stale_secs and aux_enabled (each
        # preceded by a #define) lost their accessors on the first cut of this.
        if line.startswith("#"):
            continue
        pending = f"{pending} {line}".strip() if pending else line
        if not pending.endswith(";"):
            continue
        line = " ".join(pending.split())
        pending = ""
        m = re.match(
            r"^(unsigned\s+int|int64_t|uint32_t|size_t|time_t|unsigned|int|long|double|float|char)"
            r"\s+([a-z_][a-z0-9_]*)\s*(\[[^;]*\])?\s*;$",
            line,
        )
        if not m:
            continue
        base, name, arr = m.groups()
        base = "unsigned int" if base == "unsigned" else base
        # The 2-D split below is index arithmetic that assumes "][" are adjacent.
        # A wrapped declaration joins as "] [", which silently produced
        # "char buf[[CONFIG_..." — strip whitespace so the shape a field is
        # declared in cannot change the accessor generated for it.
        if arr:
            arr = re.sub(r"\s+", "", arr)
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


def _widest_string(strings):
    """Widest declared char[] among the string fields, resolved through config.h
    defines. A caller-side buffer of this size can never truncate."""
    text = HDR.read_text()
    widest = 0
    for _, name, arr in strings:
        tok = arr.strip()[1:-1].strip()
        if tok.isdigit():
            w = int(tok)
        else:
            m = re.search(rf"#define\s+{re.escape(tok)}\s+(\d+)", text)
            if not m:
                continue
            w = int(m.group(1))
        widest = max(widest, w)
    return widest


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
    # Self-contained: int64_t appears in these signatures, and a caller that includes
    # only this header (the point of the accessor surface) must not have to include
    # config.h first to get it.
    h.append("#include <stddef.h> /* size_t, for the _copy forms */")
    h.append("#include <stdint.h>")
    h.append("")
    h.append(
        "/* A buffer of this size holds ANY string field whole, so a caller using it\n"
        " * with a _copy accessor never truncates and never has to name config_t to\n"
        " * spell the field's width. Generated as the widest string field. */"
    )
    h.append(f"#define CONFIG_COPY_MAX {_widest_string(strings)}")
    h.append("")
    h.append(
        "/* Scalars. When config cannot be read, an accessor returns the field's DECLARED\n"
        " * DEFAULT (config_field_read copies the defaults config_set_defaults applied), NOT\n"
        " * zero. Returning 0 would INVERT every default-ON dial — reporting a fail-closed\n"
        " * guard as disabled exactly when config is broken. */"
    )
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
        "/* Copy-out form for every string field.\n"
        " *\n"
        " * Prefer this over the pointer form whenever the value OUTLIVES the next\n"
        " * call to the same accessor -- stored in a struct, passed to something that\n"
        " * runs a subprocess or an HTTP round trip, or read again after other config\n"
        " * reads. The pointer form hands back a per-accessor thread-local buffer, so\n"
        " * in those cases it is a dangling read waiting to happen, and the caller has\n"
        " * to hand-size a buffer (which meant naming config_t just to spell\n"
        " * sizeof(((config_t *)0)->field), putting the type right back in the caller).\n"
        " *\n"
        " * Truncates to n and always NUL-terminates. Returns the field's full width so\n"
        " * a caller can detect truncation; 0 when out is NULL or n is 0. */"
    )
    for _, name, _arr in strings:
        h.append(f"size_t config_{name}_copy(char *out, size_t n);")
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
    for _, name, arr in strings:
        blocks.append([
            f"size_t config_{name}_copy(char *out, size_t n)",
            "{",
            f"   char buf{arr};",
            "   if (!out || n == 0)",
            "      return 0;",
            f"   config_field_read(offsetof(config_t, {name}), sizeof(buf), buf);",
            "   buf[sizeof(buf) - 1] = 0;",
            "   snprintf(out, n, \"%s\", buf);",
            "   return sizeof(buf);",
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
        if name in SECRET_FIELDS:
            blocks.append([
                f"int config_set_{name}(const char *value)",
                "{",
                f'   return config_secret_store("{SECRET_FIELDS[name]}", value);',
                "}",
                "",
            ])
            continue
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
        path.write_text("\n".join(lines).rstrip("\n") + "\n")

    print(
        f"gen_config_accessors: wrote {len(scalars)} scalar + {len(strings)} string "
        f"+ {len(string2d)} indexed accessor(s) across {SHARDS} shard(s)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
