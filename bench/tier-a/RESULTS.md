## GPU accuracy — production prompt

| model | params | licence | F1 prod | F1 no-floor | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.582 | 0.578 | 0.526 | 0.641 | 0.94 | 0.05 | 1760.4 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.253 | 0.421 | 0.406 | 0.438 | 0.99 | 0.95 | 433.0 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.000 | 0.277 | 0.260 | 0.297 | 0.97 | 1.00 | 359.2 |
| ibm-granite/granite-4.0-350m | 350M | apache-2.0 | 0.000 | 0.179 | 0.186 | 0.172 | 0.83 | 0.85 | 301.5 |
| ibm-granite/granite-4.0-h-350m | 350M | apache-2.0 | 0.000 | 0.141 | 0.286 | 0.094 | 0.31 | 1.00 | 740.2 |
| HuggingFaceTB/SmolLM2-360M-Instruct | 360M | apache-2.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 259.6 |
| LiquidAI/LFM2-350M-Extract | 350M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.14 | 0.75 | 3384.3 |
| LiquidAI/LFM2.5-230M | 230M | lfm1.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.66 | 1.00 | 298.1 |
| unsloth/gemma-3-270m-it | 270M | gemma | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 273.1 |


## GPU accuracy — confidence-literal ablation (NOT production)

| model | params | licence | F1 prod | F1 no-floor | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.584 | 0.584 | 0.548 | 0.625 | 0.90 | 0.00 | 1691.8 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.357 | 0.378 | 0.342 | 0.422 | 1.00 | 0.83 | 437.7 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.279 | 0.286 | 0.263 | 0.312 | 0.99 | 0.09 | 468.1 |
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
| ggml-org/gemma-3-270m-GGUF | gemma3 270M Q8_0 | 504.3 | 31.2 | 2333 |
| ibm-granite/granite-4.0-350m-GGUF | granite ?B Q8_0 | 246.3 | 29.4 | 3257 |
| unsloth/LFM2.5-230M-GGUF | lfm2 230M Q8_0 | 402.0 | 38.8 | 2233 |

