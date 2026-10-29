#ifndef AIMEE_SERVER_MODULE_STAGE_ADAPTERS_H
#define AIMEE_SERVER_MODULE_STAGE_ADAPTERS_H 1

#include <aimee/benchmarks/module_api.h>

#include <stdint.h>

typedef struct cJSON cJSON;

/* Register every server-owned production seam with its separately supervised
 * process module. Calls fail closed; readiness keeps the listener out of
 * rotation until all required modules have attached to the local bus. */
void server_module_stage_adapters_configure(void);
void server_module_tools_adapters_configure(void);

/* The skills nudge has no reusable monolith service object, so its call site
 * invokes this adapter directly. Returns 0 on a valid module reply. */
int server_module_skill_should_fire(int hook_count, int interval, int *fire);

/* Invoke the Go memory module's scoped data stage. `request` must contain the
 * DATA JSON contract; this connection adapter forcibly supplies user scope so
 * the server cannot address workspace/project/KB rows through its local bus.
 * Returns a newly allocated JSON reply, or NULL when the module did not answer. */
cJSON *server_module_memory_data(const cJSON *request);

/* Complete configured derived-memory cleanup before acknowledging erasure. */
int server_module_memory_reset_derived(void);

int server_module_memory_erase_subject(const char *request_id, const char *subject,
                                       const cJSON *sessions);

#endif
