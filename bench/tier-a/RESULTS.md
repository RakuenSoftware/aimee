## GPU accuracy — production prompt

| model | params | licence | F1 prod | F1 no-floor | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| google/gemma-4-E4B-it | E4B (4.5B eff) | apache-2.0 | 0.656 | 0.650 | 0.678 | 0.625 | 1.00 | 0.83 | 847.8 |
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.610 | 0.606 | 0.551 | 0.672 | 0.94 | 0.05 | 1760.4 |
| ibm-granite/granite-4.1-3b | 3B | apache-2.0 | 0.512 | 0.580 | 0.540 | 0.625 | 1.00 | 0.52 | 465.5 |
| google/gemma-4-E2B-it | E2B (2.3B eff) | apache-2.0 | 0.554 | 0.497 | 0.444 | 0.562 | 0.97 | 0.62 | 629.8 |
| ibm-granite/granite-4.0-1b | 1B | apache-2.0 | 0.493 | 0.486 | 0.447 | 0.531 | 0.89 | 0.06 | 530.3 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.276 | 0.451 | 0.435 | 0.469 | 0.99 | 0.95 | 433.0 |
| ibm-granite/granite-4.0-h-1b | 1B | apache-2.0 | 0.391 | 0.382 | 0.361 | 0.406 | 0.97 | 0.09 | 1180.7 |
| Qwen/Qwen3.5-0.8B | 800M | apache-2.0 | 0.323 | 0.341 | 0.324 | 0.359 | 1.00 | 0.74 | 552.3 |
| Qwen/Qwen3.5-2B | 2B | apache-2.0 | 0.331 | 0.313 | 0.300 | 0.328 | 1.00 | 0.30 | 557.8 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.000 | 0.292 | 0.274 | 0.312 | 0.97 | 1.00 | 359.2 |
| ibm-granite/granite-4.0-350m | 350M | apache-2.0 | 0.000 | 0.179 | 0.186 | 0.172 | 0.83 | 0.85 | 301.5 |
| ibm-granite/granite-4.0-h-350m | 350M | apache-2.0 | 0.000 | 0.141 | 0.286 | 0.094 | 0.31 | 1.00 | 740.2 |
| HuggingFaceTB/SmolLM2-360M-Instruct | 360M | apache-2.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 259.6 |
| LiquidAI/LFM2-350M-Extract | 350M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.14 | 0.75 | 3384.3 |
| LiquidAI/LFM2.5-230M | 230M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.66 | 1.00 | 298.1 |
| unsloth/gemma-3-270m-it | 270M | gemma | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 273.1 |


## GPU accuracy — confidence-literal ablation (NOT production)

| model | params | licence | F1 prod | F1 no-floor | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| google/gemma-4-E4B-it | E4B (4.5B eff) | apache-2.0 | 0.661 | 0.656 | 0.690 | 0.625 | 1.00 | 0.83 | 856.9 |
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.613 | 0.613 | 0.575 | 0.656 | 0.90 | 0.00 | 1691.8 |
| ibm-granite/granite-4.1-3b | 3B | apache-2.0 | 0.516 | 0.594 | 0.554 | 0.641 | 1.00 | 0.52 | 479.3 |
| google/gemma-4-E2B-it | E2B (2.3B eff) | apache-2.0 | 0.548 | 0.521 | 0.474 | 0.578 | 0.93 | 0.28 | 741.1 |
| ibm-granite/granite-4.0-1b | 1B | apache-2.0 | 0.496 | 0.493 | 0.449 | 0.547 | 0.93 | 0.00 | 466.7 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.393 | 0.406 | 0.367 | 0.453 | 1.00 | 0.83 | 437.7 |
| ibm-granite/granite-4.0-h-1b | 1B | apache-2.0 | 0.377 | 0.374 | 0.347 | 0.406 | 0.99 | 0.00 | 1074.5 |
| Qwen/Qwen3.5-0.8B | 800M | apache-2.0 | 0.369 | 0.368 | 0.347 | 0.391 | 1.00 | 0.61 | 487.4 |
| Qwen/Qwen3.5-2B | 2B | apache-2.0 | 0.312 | 0.305 | 0.298 | 0.312 | 0.97 | 0.14 | 559.2 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.294 | 0.300 | 0.276 | 0.328 | 0.99 | 0.09 | 468.1 |
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

