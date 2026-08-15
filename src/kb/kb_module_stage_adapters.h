#ifndef AIMEE_KB_MODULE_STAGE_ADAPTERS_H
#define AIMEE_KB_MODULE_STAGE_ADAPTERS_H 1

/* Install aimee-kb's production seams for separately supervised process
 * modules. Call once after configuring the daemon module runtime. */
void kb_module_stage_adapters_configure(void);

/* Query the separately supervised PostgreSQL module for generic store health.
 * Outputs are cleared on every failure. */
int kb_module_postgres_health_probe(int *schema_ok, int *have_pg_trgm);

#endif
