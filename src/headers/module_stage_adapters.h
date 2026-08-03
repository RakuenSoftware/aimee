#ifndef AIMEE_SERVER_MODULE_STAGE_ADAPTERS_H
#define AIMEE_SERVER_MODULE_STAGE_ADAPTERS_H 1

/* Register every server-owned production seam with its separately supervised
 * process module. Calls fail closed; readiness keeps the listener out of
 * rotation until all required modules have attached to the local bus. */
void server_module_stage_adapters_configure(void);

/* The skills nudge has no reusable monolith service object, so its call site
 * invokes this adapter directly. Returns 0 on a valid module reply. */
int server_module_skill_should_fire(int hook_count, int interval, int *fire);

#endif
