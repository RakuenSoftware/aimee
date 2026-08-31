#!/bin/bash
# Runs INSIDE CT 9010. Fetch the served manifest from the REAL running server
# and check it carries today's specs, with the exact vocabulary items added.
curl -fsS -m 10 -H "Authorization: Bearer ct-e2e-bearer" \
    http://127.0.0.1:18897/v1/cli/manifest > /tmp/manifest.json 2>/tmp/manifest.err

if [ ! -s /tmp/manifest.json ]; then
    echo "MANIFEST FETCH FAILED"
    cat /tmp/manifest.err
    exit 1
fi

python3 - <<'PY'
import json
doc = json.load(open("/tmp/manifest.json"))
for k in ("routes", "commands", "dispatch", "marshal"):
    rows = doc.get(k)
    print(f"{k:10s} {len(rows) if isinstance(rows, list) else 'MISSING'}")

marshal = {r["method"]: r.get("args") for r in doc.get("marshal", [])
           if isinstance(r, dict) and "method" in r}
specs = {m: a for m, a in marshal.items() if isinstance(a, dict)}
print(f"\nargument specs served: {len(specs)}")

# The five methods added today, and the vocabulary each one proves.
want = {
    "session.presence":      None,
    "insights.overview":     ("min", "max"),
    "delegate.backend_exec": ("from_end",),
    "memory.get":            ("alt_flag",),
    "memory.embed":          None,
}
print("\ntoday's methods, as the RUNNING SERVER reports them:")
bad = 0
for m, keys in want.items():
    spec = specs.get(m)
    if spec is None:
        print(f"  MISSING  {m}")
        bad += 1
        continue
    flat = json.dumps(spec)
    missing = [k for k in (keys or ()) if f'"{k}"' not in flat]
    print(f"  {'OK      ' if not missing else 'NO-VOCAB'} {m}"
          + (f"  (absent: {missing})" if missing else ""))
    bad += bool(missing)

# The defect that shipped: memory.delete must say atoll, not atoi.
md = json.dumps(specs.get("memory.delete", {}))
ok = "number_lenient_int64" in md
print(f"\n  {'OK      ' if ok else 'REGRESS '} memory.delete uses the 64-bit parse")
bad += (not ok)

print("\nVERDICT:", "all served correctly" if bad == 0 else f"{bad} problem(s)")
raise SystemExit(1 if bad else 0)
PY
