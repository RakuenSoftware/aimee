#!/usr/bin/env python3
"""Convert a file's config_t locals to accessors -- ONLY where it is provably safe.

Companion to check-config-encapsulation.py (the ratchet) for the phase-B work in
docs/proposals/done/config-t-encapsulation.md.

Handles:  config_t X;  [+ config_load(&X);]  ... X.field  ->  config_field()

REFUSES (prints SKIP and touches nothing) when it sees:
  - a field with no accessor
  - the local's ADDRESS TAKEN anywhere but config_load/memset/sizeof -- it escapes
    into another function, which is a chained consumer that must be converted first
  - `(void)X;`  -- lives in an #ifdef branch the default build never compiles
  - config_load used as a GUARD -- that is a validity probe, not a field read

Anything refused is listed for hand conversion. Nothing is guessed.

After running with --apply, ALWAYS:  make all kb server  then a full  make unit-tests.
`make -j8` alone is not enough: several sources compile twice under different
defines, and converting a module changes what the LINKER needs (an accessor is
opaque where a stubbed config_load was transparent to LTO), which surfaces only
in the narrow test targets.

usage: config-convert-locals.py [--apply] <path/under/src> [...]
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent / "src"
acc = "".join((ROOT / h).read_text()
              for h in ("modules/config/config_accessors.h", "modules/config/config.h"))

def accessor_for(field):
    f = re.escape(field)
    if re.search(rf"\bconfig_{f}\s*\(\s*(void)?\s*\)", acc):
        return f"config_{field}()"
    # `X.field` is a RAW field read, so the _field variant is the faithful
    # translation. Prefer it explicitly: a key can have both (embedding_command
    # has _field and _current, where _current also applies env/request
    # precedence), and picking whichever the header mentions first silently
    # returned None for embedding_command and skipped the file.
    if re.search(rf"\bconfig_{f}_field\s*\(", acc):
        return f"config_{field}_field()"
    return None

apply = "--apply" in sys.argv
files = [a for a in sys.argv[1:] if not a.startswith("--")]
converted, skipped = [], []

for rel in files:
    # accept either form: "server/x.c" (relative to src/) or "src/server/x.c"
    p = ROOT / rel
    if not p.exists():
        p = ROOT.parent / rel
    if not p.exists():
        print(f"  !! not found: {rel}")
        continue
    src = orig = p.read_text()
    reasons = []
    locals_ = sorted(set(re.findall(r"\bconfig_t\s+([a-zA-Z_]\w*)\s*;", src)))
    if not locals_:
        continue
    for var in locals_:
        v = re.escape(var)
        if re.search(rf"\(void\)\s*{v}\s*;", src):
            reasons.append(f"{var}: (void){var}; -- check #ifdef branches by hand")
            continue
        # ANY address-of the local other than the three known-safe forms means it
        # escapes -- as an argument in ANY position, stored, returned, whatever.
        # Matching only "func(&var" missed &var in later argument positions
        # (agent_route_with_caps_scoped(&acfg, role, &route_cfg, ...)) and deleted
        # locals that were genuinely passed whole. Widen, do not narrow.
        escapes = []
        for m in re.finditer(rf"&{v}\b", src):
            before = src[max(0, m.start() - 40):m.start()]
            if re.search(r"(config_load|memset)\s*\($", before) or \
               re.search(r"sizeof\s*\(?\s*$", before):
                continue
            escapes.append(str(src[:m.start()].count("\n") + 1))
        if escapes:
            reasons.append(f"{var}: address taken at line(s) {','.join(escapes)} -- escapes")
            continue
        if re.search(rf"(if|while)\s*\([^)]*config_load\(\s*&{v}\s*\)", src) or \
           re.search(rf"config_load\(\s*&{v}\s*\)\s*(==|!=|&&|\|\|)", src):
            reasons.append(f"{var}: config_load used as a GUARD -- validity probe, decide by hand")
            continue
        fields = sorted(set(re.findall(rf"\b{v}\.([a-zA-Z_]\w*)", src)))
        acc_map, missing = {}, []
        for fl in fields:
            a = accessor_for(fl)
            (acc_map.__setitem__(fl, a) if a else missing.append(fl))
        if missing:
            reasons.append(f"{var}: no accessor for {' '.join(missing)}")
            continue
        for fl, a in acc_map.items():
            src = re.sub(rf"\b{v}\.{re.escape(fl)}\b", a, src)
        src = re.sub(rf"[ \t]*config_t {v};\n", "", src)
        src = re.sub(rf"[ \t]*config_load\(\s*&{v}\s*\);\n", "", src)
        # a memset of the local is not a consumer, but removing the local orphans
        # it -- drop it too (both `sizeof X` and `sizeof(X)` forms)
        src = re.sub(rf"[ \t]*memset\(\s*&{v}\s*,[^;]*\);\n", "", src)
    if src != orig:
        converted.append(rel)
        if apply:
            p.write_text(src)
    if reasons:
        skipped.append((rel, reasons))

print(f"CONVERTED ({len(converted)}):")
for f in converted:
    print("  ", f)
print(f"\nSKIPPED -- need a human ({len(skipped)}):")
for f, rs in skipped:
    print("  ", f)
    for r in rs:
        print("      -", r)
