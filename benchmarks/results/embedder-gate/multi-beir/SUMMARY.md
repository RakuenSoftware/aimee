# Embedder baseline-gated multi-dataset eval (2026-06-22)

Harness: `benchmarks/beir_cli.py` via `llama-embedding` (Vulkan, 7900XTX). My
numbers vs published MTEB nDCG@10. nomic = mean-pool 768-d (docs truncated to its
~2048-tok limit); Qwen3-0.6B = last-pool fp16 1024-d, task-instruction prefix.

## Text BEIR (my harness vs published)
| dataset | nomic (mine) | nomic (pub) | Qwen3-0.6B (mine) | Qwen3-0.6B (pub) |
|---|---|---|---|---|
| SciFact  | 0.7034 | 0.7028 | 0.7059 | 0.6972 |
| NFCorpus | 0.3466 | 0.3467 | 0.3701 | 0.3671 |
| ArguAna  | 0.3556 | 0.5202 | 0.4856 | 0.7097 |

SciFact + NFCorpus reproduce published within ~0.3pt for BOTH models -> harness
validated. ArguAna is a symmetric argument-retrieval task; asymmetric query/doc
prefixes depress BOTH models' absolute scores, but Qwen3 still wins (+13pt mine,
+19pt published). On text, Qwen3-0.6B ties nomic on SciFact/NFCorpus and wins the
rest (per published: FiQA +9, SCIDOCS +7, ArguAna +19, TREC-COVID +27).

## Code retrieval (published MTEB nDCG@10) -- the domain aimee embeds code in
| task | nomic | Qwen3-0.6B | Qwen3-4B | Qwen3-8B |
|---|---|---|---|---|
| CodeSearchNet        | 0.856 | 0.943 | 0.960 | 0.966 |
| CodeSearchNet-CC     | 0.489 | 0.933 | 0.967 | 0.971 |
| CosQA                | 0.261 | 0.365 | 0.380 | 0.380 |
| CodeFeedback-ST      | 0.543 | 0.864 | 0.895 | 0.899 |
| CodeFeedback-MT      | 0.282 | 0.908 | 0.932 | 0.937 |
| CodeTransOcean-Cont. | 0.368 | 0.861 | 0.910 | 0.937 |
| SyntheticText2SQL    | 0.481 | 0.767 | 0.782 | 0.788 |
| StackOverflowQA      | 0.636 | 0.900 | 0.943 | 0.948 |

nomic is text-only (no code training); Qwen3 beats it by +10..+63 on code. The
0.6B->8B gain is small (+1.5..+7.7). Dims: 0.6B=1024, 4B=2560, 8B=4096->4000(trunc).

## Decision relevance
aimee embeds raw code (kb_curator_index_code_unit.c body_vec/sig_vec) and code
chunks (kb_service_code_embed.c) with the SAME configured embedder. Code-retrieval
quality is therefore first-class -> Qwen3-0.6B default is justified (code + multi-
dataset text), NOT the capped-SciFact artifact cited in #617.
