## GPU accuracy — production prompt

| model | params | licence | F1 prod | F1 no-floor | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| google/gemma-4-E4B-it | E4B (4.5B eff) | apache-2.0 | 0.705 | 0.699 | 0.729 | 0.672 | 1.00 | 0.83 | 847.8 |
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.624 | 0.620 | 0.564 | 0.688 | 0.94 | 0.05 | 1760.4 |
| ibm-granite/granite-4.1-3b | 3B | apache-2.0 | 0.512 | 0.580 | 0.540 | 0.625 | 1.00 | 0.52 | 465.5 |
| ibm-granite/granite-4.0-1b | 1B | apache-2.0 | 0.551 | 0.543 | 0.500 | 0.594 | 0.89 | 0.06 | 530.3 |
| google/gemma-4-E2B-it | E2B (2.3B eff) | apache-2.0 | 0.569 | 0.510 | 0.457 | 0.578 | 0.97 | 0.62 | 629.8 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.322 | 0.496 | 0.478 | 0.516 | 0.99 | 0.95 | 433.0 |
| ibm-granite/granite-4.0-h-1b | 1B | apache-2.0 | 0.421 | 0.412 | 0.389 | 0.438 | 0.97 | 0.09 | 1180.7 |
| Qwen/Qwen3.5-0.8B | 800M | apache-2.0 | 0.343 | 0.356 | 0.338 | 0.375 | 1.00 | 0.74 | 552.3 |
| Qwen/Qwen3.5-2B | 2B | apache-2.0 | 0.346 | 0.328 | 0.314 | 0.344 | 1.00 | 0.30 | 557.8 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.000 | 0.321 | 0.301 | 0.344 | 0.97 | 1.00 | 359.2 |
| ibm-granite/granite-4.0-350m | 350M | apache-2.0 | 0.000 | 0.179 | 0.186 | 0.172 | 0.83 | 0.85 | 301.5 |
| ibm-granite/granite-4.0-h-350m | 350M | apache-2.0 | 0.000 | 0.141 | 0.286 | 0.094 | 0.31 | 1.00 | 740.2 |
| HuggingFaceTB/SmolLM2-360M-Instruct | 360M | apache-2.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 259.6 |
| LiquidAI/LFM2-350M-Extract | 350M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.14 | 0.75 | 3384.3 |
| LiquidAI/LFM2.5-230M | 230M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.66 | 1.00 | 298.1 |
| unsloth/gemma-3-270m-it | 270M | gemma | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 273.1 |


## GPU accuracy — confidence-literal ablation (NOT production)

| model | params | licence | F1 prod | F1 no-floor | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| google/gemma-4-E4B-it | E4B (4.5B eff) | apache-2.0 | 0.711 | 0.705 | 0.741 | 0.672 | 1.00 | 0.83 | 856.9 |
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.628 | 0.628 | 0.589 | 0.672 | 0.90 | 0.00 | 1691.8 |
| ibm-granite/granite-4.1-3b | 3B | apache-2.0 | 0.516 | 0.594 | 0.554 | 0.641 | 1.00 | 0.52 | 479.3 |
| ibm-granite/granite-4.0-1b | 1B | apache-2.0 | 0.553 | 0.549 | 0.500 | 0.609 | 0.93 | 0.00 | 466.7 |
| google/gemma-4-E2B-it | E2B (2.3B eff) | apache-2.0 | 0.563 | 0.535 | 0.487 | 0.594 | 0.93 | 0.28 | 741.1 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.429 | 0.448 | 0.405 | 0.500 | 1.00 | 0.83 | 437.7 |
| ibm-granite/granite-4.0-h-1b | 1B | apache-2.0 | 0.406 | 0.403 | 0.373 | 0.438 | 0.99 | 0.00 | 1074.5 |
| Qwen/Qwen3.5-0.8B | 800M | apache-2.0 | 0.388 | 0.382 | 0.361 | 0.406 | 1.00 | 0.61 | 487.4 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.324 | 0.329 | 0.303 | 0.359 | 0.99 | 0.09 | 468.1 |
| Qwen/Qwen3.5-2B | 2B | apache-2.0 | 0.328 | 0.321 | 0.313 | 0.328 | 0.97 | 0.14 | 559.2 |
| ibm-granite/granite-4.0-350m | 350M | apache-2.0 | 0.000 | 0.180 | 0.174 | 0.188 | 0.94 | 0.74 | 252.9 |
| LiquidAI/LFM2-350M-Extract | 350M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.13 | 0.33 | 3351.9 |
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

