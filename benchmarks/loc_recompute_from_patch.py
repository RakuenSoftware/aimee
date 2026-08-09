import glob, os, re, json, collections

def is_test(path):
    b = os.path.basename(path)
    return "/tests/" in path or path.startswith("tests/") or b.startswith("test_") or b.endswith("_test.go")

def loc_from_diff(p):
    prod_a = prod_d = test_a = test_d = 0
    files = set()
    cur = None
    for line in open(p, errors="replace"):
        m = re.match(r"^\+\+\+ b/(.+)$", line)
        if m:
            cur = m.group(1).strip()
            files.add(cur)
            continue
        if line.startswith("--- ") or line.startswith("+++ ") or line.startswith("@@"):
            continue
        if cur is None:
            continue
        if line.startswith("+"):
            if is_test(cur): test_a += 1
            else: prod_a += 1
        elif line.startswith("-"):
            if is_test(cur): test_d += 1
            else: prod_d += 1
    return dict(prod_added=prod_a, prod_deleted=prod_d, test_added=test_a,
                test_deleted=test_d, files=len(files))

rows = []
for d in sorted(glob.glob("/opt/bench/results/cells/*")):
    pd = os.path.join(d, "patch.diff")
    sj = os.path.join(d, "summary.json")
    if not (os.path.exists(pd) and os.path.exists(sj)):
        continue
    try: s = json.load(open(sj))
    except Exception: continue
    r = loc_from_diff(pd)
    old = s.get("loc") or {}
    rows.append((s.get("arm"), s.get("task"), r, old))

print(f"{'arm':22} {'task':16} {'prod+':>6} {'test+':>6} | harness said prod+/test+")
for arm, task, r, old in sorted(rows):
    print(f"{arm:22} {task:16} {r['prod_added']:6} {r['test_added']:6} | "
          f"{old.get('production_added','?'):>5}/{old.get('test_added','?')}")

print("\n=== per-arm totals (recomputed) ===")
agg = collections.defaultdict(lambda: [0, 0, 0])
for arm, task, r, old in rows:
    agg[arm][0] += r["prod_added"]; agg[arm][1] += r["test_added"]; agg[arm][2] += 1
print(f"{'arm':22} {'cells':>5} {'prod+':>7} {'test+':>7}  wrote-tests")
for arm, (pa, ta, n) in sorted(agg.items()):
    wrote = sum(1 for a, t, r, o in rows if a == arm and r["test_added"] > 0)
    print(f"{arm:22} {n:5} {pa:7} {ta:7}  {wrote}/{n}")
