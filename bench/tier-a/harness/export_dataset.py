"""Flatten every result into one tidy dataset for charting and writeups.

Produces results/dataset.csv (one row per model x condition x metric-group) and
results/dataset.json (nested, including the per-category breakdown). Both are
generated, never hand-edited — so a chart in an article and the committed raw
predictions cannot drift apart.
"""

import csv
import json
import pathlib

HERE = pathlib.Path(__file__).parent
ROOT = HERE.parent
RESULTS = ROOT / "results"

CONDITIONS = {
    "production": "gpu",
    "ablation_conf": "ablation-conf",
}


def load_registry():
    reg = json.loads((HERE / "models.json").read_text())
    by_id = {m["id"]: m for m in reg["models"]}
    # Results are keyed by slug; map both ways.
    return {m["id"].replace("/", "_"): m for m in reg["models"]} | by_id


def cpu_rows():
    out = {}
    d = RESULTS / "cpu"
    if not d.is_dir():
        return out
    for f in sorted(d.glob("*.json")):
        try:
            runs = json.loads(f.read_text())
        except json.JSONDecodeError:
            continue
        pp = tg = None
        repo = None
        for r in runs:
            raw = r.get("model_filename") or ""
            if "models--" in raw:
                repo = raw.split("models--")[1].split("/")[0].replace("--", "/")
            if r.get("n_prompt"):
                pp = r["avg_ts"]
            elif r.get("n_gen"):
                tg = r["avg_ts"]
        if repo and pp and tg:
            # GGUF conversions live under a different org than the original
            # weights (ggml-org/gemma-4-E4B-it-GGUF vs google/gemma-4-E4B-it), so
            # join on the bare model name, not the full repo id.
            base = repo.split("/")[-1].replace("-GGUF", "").replace("-Instruct-Q8_0", "-Instruct")
            out[base.casefold()] = {
                "cpu_prompt_tok_s": round(pp, 1),
                "cpu_gen_tok_s": round(tg, 1),
                "cpu_est_ms_per_note": round(400 / pp * 1000 + 48 / tg * 1000),
            }
    return out


def main():
    reg = load_registry()
    cpu = cpu_rows()
    nested, flat = {}, []

    for cond, subdir in CONDITIONS.items():
        d = RESULTS / subdir
        if not d.is_dir():
            continue
        for f in sorted(d.glob("*.score.json")):
            slug = f.name[: -len(".score.json")]
            floored = json.loads(f.read_text())
            nf_path = d / f"{slug}.score.nofloor.json"
            nofloor = json.loads(nf_path.read_text()) if nf_path.exists() else None
            mid = floored.get("model") or slug
            meta = reg.get(slug) or reg.get(mid) or {}
            h, oe = floored["output_health"], floored["over_extraction"]

            row = {
                "model": mid,
                "condition": cond,
                "params": meta.get("params"),
                "licence": meta.get("licence"),
                "class": meta.get("class"),
                "f1_production": floored["lenient"]["f1"],
                "f1_no_floor": nofloor["lenient"]["f1"] if nofloor else None,
                "precision": floored["lenient"]["precision"],
                "recall": floored["lenient"]["recall"],
                "f1_strict": floored["strict"]["f1"],
                "schema_rate": h["schema_rate"],
                "json_parse_rate": h["json_parse_rate"],
                "in_seed_ontology": h.get("in_seed_ontology"),
                "abstention_on_factless": oe.get("abstention_rate_on_schema"),
                "spurious_triples": oe.get("spurious_triples"),
                "dropped_by_conf_floor": h.get("dropped_by_conf_floor"),
                "median_latency_ms_gpu": floored.get("latency_ms", {}).get("median"),
                "median_completion_tokens": floored.get("completion_tokens", {}).get("median"),
                "notes_scored": h["notes"],
            }
            # CPU numbers are condition-independent; attach to the production row.
            if cond == "production":
                row.update(cpu.get(mid.split("/")[-1].casefold(), {}))
            flat.append(row)
            nested.setdefault(mid, {})[cond] = {
                "summary": row,
                "by_category": floored["lenient"]["by_category"],
            }

    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "dataset.json").write_text(json.dumps(nested, indent=2) + "\n")

    cols = sorted({k for r in flat for k in r})
    lead = ["model", "condition", "params", "licence", "class",
            "f1_production", "f1_no_floor", "precision", "recall"]
    cols = lead + [c for c in cols if c not in lead]
    with open(RESULTS / "dataset.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        for r in sorted(flat, key=lambda r: (-(r["f1_production"] or 0), r["model"])):
            w.writerow(r)

    print(f"wrote dataset.csv ({len(flat)} rows) and dataset.json ({len(nested)} models)")


if __name__ == "__main__":
    main()
