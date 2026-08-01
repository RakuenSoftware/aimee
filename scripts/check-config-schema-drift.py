#!/usr/bin/env python3
"""Does the config schema allowlist agree with what config actually parses and writes?

`config_schema[]` in src/modules/config/config.c is a hand-maintained allowlist of
top-level keys. Anything not in it is an "unknown key" validation issue -- fatal now
that strict mode is on. The list drifts from the parsers silently, because nothing
checked it:

  - `worktree_gc` was PARSED (config_sections.c) and WRITTEN by config_save, yet
    absent from the allowlist, so aimee emitted a config it would refuse to load.
  - `autonomy`, `memory_window`, `memory_rewrite`, `memory_negation` were all
    parsed and unlisted.

Reports three classes:
  MISSING (parsed)   -- a parser reads it, the allowlist rejects it   [breaks configs]
  MISSING (written)  -- config_save emits it, the allowlist rejects it [breaks round-trip]
  UNUSED             -- allowlisted but no parser reads it            [dead or renamed]

Exit 1 if either MISSING class is non-empty. UNUSED is reported, not enforced:
a key may legitimately be read somewhere this script cannot see.
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
CFG = ROOT / "src" / "modules" / "config"

schema_src = (CFG / "config.c").read_text()
m = re.search(r"config_schema\[\]\s*=\s*\{(.*?)\n\};", schema_src, re.S)
if not m:
    print("check-config-schema-drift: could not locate config_schema[]", file=sys.stderr)
    sys.exit(2)
allowed = set(re.findall(r'\{"([^"]+)"\s*,\s*SCHEMA_', m.group(1)))

# A key is valid if config_schema[] names it OR config_fields[] does: the
# validator (config.c, "unknown key" branch) falls back to config_field_lookup
# for flat scalars, which are NOT hand-listed in config_schema[]. Checking only
# the schema table reports every flat scalar -- provider, guardrail_mode, ... --
# as missing, which is how this script lied the first time it was run.
fields = set(re.findall(r'\{"([^"]+)"\s*,\s*offsetof\(', (CFG / "config_fields.c").read_text()))
allowed |= fields

parsed, written = set(), set()
for c in sorted(CFG.glob("*.c")):
    txt = c.read_text()
    # top-level reads: cJSON_GetObjectItem*(root, "key")
    parsed |= set(re.findall(r'cJSON_GetObjectItem\w*\(\s*root\s*,\s*"([^"]+)"', txt))
    # top-level writes: cJSON_Add*ToObject(root, "key", ...)
    written |= set(re.findall(r'cJSON_Add\w+ToObject\(\s*root\s*,\s*"([^"]+)"', txt))

missing_parsed = sorted(parsed - allowed)
missing_written = sorted(written - allowed)
# Only config_schema[] entries can be 'unused'. A config_fields[] scalar is read
# through config_field_lookup rather than a top-level cJSON_GetObjectItem, so it
# never appears in `parsed` and reporting it here is noise, not drift.
unused = sorted(k for k in (allowed - fields - parsed - written) if '.' not in k)

rc = 0
if missing_parsed:
    rc = 1
    print(f"MISSING from config_schema[] but PARSED ({len(missing_parsed)}):")
    for k in missing_parsed:
        print(f"  {k}")
if missing_written:
    rc = 1
    print(f"MISSING from config_schema[] but WRITTEN by config_save ({len(missing_written)}):")
    for k in missing_written:
        print(f"  {k}   <- aimee emits a config it would refuse to load")
if unused:
    print(f"note: allowlisted but no top-level parser found ({len(unused)}): {' '.join(unused)}")
if rc == 0:
    print(f"check-config-schema-drift: ok ({len(allowed)} allowlisted, "
          f"{len(parsed)} parsed, {len(written)} written)")
sys.exit(rc)
