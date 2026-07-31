## GPU accuracy — production prompt

| model | params | licence | F1 strict | F1 lenient | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| google/gemma-4-E4B-it | E4B (4.5B eff) | apache-2.0 | 0.729 | 0.760 | 0.810 | 0.662 | 1.00 | 0.82 | 847.8 |
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.608 | 0.608 | 0.584 | 0.634 | 1.00 | 0.23 | 1760.4 |
| google/gemma-4-E2B-it | E2B (2.3B eff) | apache-2.0 | 0.599 | 0.642 | 0.621 | 0.578 | 0.99 | 0.67 | 629.8 |
| ibm-granite/granite-4.0-1b | 1B | apache-2.0 | 0.538 | 0.552 | 0.527 | 0.549 | 0.91 | 0.18 | 530.3 |
| ibm-granite/granite-4.1-3b | 3B | apache-2.0 | 0.515 | 0.530 | 0.557 | 0.479 | 1.00 | 0.55 | 465.5 |
| ibm-granite/granite-4.0-h-1b | 1B | apache-2.0 | 0.429 | 0.457 | 0.435 | 0.422 | 0.97 | 0.09 | 1180.7 |
| Qwen/Qwen3.5-0.8B | 800M | apache-2.0 | 0.377 | 0.377 | 0.571 | 0.282 | 1.00 | 0.77 | 552.3 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.319 | 0.362 | 0.652 | 0.211 | 0.99 | 0.95 | 433.0 |
| Qwen/Qwen3.5-2B | 2B | apache-2.0 | 0.298 | 0.328 | 0.318 | 0.282 | 1.00 | 0.32 | 557.8 |
| LiquidAI/LFM2-350M-Extract | 350M | lfm1.0 | 0.023 | 0.023 | 0.062 | 0.014 | 0.14 | 0.75 | 3384.3 |
| HuggingFaceTB/SmolLM2-360M-Instruct | 360M | apache-2.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 259.6 |
| LiquidAI/LFM2.5-230M | 230M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.66 | 1.00 | 298.1 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.97 | 1.00 | 359.2 |
| ibm-granite/granite-4.0-350m | 350M | apache-2.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.83 | 0.89 | 301.5 |
| ibm-granite/granite-4.0-h-350m | 350M | apache-2.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.31 | 1.00 | 740.2 |
| unsloth/gemma-3-270m-it | 270M | gemma | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 273.1 |


## GPU accuracy — confidence-literal ablation (NOT production)

| model | params | licence | F1 strict | F1 lenient | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| google/gemma-4-E4B-it | E4B (4.5B eff) | apache-2.0 | 0.694 | 0.711 | 0.737 | 0.656 | 1.00 | 0.83 | 856.9 |
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.628 | 0.628 | 0.589 | 0.672 | 1.00 | 0.30 | 1691.8 |
| ibm-granite/granite-4.0-1b | 1B | apache-2.0 | 0.553 | 0.553 | 0.506 | 0.609 | 0.96 | 0.10 | 466.7 |
| google/gemma-4-E2B-it | E2B (2.3B eff) | apache-2.0 | 0.548 | 0.563 | 0.521 | 0.578 | 0.97 | 0.38 | 741.1 |
| ibm-granite/granite-4.1-3b | 3B | apache-2.0 | 0.516 | 0.516 | 0.533 | 0.500 | 1.00 | 0.52 | 479.3 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.429 | 0.429 | 0.500 | 0.375 | 1.00 | 0.83 | 437.7 |
| ibm-granite/granite-4.0-h-1b | 1B | apache-2.0 | 0.406 | 0.406 | 0.378 | 0.438 | 0.99 | 0.00 | 1074.5 |
| Qwen/Qwen3.5-0.8B | 800M | apache-2.0 | 0.388 | 0.388 | 0.513 | 0.312 | 1.00 | 0.61 | 487.4 |
| Qwen/Qwen3.5-2B | 2B | apache-2.0 | 0.328 | 0.328 | 0.328 | 0.328 | 1.00 | 0.22 | 559.2 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.324 | 0.324 | 0.306 | 0.344 | 0.99 | 0.09 | 468.1 |
| LiquidAI/LFM2-350M-Extract | 350M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.13 | 0.33 | 3351.9 |
| ibm-granite/granite-4.0-350m | 350M | apache-2.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.94 | 0.74 | 252.9 |
| unsloth/gemma-3-270m-it | 270M | gemma | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 263.7 |


## CPU speed (llama.cpp, Q8_0, pinned cores)

| model | quant | pp t/s (400 tok prompt) | tg t/s (64 tok gen) | est ms/note |
|---|---|---:|---:|---:|
| HuggingFaceTB/SmolLM2-360M-Instruct-GGUF | llama 3B Q8_0 | 196.6 | 21.2 | 4304 |
| LiquidAI/LFM2-350M-Extract-GGUF | lfm2 350M Q8_0 | 269.9 | 36.2 | 2806 |
| Qwen/Qwen3-0.6B-GGUF | qwen3 0.6B Q8_0 | 136.1 | 15.2 | 6095 |
| Qwen/Qwen3-1.7B-GGUF | qwen3 1.7B Q8_0 | 53.5 | 10.4 | 12088 |
| ggml-org/Qwen3.5-0.8B-GGUF | qwen35 0.8B Q8_0 | 115.2 | 14.7 | 6747 |
| ggml-org/gemma-3-270m-GGUF | gemma3 270M Q8_0 | 504.3 | 31.2 | 2333 |
| ggml-org/gemma-4-E2B-it-GGUF | gemma4 E2B Q8_0 | 38.6 | 6.3 | 17985 |
| ggml-org/gemma-4-E4B-it-GGUF | gemma4 E4B Q8_0 | 19.4 | 3.3 | 35230 |
| ibm-granite/granite-4.0-1b-GGUF | granite 3B Q8_0 | 50.7 | 8.5 | 13568 |
| ibm-granite/granite-4.0-350m-GGUF | granite ?B Q8_0 | 246.3 | 29.4 | 3257 |
| ibm-granite/granite-4.0-h-1b-GGUF | granitehybrid 1B Q8_0 | 56.8 | 8.9 | 12428 |
| ibm-granite/granite-4.1-3b-GGUF | granite 3B Q8_0 | 25.1 | 4.2 | 27346 |
| unsloth/LFM2.5-230M-GGUF | lfm2 230M Q8_0 | 402.0 | 38.8 | 2233 |

