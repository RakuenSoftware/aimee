# Sampling capability matrix

Capability is based on the IR backend builders and the curated local-model overlay. `yes` means verified request plumbing; `conditional` means only the opt-in model sampling overlay supplies it; `no` means no verified plumbing.

| backend | temperature | top_p | max_tokens | stop | repetition_penalty | presence_penalty | frequency_penalty | min_p | stop_sequences | continuation/prefix |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| OpenAI Chat | yes | yes | yes | yes | conditional (`repeat_penalty`) | no | no | conditional | yes (`stop`) | no verified prompt-cache key or assistant-prefill |
| OpenAI Responses | yes | yes | no by design | no | conditional (`repeat_penalty`) | no | no | conditional | no | `previous_response_id` supported |
| Anthropic | yes | yes | yes | yes | no | no | no | no | yes (`stop_sequences`) | prompt-cache system markers; no native assistant-prefill plumbing |
| Bedrock Converse | yes | yes | yes | yes | no | no | no | no | yes (`stopSequences`) | no verified continuation/prefix primitive |
| Ollama/llama-compatible | yes | yes | yes | yes | conditional (`repeat_penalty`) | no | no | conditional | yes (`stop`) | no verified continuation/prefix primitive |

Citations: OpenAI fields and stop translation are `src/server/aimee_backend_openai.c:50-57,163-170`; Anthropic fields and stop sequences are `src/server/aimee_backend_anthropic.c:183-190,234-239`; Bedrock Converse inference fields are `src/server/aimee_backend_bedrock.c:421-444`. The opt-in OpenAI-compatible overlay adds `top_p`, `top_k`, `min_p`, and provider-spelled `repeat_penalty` at `src/server/model_sampling.c:71-88`. Responses deliberately omits `max_tokens` at `src/server/agent_request_build.c:64`, and continuation reads `previous_response_id` at `src/server/openai_chat.c:622` (shape parsing at `src/server/openai_shape.c:216`). Anthropic prompt-cache shaping is applied at `src/server/agent_request_build.c:75-90`.

## Phase 4.0 missing plumbing

Before Phase 4 can vary an unsupported control, Phase 4.0 must add and test (starting test sites: `src/tests/test_aimee_backend.c` for OpenAI + Anthropic backend builders, and `src/tests/test_aimee_backend_bedrock.c` for the Bedrock Converse builder; per-backend split files do not exist today and Phase 4.0 introduces them if coverage gaps warrant):

- OpenAI Chat/Responses and Ollama: canonical `repetition_penalty` mapping; only provider-specific `repeat_penalty` exists at `src/server/model_sampling.c:88`.
- All backends: `presence_penalty` and `frequency_penalty`; no backend builder emits either in the cited builder ranges.
- Anthropic and Bedrock: `min_p`; only the OpenAI-compatible overlay emits it at `src/server/model_sampling.c:87`.
- OpenAI Responses: `max_tokens`, stop/stop-sequence mapping, and any provider-supported repetition controls; its max-token omission is explicit at `src/server/agent_request_build.c:64`.
- OpenAI Chat/Responses and Ollama: prompt-cache keying and native assistant-prefill, if required; neither is emitted by `src/server/aimee_backend_openai.c:43-170`.
- Anthropic: native assistant-prefill, if required; current continuation support is limited to prompt-cache system shaping at `src/server/agent_request_build.c:75-90`.
- Bedrock Converse: provider-native continuation/prefix plumbing, if required; the current request builder surface is `src/server/aimee_backend_bedrock.c:366-444`.

**Phase 4 scope:** preserve verified fields only. Do not silently emulate unsupported controls.
