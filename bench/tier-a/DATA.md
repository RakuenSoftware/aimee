# Data package

Everything behind the numbers in `REPORT.md`, kept so the results can be
re-derived, audited, or charted without rerunning anything. Written with a public
writeup in mind: every claim should be traceable to a file here.

## Layout

```
bench/tier-a/
  data/
    gold.jsonl              70 notes + gold triples — the benchmark itself
    LABELING.md             labelling rules, category taxonomy, known limits
  harness/
    prompt.py               reconstructs the production prompt from the C sources
    run_hf.py               transformers runner (accuracy)
    score.py                triple P/R/F1, abstention, ontology adherence
    summarize.py            regenerates the report's markdown tables
    export_dataset.py       flattens everything to dataset.csv / dataset.json
    capture_provenance.py   model revision SHAs + software stack
    verify_licences.py      re-reads each licence from the Hub, fails on mismatch
    models.json             candidate registry with licence and generation class
    sweep_*.sh              the runs, in the order they were made
  results/
    gpu/                    accuracy, production prompt
    ablation-conf/          accuracy, confidence-literal ablation
    cpu/                    llama.cpp speed (raw llama-bench JSON)
    diagnostics/            probes into the models that scored zero
    dataset.csv             tidy, one row per model x condition — chart from this
    dataset.json            same plus the per-category breakdown
    PROVENANCE.json         model revisions, licences, torch/CUDA, commits
```

## Per-model files

For every model and condition:

- `*.pred.jsonl`: **one row per note**: the model's raw text, the parsed facts
  before and after the confidence floor, latency, token counts, whether the JSON
  parsed, whether it carried the `facts` array, and whether it truncated. This is
  the primary record; every aggregate is derived from it.
- `*.score.json`: production scoring (confidence floor applied).
- `*.score.nofloor.json`: identical run with `MF_CONF_FLOOR` lifted.
- `*.log`: stdout/stderr, including model load and any recovery paths taken.

Raw predictions are committed deliberately. A benchmark table nobody can dispute
in detail is not evidence, and several conclusions here (the confidence-0.0
behaviour, LFM2's repetition loop, the unterminated-JSON rate) are only visible
in the per-note records.

## Regenerating

```bash
python3 harness/summarize.py            # report tables
python3 harness/export_dataset.py       # dataset.csv / dataset.json
python3 harness/verify_licences.py      # licence check against the Hub
python3 harness/capture_provenance.py   # provenance (needs network)
```

## Fields worth explaining before charting

| field | meaning |
|---|---|
| `f1_strict` | **the headline.** Both endpoints must name the labelled entity (surface variation absorbed by normalisation); only the predicate may vary. Measures extraction, full stop: it assumes no downstream entity resolution, because none is being measured |
| `f1_production` | what the drain would actually commit: floor applied |
| `f1_no_floor` | same extraction, `MF_CONF_FLOOR` lifted |
| `schema_rate` | fraction of notes where the model emitted `{"facts":[...]}` |
| `abstention_on_factless` | of the 23 notes asserting no durable fact, the fraction where the model correctly emitted nothing. **Only meaningful alongside a non-zero F1**: a model that outputs nothing anywhere scores 1.00 here |
| `dropped_by_conf_floor` | facts extracted correctly then discarded at the floor |
| `cpu_est_ms_per_note` | derived: 400-token prompt at `pp` + 48 generated at `tg` |

## Caveats that belong in any writeup

- **Hand-authored gold set.** 70 notes written for this benchmark, not sampled
  from the live corpus. Measures relative capability on a faithful task, not
  absolute production quality.
- **Small sample.** Differences under ~0.05 F1 are not meaningful at n=70.
- **Shared CPU host.** Speed runs are core-pinned but the physical machine also
  runs other containers. Treat CPU figures as ±20% and comparative.
- **Zero-shot only.** No fine-tuning was attempted on any model.
- **One task.** This is ontology-bound triple extraction with a fixed 17-relation
  vocabulary. It does not generalise to "which small model is best".
- **Two generations mixed.** Models tagged `superseded` in `models.json` are from
  the first pass, kept to show the generational delta. Do not chart them
  alongside current models without saying so. The first pass baselined the wrong
  incumbent (Gemma 3n E4B rather than Gemma 4 E4B) and that error is preserved in
  the record on purpose.
