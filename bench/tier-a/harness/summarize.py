"""Collapse the per-model score files into the comparison tables for the report.

Emits markdown so the report is generated from the committed result files rather
than hand-copied numbers.
"""

import argparse
import json
import pathlib

HERE = pathlib.Path(__file__).parent
ROOT = HERE.parent


def load(dirname):
    d = ROOT / "results" / dirname
    if not d.is_dir():
        return {}
    out = {}
    for f in sorted(d.glob("*.score.json")):
        base = f.name[: -len(".score.json")]
        nofloor = d / f"{base}.score.nofloor.json"
        out[base] = {
            "floored": json.loads(f.read_text()),
            "nofloor": json.loads(nofloor.read_text()) if nofloor.exists() else None,
        }
    return out


def licence_of(model_id, reg):
    for m in reg["models"]:
        if m["id"].lower() == model_id.lower() or m["id"].split("/")[-1].lower() == model_id.split("/")[-1].lower():
            return m["licence"], m["class"]
    return "?", "?"


def accuracy_table(rows, reg):
    # Strict leads: both endpoints must name the labelled entity, only the
    # predicate may vary. This measures extraction, not what a downstream entity
    # resolver might later reconcile.
    # Two headline columns, because they answer different questions:
    #   capability = can this model extract the fact at all (MF_CONF_FLOOR lifted)
    #   committed  = what the drain would actually write today (floor applied)
    # The gap is a config artefact, not a model property, and it falls almost
    # entirely on small models: Qwen3-0.6B is 0.400 capable and 0.000 committed.
    out = ["| model | params | licence | F1 capability | F1 committed | precision | recall | schema | abstain | med ms |",
           "|---|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    ranked = sorted(rows.items(),
                    key=lambda kv: -((kv[1]["nofloor"] or kv[1]["floored"])["strict"]["f1"]))
    for _, r in ranked:
        fl, nf = r["floored"], r["nofloor"]
        mid = fl.get("model") or "?"
        lic, cls = licence_of(mid, reg)
        params = next((m["params"] for m in reg["models"]
                       if m["id"].split("/")[-1].lower() == mid.split("/")[-1].lower()), "?")
        h = fl["output_health"]
        oe = fl["over_extraction"]
        ab = oe.get("abstention_rate_on_schema")
        lat = fl.get("latency_ms", {}).get("median")
        cap = nf['strict']['f1'] if nf else fl['strict']['f1']
        out.append(
            f"| {mid} | {params} | {lic} | {cap:.3f} | "
            f"{fl['strict']['f1']:.3f} | {fl['strict']['precision']:.3f} | "
            f"{fl['strict']['recall']:.3f} | {h['schema_rate']:.2f} | "
            f"{ab if ab is None else f'{ab:.2f}'} | {lat} |"
        )
    return "\n".join(out)


def cpu_table():
    d = ROOT / "results" / "cpu"
    if not d.is_dir():
        return "_no CPU results_"
    out = ["| model | quant | pp t/s (400 tok prompt) | tg t/s (64 tok gen) | est ms/note |",
           "|---|---|---:|---:|---:|"]
    for f in sorted(d.glob("*.json")):
        try:
            runs = json.loads(f.read_text())
        except json.JSONDecodeError:
            continue
        pp = tg = None
        name = quant = "?"
        for r in runs:
            # model_filename is the full HF cache path; the repo id is the
            # readable part and matches the accuracy tables.
            raw = r.get("model_filename") or f.stem
            if "models--" in raw:
                name = raw.split("models--")[1].split("/")[0].replace("--", "/")
            else:
                name = pathlib.Path(raw).stem
            quant = r.get("model_type", "?")
            if r.get("n_prompt"):
                pp = r["avg_ts"]
            elif r.get("n_gen"):
                tg = r["avg_ts"]
        if pp and tg:
            est = round(400 / pp * 1000 + 48 / tg * 1000)
        else:
            est = "?"
        out.append(f"| {name} | {quant} | {pp and round(pp,1)} | {tg and round(tg,1)} | {est} |")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    args = ap.parse_args()
    reg = json.loads((HERE / "models.json").read_text())

    parts = ["## GPU accuracy — production prompt\n", accuracy_table(load("gpu"), reg), ""]
    abl = load("ablation-conf")
    if abl:
        parts += ["\n## GPU accuracy — confidence-literal ablation (NOT production)\n",
                  accuracy_table(abl, reg), ""]
    parts += ["\n## CPU speed (llama.cpp, Q8_0, pinned cores)\n", cpu_table(), ""]
    md = "\n".join(parts)
    print(md)
    if args.out:
        (ROOT / args.out).write_text(md + "\n")


if __name__ == "__main__":
    main()
