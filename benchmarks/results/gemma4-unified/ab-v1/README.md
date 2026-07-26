# Gemma 4 unified baseline results

These artifacts use the frozen `ab-v1` suite manifest with SHA-256
`16d2c16add86052ff24be410699ab9452ee1a36252de6dba31ab5391de7ab81c`.

## Completed reranking controls

| Model | Cases | Success | MRR@10 | NDCG@10 | Recall@1 | Recall@5 | Recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Ettin 68M | 10,000 | 100% | 0.519094 | 0.607353 | 0.3300 | 0.7618 | 0.8832 |
| Ettin 400M | 10,000 | 100% | 0.562028 | 0.643879 | 0.3720 | 0.7996 | 0.8969 |

The raw files are append-only recovery logs. Consumers must select the last row
for each `case_id` before computing metrics. Ettin 68M has 10,014 rows for
10,000 unique cases; 14 failed attempts are superseded by successful retries.
Ettin 400M has 10,204 rows for 10,000 unique cases; 204 failed attempts are
superseded by successful retries. Every latest-per-case row is successful, and
the committed summaries were calculated from those final rows.

## Latency qualification status

The latency fields in these summaries are diagnostic only and must not be used
as clean qualification measurements. The append-only logs combine early
successful rows from the original serialized run (`physical_batch_size=512`)
with the corrected concurrent continuation and retries (`workers=8`,
`pairs_per_request=4`, `max_inflight_pairs=32`,
`physical_batch_size=2048`). Quality metrics remain comparable because the
same frozen cases and scoring procedure were used. A latency comparison
requires rerunning each control from an empty output directory under one fixed
load profile.

The hardware snapshots record the environment at completion. Partial artifacts
from the model sweep are intentionally excluded; a model is published here only
after all 10,000 cases have final results.

## Artifact hashes

| Artifact | SHA-256 |
| --- | --- |
| `ettin68m/hardware_reranking.json` | `0ac6ddeb72c6df3a25f082086a51d8c2fcba8b57348dfdb825743aed3e0c3754` |
| `ettin68m/raw_reranking_ettin68m.jsonl` | `ea15cda7838f49bb1ea092a685f79ae02b4a43898568347d1e72b6401ac59ee5` |
| `ettin68m/summary_reranking_ettin68m.json` | `a903b731d1762ba4902ada0ac35d56d3a34a2c189daa0c7b09627f3c68e9ceab` |
| `ettin400m/hardware_reranking.json` | `e46b9b4c3428ea51de43e65c8d5ebfc352126a2092e9a506856f971610616868` |
| `ettin400m/raw_reranking_ettin400m.jsonl` | `338288a81adb8b45355cfa47e3919ddd971e189b85895ca047256c3f734051d3` |
| `ettin400m/summary_reranking_ettin400m.json` | `b3238d38ec4013f4e398f9d4f25dde3b644b32f54bdda392e0098862e14c29f2` |
