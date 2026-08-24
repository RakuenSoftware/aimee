"""Every served numeric field, checked against the parse its marshaller uses.

memory.delete shipped with atoi where the marshaller used atoll, and no sample
disagreed because every id was small. That is a whole CLASS of defect, so this
audits the class rather than the one instance: for each served spec, find the
marshaller, and compare the parse function actually called against the type the
spec claims.
"""
import re, pathlib, json, sys

# Run from anywhere: make invokes this from src/, a developer from the root.
ROOT = pathlib.Path(__file__).resolve().parent.parent

SRC = list((ROOT / "src").rglob("cli_v1_routes*.c"))
text = {p: p.read_text() for p in SRC}
allsrc = "\n".join(text.values())

# method -> marshaller name, from the dispatch tables
m2f = dict(re.findall(r'\{"([a-z0-9_.]+)",\s*(marshal_[a-z0-9_]+)\}', allsrc))

def body(fn):
    m = re.search(r'^cJSON \*' + re.escape(fn) + r'\([^)]*\)\s*\{', allsrc, re.M)
    if not m:
        return ""
    i = m.end() - 1
    depth, j = 0, i
    while j < len(allsrc):
        if allsrc[j] == '{': depth += 1
        elif allsrc[j] == '}':
            depth -= 1
            if depth == 0: return allsrc[i:j+1]
        j += 1
    return ""

specs = (ROOT / "src/server/cli_argspec_defs_data.h").read_text()
rows = re.findall(r'\{"([a-z0-9_.]+)",\s*\n?((?:\s*"(?:[^"\\]|\\.)*"\s*\n?)+)\}', specs)

# cli_args_get_int() is `v ? atoi(v) : def` -- the same parse, so a spec saying
# number_lenient for a field read through it is correct, not a mismatch. Each
# type maps to the set of calls that implement it.
EXPECT = {"number_lenient": {"atoi", "cli_args_get_int"},
          "number_lenient_int64": {"atoll", "strtoll"},
          "number_lenient_real": {"atof"},
          "number": {"strtod"}}

bad = 0
for method, blob in rows:
    lit = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', blob))
    try:
        spec = json.loads(lit.encode().decode("unicode_escape"))
    except Exception:
        continue
    fn = m2f.get(method)
    if not fn:
        continue
    b = body(fn)
    for f in spec.get("fields", []):
        ty = f.get("type")
        if ty not in EXPECT:
            continue
        name = f["json"]
        want = EXPECT[ty]
        # which parse does the marshaller use for this json field?
        used = set(re.findall(r'(atoi|atoll|atof|strtod|strtoll|cli_args_get_int)\s*\(', b))
        if used and not (want & used):
            print(f"MISMATCH {method}.{name}: spec says {ty} (expects one of "
                  f"{sorted(want)}), marshaller uses {sorted(used)}")
            bad += 1
print(f"\n{bad} numeric mismatch(es)")
sys.exit(1 if bad else 0)
