# Embedder eval on aimee's OWN code (2026-06-22)

Task: NL doc-comment -> code-body retrieval over 1864 functions extracted from
aimee `src/**/*.c` (a leading block comment = query, the function signature+body =
the relevant doc; 1:1). Mirrors aimee's real operation: match a code unit's intent
(NL) to its body (`kb_curator_index_code_unit.c` intent_vec vs body_vec). Docs
capped to 2000 chars so each is one clean sequence. Harness: beir_cli.py via
llama-embedding (Vulkan, 7900XTX), **--no-escape** (code's literal \n/\t in string
literals were being expanded into newlines, splitting one prompt into many).

| model | nDCG@10 | Recall@10 | wall |
|---|---|---|---|
| nomic-embed-text-v1.5 (mean, 768-d) | 0.5700 | 0.7167 | 20s |
| Qwen3-Embedding-0.6B-f16 (last, 1024-d) | 0.6970 | 0.8165 | 217s |

Qwen3-0.6B wins by +12.7 nDCG@10 / +10 Recall@10 on aimee's own code, consistent
with the published MTEB code-retrieval gap (nomic is text-only, no code training).
~10x slower to embed (ingest-time cost). This is the decision-relevant axis: aimee
embeds raw code bodies/signatures, so the default embedder must be code-capable.

## + Qwen3-8B (high tier)
| model | nDCG@10 | Recall@10 | wall | dim |
|---|---|---|---|---|
| Qwen3-Embedding-8B-f16 (last) | 0.7608 | 0.8804 | 474.8s | 4096 |

8B beats 0.6B by +6.4 nDCG@10 / +6.4 Recall@10 on aimee code. ~2.2x slower than
0.6B (NOT 13x; GPU compute-bound, sublinear), ~24x nomic. Real cost is dimension:
4096-d = 4x pgvector storage/scan vs 0.6B's 1024-d, + 15GB VRAM resident.

## + Qwen3-4B (Q8) — the high-tier sweet spot
| model | nDCG@10 | Recall@10 | wall | dim |
|---|---|---|---|---|
| Qwen3-Embedding-4B-Q8 (last) | 0.7592 | 0.8739 | 343.3s | 2560 |

4B ~= 8B on nDCG (0.7592 vs 0.7608 = +0.16, noise) at 2560-d vs 4096-d (1.6x
smaller index) and 1.4x faster — and 4B here is Q8 vs 8B f16, so f16-4B likely
edges 8B. 0.6B->4B = +6.2 nDCG; 4B->8B = +0.16. => high tier should be 4B, not 8B.
Ladder: default Qwen3-0.6B (1024-d), high Qwen3-4B (2560-d). nomic dropped.

## 4B-f16 (settles the Q8 caveat + the 4B-vs-8B call)
| model | nDCG@10 | Recall@10 | wall | dim |
|---|---|---|---|---|
| Qwen3-4B-f16 (last) | 0.7592 | 0.8777 | 332.9s | 2560 |

4B-f16 nDCG == 4B-Q8 nDCG (0.7592) -> Q8 is lossless here. 4B-f16 vs 8B-f16:
8B leads by only +0.16 nDCG / +0.27 R@10 (noise). 8B's 4096-d (1.6x 4B's 2560-d
pgvector cost) + 1.4x embed time + 2x VRAM buys nothing. FINAL: default Qwen3-0.6B
(1024-d), high Qwen3-4B (2560-d, Q8 fine); nomic dropped.
