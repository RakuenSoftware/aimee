#ifndef DEC_KB_CURATOR_DRAIN_H
#define DEC_KB_CURATOR_DRAIN_H 1

#include <pthread.h>

typedef struct
{
   pthread_t thread;       /* LLM lane (GPU): extract/resolve/synthesize + sweeps */
   pthread_t index_thread; /* INDEX lane (CPU): embed/index + contradiction SQL */
   int active;
   int index_active;
   volatile int stop;
} kb_curator_drain_ctx_t;

/* Initialise and start the curator drain thread.
 * No-op if both kb_curator_extract_docs_enabled and
 * kb_curator_extract_code_enabled are 0 (default). */
void kb_curator_drain_init(kb_curator_drain_ctx_t *ctx);

/* Stop the drain thread and wait for it to exit. */
void kb_curator_drain_shutdown(kb_curator_drain_ctx_t *ctx);

/* Option B (single source of truth): the curator stage registry as JSON, for
 * the Pipeline GUI to render dynamically. Returns a fresh cJSON array
 * [{name,label,lane,budget,order,config_key}]; the caller owns it
 * (cJSON_Delete). Defined in kb_curator_drain.c beside CURATOR_STAGES. */
struct cJSON;
struct cJSON *kb_curator_stages_json(void);

#endif /* DEC_KB_CURATOR_DRAIN_H */
