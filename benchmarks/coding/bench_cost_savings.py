#!/usr/bin/env python3
"""3-measurement cost/token/time savings benchmark driver.

Reuses the supervised-swebench arms (primary-alone = measurements 1+2 via the
economizer's realized/avoided split; supervised = measurement 3) and attributes
primary-model tokens from the REAL fleet ledger by bracketing token_audit row
ids around each arm.

Why id-range attribution (not the harness's `delegation_id LIKE '%-<job_id>'`):
on this fleet a delegation_id is `deleg-<lease_owner>-<ts>-<seqno>` -- the
trailing number is a per-session sequence, NOT the CLI job id, so the LIKE match
mis-attributes. We instead snapshot MAX(id) on the fleet ledger immediately
before and after each arm and sum rows in that id window filtered to the primary
model. This is exact as long as the primary model has no other concurrent
traffic during the run (we are the sole tenant). The primary agent (codex) and
the supervisor in arm C are the SAME model (gpt-5.6-sol); the cheap workers are
other models, so the model filter isolates the primary/supervisor automatically.

Ledger access is over SSH to the fleet host (this box's local ledger is empty).

Example:
  python3 benchmarks/coding/bench_cost_savings.py \
    --regions benchmarks/results/swebench_supervised/regions \
    --limit 3 \
    --primary codex --primary-model gpt-5.6-sol \
    --pool MiniMax-M3,mimo-v2.5-pro,mistral-medium-3-5,kimi-k2.7-code --n 3 \
    --output benchmarks/results/cost_savings/smoke.json
"""
from __future__ import annotations
import argparse, glob, json, os, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from benchmarks.coding import bench_swebench_supervised as B
from benchmarks.coding import cost_savings as C

FLEET_SSH = os.environ.get("AIMEE_FLEET_SSH", "admin@192.168.1.254")
FLEET_DB = os.environ.get("AIMEE_FLEET_DB",
                          "/mnt/media/.plugins/aimee-server/home/aimee.db")

# Runs on the fleet host. argv: <db> <id_lo> <id_hi> <model>. id window is (lo, hi].
_REMOTE = r'''
import sqlite3, sys, json
db, lo, hi, model = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
c = sqlite3.connect(db)
if hi < 0:  # watermark request
    print(json.dumps({"maxid": c.execute("SELECT COALESCE(MAX(id),0) FROM token_audit").fetchone()[0]}))
    sys.exit()
out = {}
for uk in ("realized", "avoided"):
    r = c.execute(
        "SELECT COUNT(*), COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(completion_tokens),0) "
        "FROM token_audit WHERE model=? AND usage_kind=? AND id>? AND id<=?",
        (model, uk, lo, hi)).fetchone()
    out[uk] = {"rows": r[0], "prompt": r[1], "completion": r[2]}
print(json.dumps(out))
'''


def _ssh_ledger(*argv: str) -> dict:
    p = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                        FLEET_SSH, "python3", "-", FLEET_DB, *map(str, argv)],
                       input=_REMOTE, capture_output=True, text=True, timeout=45)
    if p.returncode != 0:
        raise RuntimeError(f"fleet ledger ssh failed: {p.stderr.strip()}")
    return json.loads(p.stdout)


def _maxid() -> int:
    return _ssh_ledger(0, -1, "_")["maxid"]


def _settle_maxid(prev: int, quiet_s: float = 4.0, timeout_s: float = 40.0) -> int:
    """Poll MAX(id) until it stops growing for `quiet_s` (ledger flush lag), so the
    hi watermark captures every row the arm produced."""
    last, stable_since, t0 = _maxid(), time.time(), time.time()
    while time.time() - t0 < timeout_s:
        time.sleep(2)
        cur = _maxid()
        if cur == last:
            if time.time() - stable_since >= quiet_s:
                return cur
        else:
            last, stable_since = cur, time.time()
    return last


def _arm_tokens(id_lo: int, id_hi: int, model: str) -> C.ArmTokens:
    s = _ssh_ledger(id_lo, id_hi, model)
    return C.ArmTokens(
        realized_prompt=s["realized"]["prompt"], realized_completion=s["realized"]["completion"],
        avoided_prompt=s["avoided"]["prompt"], avoided_completion=s["avoided"]["completion"],
    )


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--regions", default="benchmarks/results/swebench_supervised/regions")
    ap.add_argument("--limit", type=int, default=0, help="use only the first N regions (0=all)")
    ap.add_argument("--primary", default="codex")
    ap.add_argument("--primary-model", default="gpt-5.6-sol")
    ap.add_argument("--pool", default="MiniMax-M3,mimo-v2.5-pro,mistral-medium-3-5,kimi-k2.7-code")
    ap.add_argument("--n", type=int, default=3)
    ap.add_argument("--seed", type=int, default=1729)
    ap.add_argument("--aimee-bin", default=os.environ.get("AIMEE_BENCH_CLIENT", "aimee"))
    ap.add_argument("--price-in", type=float, default=C.DEFAULT_PRICE["input_per_mtok"])
    ap.add_argument("--price-out", type=float, default=C.DEFAULT_PRICE["output_per_mtok"])
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    pool = [w.strip() for w in args.pool.split(",") if w.strip()]
    if args.primary in pool:
        raise SystemExit(f"--primary '{args.primary}' must not appear in --pool")

    insts = [json.load(open(f)) for f in sorted(glob.glob(str(Path(args.regions) / "*.json")))]
    if args.limit:
        insts = insts[:args.limit]
    if not insts:
        raise SystemExit(f"no regions in {args.regions}; run swebench_supervised_prep.py first")
    # The `draft` role is result-cached server-side (delegate_role_result_cache_enabled):
    # a repeated (role, prompt) returns the cached diff with NO new token_audit row, so a
    # re-run's arm would measure 0 tokens. Stamp each problem with a per-run nonce so every
    # solve/select prompt is a guaranteed cache MISS and we measure real model spend.
    nonce = f"benchrun-{os.getpid()}-{int(time.time())}"
    for i in insts:
        i["problem"] = f"[{nonce}]\n{i['problem']}"
    price = {"input_per_mtok": args.price_in, "output_per_mtok": args.price_out}
    print(f"{len(insts)} instances; primary={args.primary} ({args.primary_model}); "
          f"pool={pool}; n={args.n}", file=sys.stderr)

    # Confirm ledger reachable before spending fleet tokens.
    base = _maxid()
    print(f"fleet ledger reachable; token_audit max id = {base}", file=sys.stderr)

    fleet = B.Fleet(args.aimee_bin)

    # --- Arm P: primary solves alone (measurements 1 default + 2 economized) ---
    print("=== ARM P: primary alone (single-shot) ===", file=sys.stderr)
    p_lo = _maxid()
    P_recs, p_wall, _ = B.run_arm_A(insts, fleet, args.primary)
    p_hi = _settle_maxid(p_lo)
    primary_tok = _arm_tokens(p_lo, p_hi, args.primary_model)
    print(f"  wall={p_wall}s  ledger[{p_lo}->{p_hi}]  "
          f"realized={primary_tok.realized_total} avoided_prompt={primary_tok.avoided_prompt}",
          file=sys.stderr)

    # --- Arm S: supervised best-of-N (measurement 3, supervisor tokens only) ---
    print("=== ARM S: supervised best-of-N + selection (single-shot) ===", file=sys.stderr)
    s_lo = _maxid()
    S_recs, s_wall, _ = B.run_arm_C(insts, fleet, args.primary, pool, args.n, args.seed)
    s_hi = _settle_maxid(s_lo)
    superv_tok = _arm_tokens(s_lo, s_hi, args.primary_model)
    print(f"  wall={s_wall}s  ledger[{s_lo}->{s_hi}]  supervisor_realized={superv_tok.realized_total}",
          file=sys.stderr)

    summary = C.summarize(primary_tok, superv_tok, p_wall, s_wall, price)

    def _git(*a):
        try:
            return subprocess.run(["git", *a], capture_output=True, text=True, timeout=10).stdout.strip()
        except Exception:
            return None

    result = {
        "provenance": {
            "aimee_commit": _git("rev-parse", "HEAD"),
            "primary_agent": args.primary, "primary_model": args.primary_model,
            "pool": pool, "n": args.n, "instances": [i["instance_id"] for i in insts],
            "single_shot": True, "fleet_ssh": FLEET_SSH, "fleet_db": FLEET_DB,
            "ledger_windows": {"primary": [p_lo, p_hi], "supervised": [s_lo, s_hi]},
        },
        "raw_tokens": {"primary": vars(primary_tok), "supervised": vars(superv_tok)},
        "candidate_counts": {iid: r.get("n_candidates") for iid, r in S_recs.items()},
        "summary": summary,
    }
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2))
    md = out.with_suffix(".md")
    md.write_text(C.render_markdown(summary) + "\n")

    print("\n" + C.render_markdown(summary), file=sys.stderr)
    print("\n" + C.render_headline(summary), file=sys.stderr)
    print(f"\nwritten: {out}  and  {md}", file=sys.stderr)


if __name__ == "__main__":
    main()
