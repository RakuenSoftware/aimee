#ifndef AIMEE_KB_MODULE_STAGE_ADAPTERS_H
#define AIMEE_KB_MODULE_STAGE_ADAPTERS_H 1

typedef struct cJSON cJSON;

/* Install aimee-kb's production seams for separately supervised process
 * modules. Call once after configuring the daemon module runtime. */
void kb_module_stage_adapters_configure(void);

/* Query the separately supervised PostgreSQL module for generic store health.
 * Outputs are cleared on every failure. */
int kb_module_postgres_health_probe(int *schema_ok, int *have_pg_trgm, int *kb_tables_ok);

/* Invoke the shared Go memory module's JSON data stage in KB placement.  This
 * is a connection adapter only: policy and persistence live in the module.
 * Returns a newly allocated response object, or NULL on transport/refusal. */
cJSON *kb_module_memory_data(const cJSON *request);

int kb_module_memory_reset_derived(void);

int kb_module_memory_erase_subject(const char *request_id, const char *subject, const cJSON *sessions);

#endif
