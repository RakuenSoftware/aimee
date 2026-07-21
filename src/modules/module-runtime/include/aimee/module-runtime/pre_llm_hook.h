/* pre_llm_hook.h: required in-process PRE_LLM_CALL hook registry.
 *
 * Part of the pluggable context engine surface described in
 * docs/proposals/pending/plugin-extension-surface-and-context-engine.md.
 *
 * Distinct from the shell-script hook system (plugin_hook_t / cmd_hooks.c).
 * C hooks are registered by in-process plugin code and run synchronously.
 *
 * PRE_LLM_CALL:
 *   - Called just before each LLM request is sent.
 *   - May return ephemeral context text appended to the user message.
 *   - The system prompt is NEVER passed to or modified by these hooks —
 *     this preserves the byte-stable prefix required for prompt caching.
 */
#ifndef DEC_PLUGIN_C_HOOK_H
#define DEC_PLUGIN_C_HOOK_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PLUGIN_C_HOOK_MAX 32 /* max registered hooks per kind */

   /* Function pointer type for PRE_LLM_CALL hooks.
    *   user_msg  — the current user message (read-only; never the system prompt)
    *   out       — buffer to write ephemeral context into (appended to user msg)
    *   out_len   — size of out buffer
    *   user_data — opaque pointer passed at registration
    * Return 0 to include out in the ephemeral context; -1 to skip. */
   typedef int (*plugin_chook_pre_llm_fn)(const char *user_msg, char *out, size_t out_len,
                                          void *user_data);

   /* Register a PRE_LLM_CALL hook.
    * Returns 0 on success, -1 if the registry is full. */
   int plugin_chook_register_pre_llm(plugin_chook_pre_llm_fn fn, void *user_data);

   /* Run all registered PRE_LLM_CALL hooks.
    * Each hook's output is appended to ephemeral_out separated by '\n'.
    * The system_prompt parameter is deliberately absent — hooks never see or
    * modify it; callers pass only the user message.
    * Returns the number of hooks whose output was included (≥ 0). */
   int plugin_chook_run_pre_llm(const char *user_msg, char *ephemeral_out, size_t out_len);

   /* Apply PRE_LLM_CALL hooks to user_msg.
    * If any hook returned ephemeral context, returns a heap-allocated string
    * "user_msg\nephemeral_ctx"; caller must free() it.
    * If no hooks fired or all returned empty/error, returns NULL (caller uses
    * user_msg unchanged — no free needed).
    * The system_prompt is NEVER passed in or modified. */
   char *plugin_chook_apply_pre_llm(const char *user_msg);

   /* Reset the registry (for tests only). */
   void plugin_chook_reset(void);

   /* Return the count of registered PRE_LLM_CALL hooks. */
   int plugin_chook_pre_llm_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_PLUGIN_C_HOOK_H */
