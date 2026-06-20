/* memory_rewrite_llm.h: in-process HyDE / decomposition query rewrite.
 *
 * The rewrite runs the query through the curator LLM dispatch (per-tier
 * provider, i.e. a small fast local model on the curator-llm sidecar) with no
 * subprocess. It is defined only in the KB build, which links the curator LLM +
 * provider client. memory_core.c is also compiled into the server/CLI targets
 * that do NOT link those, so the symbol is declared #pragma weak: it resolves to
 * NULL there and memory_query_rewrite falls back to the legacy subprocess
 * command. At runtime the rewrite only ever executes KB-side (it is reached
 * through the memory.find_facts_scoped RPC), so the in-process path is what runs
 * in production. */
#ifndef AIMEE_MEMORY_REWRITE_LLM_H
#define AIMEE_MEMORY_REWRITE_LLM_H

#include "config.h"

/* Run `system_prompt` (the rewrite instructions) plus `query` (the user turn)
 * through the curator LLM. Returns the model's response text (malloc'd, caller
 * frees) or NULL on error / when no provider is configured. */
char *memory_rewrite_llm_inproc(const config_t *cfg, const char *system_prompt, const char *query);

#if defined(__GNUC__) || defined(__clang__)
#pragma weak memory_rewrite_llm_inproc
#endif

#endif /* AIMEE_MEMORY_REWRITE_LLM_H */
