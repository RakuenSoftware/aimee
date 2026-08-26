## GPU accuracy: production prompt

| model | params | licence | F1 capability | F1 committed | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| google/gemma-4-E4B-it | E4B (4.5B eff) | apache-2.0 | 0.732 | 0.738 | 0.818 | 0.672 | 1.00 | 0.78 | 838.7 |
| ibm-granite/granite-4.1-3b | 3B | apache-2.0 | 0.647 | 0.571 | 0.610 | 0.537 | 1.00 | 0.57 | 459.4 |
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.639 | 0.643 | 0.605 | 0.687 | 1.00 | 0.22 | 1753.5 |
| ibm-granite/granite-4.0-1b | 1B | apache-2.0 | 0.592 | 0.600 | 0.575 | 0.627 | 0.91 | 0.17 | 529.9 |
| google/gemma-4-E2B-it | E2B (2.3B eff) | apache-2.0 | 0.579 | 0.646 | 0.667 | 0.627 | 0.99 | 0.64 | 622.1 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.567 | 0.400 | 0.783 | 0.269 | 0.99 | 0.95 | 428.8 |
| ibm-granite/granite-4.0-h-1b | 1B | apache-2.0 | 0.507 | 0.518 | 0.515 | 0.522 | 0.97 | 0.13 | 1170.7 |
| Qwen/Qwen3.5-0.8B | 800M | apache-2.0 | 0.438 | 0.416 | 0.618 | 0.313 | 1.00 | 0.74 | 545.2 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.403 | 0.000 | 0.000 | 0.000 | 0.97 | 1.00 | 359.0 |
| Qwen/Qwen3.5-2B | 2B | apache-2.0 | 0.324 | 0.341 | 0.355 | 0.328 | 1.00 | 0.30 | 557.8 |
| ibm-granite/granite-4.0-350m | 350M | apache-2.0 | 0.206 | 0.000 | 0.000 | 0.000 | 0.84 | 0.90 | 301.5 |
| ibm-granite/granite-4.0-h-350m | 350M | apache-2.0 | 0.136 | 0.000 | 0.000 | 0.000 | 0.32 | 1.00 | 722.7 |
| LiquidAI/LFM2-350M-Extract | 350M | lfm1.0 | 0.070 | 0.072 | 0.188 | 0.045 | 0.14 | 0.75 | 3384.3 |
| LiquidAI/LFM2.5-230M | 230M | lfm1.0 | 0.026 | 0.000 | 0.000 | 0.000 | 0.65 | 1.00 | 274.1 |
| HuggingFaceTB/SmolLM2-360M-Instruct | 360M | apache-2.0 | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 259.6 |
| unsloth/gemma-3-270m-it | 270M | gemma | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 270.8 |


## GPU accuracy: confidence-literal ablation (NOT production)

| model | params | licence | F1 capability | F1 committed | precision | recall | schema | abstain | med ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| google/gemma-4-E4B-it | E4B (4.5B eff) | apache-2.0 | 0.738 | 0.744 | 0.833 | 0.672 | 1.00 | 0.78 | 853.8 |
| ibm-granite/granite-4.1-3b | 3B | apache-2.0 | 0.662 | 0.576 | 0.621 | 0.537 | 1.00 | 0.57 | 474.5 |
| unsloth/gemma-3n-E4B-it | E4B | gemma | 0.647 | 0.647 | 0.625 | 0.672 | 1.00 | 0.30 | 1663.4 |
| google/gemma-4-E2B-it | E2B (2.3B eff) | apache-2.0 | 0.606 | 0.637 | 0.632 | 0.642 | 0.97 | 0.38 | 737.1 |
| ibm-granite/granite-4.0-1b | 1B | apache-2.0 | 0.597 | 0.601 | 0.566 | 0.642 | 0.96 | 0.10 | 466.7 |
| Qwen/Qwen3-1.7B | 1.7B | apache-2.0 | 0.514 | 0.504 | 0.604 | 0.433 | 1.00 | 0.87 | 434.4 |
| ibm-granite/granite-4.0-h-1b | 1B | apache-2.0 | 0.496 | 0.500 | 0.479 | 0.522 | 0.99 | 0.04 | 1071.4 |
| Qwen/Qwen3.5-0.8B | 800M | apache-2.0 | 0.449 | 0.438 | 0.605 | 0.343 | 1.00 | 0.65 | 485.0 |
| Qwen/Qwen3-0.6B | 600M | apache-2.0 | 0.437 | 0.435 | 0.422 | 0.448 | 0.99 | 0.09 | 468.1 |
| Qwen/Qwen3.5-2B | 2B | apache-2.0 | 0.316 | 0.323 | 0.333 | 0.313 | 1.00 | 0.22 | 559.2 |
| ibm-granite/granite-4.0-350m | 350M | apache-2.0 | 0.209 | 0.026 | 0.091 | 0.015 | 0.94 | 0.74 | 252.7 |
| LiquidAI/LFM2-350M-Extract | 350M | lfm1.0 | 0.081 | 0.081 | 0.429 | 0.045 | 0.13 | 0.33 | 3316.6 |
| unsloth/gemma-3-270m-it | 270M | gemma | 0.000 | 0.000 | 0.000 | 0.000 | 0.00 | None | 257.9 |


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

