#ifndef DEC_KB_HTTP_CODE_VECTOR_STATUS_H
#define DEC_KB_HTTP_CODE_VECTOR_STATUS_H 1

#include "cJSON.h"

#include <string.h>

typedef struct
{
   const char *status;
   const char *dependency;
   int observed_dimension;
   int current_dimension;
   int retry_after_ms;
} kb_code_vector_status_t;

#define KB_CODE_VECTOR_RETRY_AFTER_MS     1000
#define KB_CODE_VECTOR_STATUS_INITIALIZER {"disabled", NULL, 0, 0, 0}

static inline void kb_code_vector_status_store(kb_code_vector_status_t *state, int query_rc,
                                               int result_count)
{
   if (query_rc < 0)
   {
      state->status = "unavailable";
      state->dependency = "vector_store";
      state->retry_after_ms = KB_CODE_VECTOR_RETRY_AFTER_MS;
   }
   else
      state->status = result_count > 0 ? "ok" : "empty";
}

static inline void kb_code_vector_status_embed(kb_code_vector_status_t *state, const char *command,
                                               int observed_dimension, int current_dimension,
                                               int unauthorized)
{
   state->status = observed_dimension <= 0 ? (command && strcmp(command, "builtin") == 0
                                                  ? "disabled"
                                                  : (unauthorized ? "unauthorized" : "unavailable"))
                                           : "stale";
   if (strcmp(state->status, "disabled") != 0)
   {
      state->dependency = "embedder";
      state->observed_dimension = observed_dimension;
      state->current_dimension = current_dimension;
      if (strcmp(state->status, "unavailable") == 0)
         state->retry_after_ms = KB_CODE_VECTOR_RETRY_AFTER_MS;
   }
}

static inline void kb_code_vector_status_add_json(cJSON *object,
                                                  const kb_code_vector_status_t *state)
{
   cJSON_AddStringToObject(object, "vector_status", state->status);
   if (state->dependency)
      cJSON_AddStringToObject(object, "vector_dependency", state->dependency);
   if (state->observed_dimension > 0)
      cJSON_AddNumberToObject(object, "vector_observed_dimension", state->observed_dimension);
   if (state->current_dimension > 0)
      cJSON_AddNumberToObject(object, "vector_current_dimension", state->current_dimension);
   if (state->retry_after_ms > 0)
      cJSON_AddNumberToObject(object, "vector_retry_after_ms", state->retry_after_ms);
}

#endif /* DEC_KB_HTTP_CODE_VECTOR_STATUS_H */
