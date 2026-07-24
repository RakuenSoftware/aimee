# Sampling capability matrix

Capability is based on request shaping in `src/server/agent_bridge.c:165-321`, `src/server/model_sampling.c`, and backend builders. `yes` means plumbing exists; `partial` means provider-specific or conditional; `no` means no verified plumbing.

| backend | temperature | top_p | max_tokens | stop | repetition_penalty | presence_penalty | frequency_penalty | min_p | stop_sequences | continuation/prefix |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| OpenAI Chat | yes | yes | yes | yes | no | yes | yes | no | yes | prompt-cache keying only |
| OpenAI Responses | yes | yes | yes (Responses intentionally omits the legacy field; `src/server/agent_request_build.c:62`) | partial | no | partial | partial | no | partial | `previous_response_id` plumbing not verified; Phase 4.0 |
| Anthropic | yes | yes | yes | yes | no | no | no | no | yes | native prompt-cache markers in `src/server/aimee_backend_anthropic.c:27-31`; native assistant-prefill not verified |
| Bedrock Converse | partial | partial | yes | partial | no | no | no | no | partial | provider-native continuation not verified |
| Ollama/llama-compatible | yes | yes | yes | partial | partial | no | no | partial (`top_k` precedent at `src/server/aimee_backend_openai.c:56`) | partial | prefix support provider-dependent |

**Phase 4 scope:** preserve only verified fields; missing repetition/presence/frequency/min_p and continuation plumbing are explicit Phase 4.0 prerequisites. Do not silently emulate unsupported controls.
