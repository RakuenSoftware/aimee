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

## Completed Gemma 4 E2B baseline

The stock, untrained Gemma 4 E2B instruction checkpoint completed both applicable
10,000-case views. This is a pre-training control, not evidence that the model is
ready to replace the supported embedder or synthesis route.

| Synthesis cases | Request success | Raw JSON parse | Schema valid | Required-field recall | Content F1 | p50 latency | p95 latency |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 100% | 99.79% | 97.41% | 99.00% | 0.256746 | 10.69 s | 37.66 s |

| Embedding cases | Native width | MRR@10 | NDCG@10 | Recall@1 | Recall@5 | Recall@10 | Vectors/s |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 1,536 | 0.269053 | 0.362803 | 0.1380 | 0.4318 | 0.6735 | 5.4347 |

Both views ran at 64 parallel slots with 131,072 aggregate context tokens,
2,048 physical-batch tokens, and a bounded 512 MiB prompt cache. Synthesis used
64 client workers; embedding used batches of 64. The completion snapshots record
approximately 4.23 GiB VRAM for synthesis and 6.22 GiB for embedding. Cold load
was 12.67 and 12.74 seconds respectively. Each raw file contains exactly 10,000
unique, successful final rows.

The generated response for case `9490bd93bed2a6ceabb59f3f` matched the
credential-like syntax scanner after scoring. The committed row replaces only
that response text with `<REDACTED_GENERATED_RESPONSE>`, records
`response_redacted: true` and the original response SHA-256, and preserves its
pre-redaction metrics and telemetry. The authorized benchmark host retains the
same score-preserving redacted checkpoint used here; the credential-shaped text
is not retained in either publishable result copy.

Each completed view also includes `validation_<view>.json`. These acceptance
records prove the exact frozen case population, latest-row success, metric
reproduction, suite identity, required hardware snapshot, artifact hashes, and
a passing secret scan.

`ARTIFACTS.json` freezes the locally verified sizes and SHA-256 digests of all six
model files before the sweep. E2B was
`gemma-4-E2B-it-Q4_0.gguf` at SHA-256
`8e30dff3ac4c8434c49a7036fa15564bdbb6044e42bf04550bf1a096ad7e6a52`.

## Additional completed view checkpoints

Gemma 4 E4B completed the embedding view at its native 2,560 dimensions. Its
synthesis view is intentionally absent from this checkpoint because four frozen
cases do not yet have successful final rows.

| Embedding model | Cases | Native width | MRR@10 | NDCG@10 | Recall@1 | Recall@5 | Recall@10 | Vectors/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Gemma 4 E4B | 10,000 | 2,560 | 0.328657 | 0.422186 | 0.1832 | 0.5205 | 0.7275 | 2.2839 |

The E4B raw embedding file contains exactly 10,000 unique frozen case IDs. Its
raw metric means reproduce every summary value, and both artifacts use suite
manifest SHA-256 `16d2c16add86052ff24be410699ab9452ee1a36252de6dba31ab5391de7ab81c`.
The 64-slot/64-input profile used 131,072 aggregate context tokens and a 2,048-token
physical batch. The completion snapshot records approximately 12.34 GiB VRAM and
a 10.75-second cold load.

Gemma 4 12B completed the synthesis view. Its append-only raw log has 10,013 rows:
13 failed rows from the interrupted pass are superseded by successful retries,
leaving exactly 10,000 unique successful final cases. Recomputing the summary
from those latest rows reproduces the published summary exactly.

| Synthesis model | Cases | Request success | Raw JSON parse | Schema valid | Required-field recall | Content F1 | p50 latency | p95 latency |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Gemma 4 12B | 10,000 | 100% | 100% | 99.96% | 97.57% | 0.327918 | 42.39 s | 91.84 s |

The 12B synthesis view used 32 workers/slots, 65,536 aggregate context tokens,
and a 2,048-token physical batch. Its completion snapshot records approximately
23.77 GiB VRAM and a 40.94-second cold load. The 12B embedding view remains outside
this checkpoint until its own 10,000-case summary is complete.

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
| `ARTIFACTS.json` | `382550550e50f8283512658f87903951e6e1ae88088e78d5ede618923d3f8433` |
| `ettin68m/hardware_reranking.json` | `0ac6ddeb72c6df3a25f082086a51d8c2fcba8b57348dfdb825743aed3e0c3754` |
| `ettin68m/raw_reranking_ettin68m.jsonl` | `ea15cda7838f49bb1ea092a685f79ae02b4a43898568347d1e72b6401ac59ee5` |
| `ettin68m/summary_reranking_ettin68m.json` | `a903b731d1762ba4902ada0ac35d56d3a34a2c189daa0c7b09627f3c68e9ceab` |
| `ettin68m/validation_reranking.json` | `6fedf2d5dd6ced671164f28eeb8e1cae3cb49f7e74956e509685fcd02c9bc897` |
| `ettin400m/hardware_reranking.json` | `e46b9b4c3428ea51de43e65c8d5ebfc352126a2092e9a506856f971610616868` |
| `ettin400m/raw_reranking_ettin400m.jsonl` | `338288a81adb8b45355cfa47e3919ddd971e189b85895ca047256c3f734051d3` |
| `ettin400m/summary_reranking_ettin400m.json` | `b3238d38ec4013f4e398f9d4f25dde3b644b32f54bdda392e0098862e14c29f2` |
| `ettin400m/validation_reranking.json` | `1e30b3ddf668b6c1085e81696abf1648ee13d79796f68e686541019748d5449f` |
| `gemma4_e2b/hardware_embedding.json` | `8571642ef9c0909f8a5eba44a0fe569bb297ffb1e4c2265b2b3c4aacf62fec50` |
| `gemma4_e2b/hardware_synthesis.json` | `29f82375841c12f56c0585c9c71b4a6aefc47cfa9ca5516a76f5a7d47570475d` |
| `gemma4_e2b/raw_embedding_gemma4_e2b.jsonl` | `30a9da053ce28643d11aad9b0c2dbc8a97af99184726f8fc8244f71c68255371` |
| `gemma4_e2b/raw_gemma4_e2b.jsonl` | `c1e9470649901a5893e98a9f2282209739518411e2190b9190fd90fce6eaf137` |
| `gemma4_e2b/summary_embedding_gemma4_e2b.json` | `a7c0cf0019a4ffbed8bcf9e58d1e6e5bb33c3315d47f4c3d269ce91e854bf04f` |
| `gemma4_e2b/summary_gemma4_e2b.json` | `aa372da4d602d201a132c3a3bda6e10ddb538f502c093bd7207137dc1b618c32` |
| `gemma4_e2b/validation_embedding.json` | `4737d80f203489c41230fd720da01a065f4e26eb5887fff3376f355740a99a3f` |
| `gemma4_e2b/validation_synthesis.json` | `066d75a13929b1fabd4ca62e9de4606e43d9aa2a0f6ae0461b991d4c3ee20740` |
| `gemma4_e4b/hardware_embedding.json` | `e2edd2cc526cff329be4a278d1acfd75d7f456edaf350066e4d55916a1989b83` |
| `gemma4_e4b/raw_embedding_gemma4_e4b.jsonl` | `891dc7e8e827d4b5562fd4395b9f840a639d25ce32f6c920cc3d6e9d5a1b1a52` |
| `gemma4_e4b/summary_embedding_gemma4_e4b.json` | `8b41cb5d9427ab72daa032537bd820f8ebc634036d88192d205ff593cf2aa874` |
| `gemma4_e4b/validation_embedding.json` | `7fd92b8b19f0c1318fd8a871f54473500a4f7bf50037824f01a70d63b958c0bd` |
| `gemma4_12b/hardware_synthesis.json` | `a046d9197ee7eb5da0166f9671e226205e4655a605e111bddc9f84f9e7c0f879` |
| `gemma4_12b/raw_gemma4_12b.jsonl` | `d5299e3e1b5b59de80da0f0d8ef57fd4d2cb13c68d2621fdad7dc781a7139ee4` |
| `gemma4_12b/summary_gemma4_12b.json` | `d00141a350bc78083b6a932b2673f6a578ee39dc7510b8ffcb3ec5039ab46060` |
| `gemma4_12b/validation_synthesis.json` | `8be1285bcf31d80584148a747aa41f6a5c66fd6190feaf1a65f6dee6fcb5e54a` |
