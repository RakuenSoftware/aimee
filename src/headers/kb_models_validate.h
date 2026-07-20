/* kb_models_validate.h: pure input validators for the P2a /v1/models routes.
 *
 * No storage or transport dependency (string logic only), so the admission checks
 * are unit-testable in isolation from the db2/PG layer. Shared by kb_http_models.c
 * (route admission) and the models-validate unit test. */
#ifndef DEC_KB_MODELS_VALIDATE_H
#define DEC_KB_MODELS_VALIDATE_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* The catalog wire whitelist (mirrors the schema CHECK). Returns 1 iff `wire` is
    * exactly one of anthropic|openai|responses|gemini. NULL -> 0. */
   int kb_models_wire_valid(const char *wire);

   /* A model_id / provider is printable text of bounded length: reject empty, control
    * chars, DEL, and over-length so a name can't smuggle control bytes into stored
    * state. `max` is the inclusive upper bound. Returns 1 when clean, else 0. */
   int kb_models_name_clean(const char *s, int max);

   /* A catalog endpoint override. Empty is VALID (the provider default is used). A
    * non-empty endpoint must be <= max chars, contain no control/DEL bytes, and begin
    * with http:// or https:// (so a bare host or a non-URL scheme can't be stored). NULL
    * -> 0. `max` is the inclusive upper bound (500 for the org_model_catalog column). */
   int kb_models_endpoint_valid(const char *endpoint, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_MODELS_VALIDATE_H */
