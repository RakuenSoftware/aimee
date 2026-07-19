/* tool_call_args.h: helpers for assistant tool_call arguments. */
#ifndef AIMEE_TOOL_CALL_ARGS_H
#define AIMEE_TOOL_CALL_ARGS_H

#ifdef __cplusplus
extern "C"
{
#endif

   struct cJSON;

   /* Force the indexed tool_call's function.arguments field to be the given
    * JSON-encoded string. Adds the field when absent, replaces otherwise. */
   void tool_call_normalize_assistant_arguments(struct cJSON *assistant_message, int index,
                                                const char *arguments);

   /* Walk every tool_call on the assistant message and coerce any missing,
    * non-string, or unparseable function.arguments field to a valid JSON
    * string ("{}" by default). Idempotent. Required to satisfy strict
    * OpenAI-compat providers such as MiniMax-M2.7. */
   void tool_call_sanitize_assistant_arguments(struct cJSON *assistant_message);

#ifdef __cplusplus
}
#endif

#endif
