/* tool_args_coerce.h: best-effort coercion of model-emitted tool-call
 * arguments against their declared JSON schema. Implements section 4 of
 * docs/proposals/pending/provider-profile-registry-and-auxiliary-models.md. */
#ifndef DEC_TOOL_ARGS_COERCE_H
#define DEC_TOOL_ARGS_COERCE_H 1

#include "cJSON.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Returns a NEWLY-ALLOCATED cJSON value the caller must cJSON_Delete.
    * Never modifies inputs. NULL only on allocation failure; otherwise
    * returns at minimum a deep clone of raw_args. */
   cJSON *tool_args_coerce(const cJSON *declared_schema, const cJSON *raw_args);

   /* The ONE place a tool call's argument shape is decided: alias resolution,
    * named integer-field coercion, then the schema coercion above.
    *
    * Callers must canonicalize BEFORE authorization, then pass the same result
    * to every gate, to the executor, and to the audit record. Authorizing one
    * shape and executing another is the defect this exists to prevent.
    *
    * Idempotent: canonicalizing an already-canonical object is a no-op, so a
    * second call on a deeper layer is safe.
    *
    * Both return a NEWLY-ALLOCATED value the caller frees (cJSON_Delete /
    * free). tool_args_canonicalize_json returns NULL when raw_args_json is not
    * valid JSON, which the validator reports; the caller should then fall back
    * to the raw string so the error surfaces there rather than here. */
   cJSON *tool_args_canonicalize(const cJSON *declared_schema, const cJSON *raw_args);
   char *tool_args_canonicalize_json(const cJSON *declared_schema, const char *raw_args_json);

#ifdef __cplusplus
}
#endif

#endif /* DEC_TOOL_ARGS_COERCE_H */
