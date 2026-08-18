#ifndef AIMEE_DB2_MODULE_ADAPTER_H
#define AIMEE_DB2_MODULE_ADAPTER_H 1

#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/db2/module_api.h>

typedef struct
{
   int (*is_initialized)(void);
   int (*health_probe)(int *schema_ok, int *have_pg_trgm);
   int (*kb_health_probe)(int *kb_tables_ok);
   int (*embedding_dimension)(void);
   int (*level3_count)(void);
   int (*level2_count)(void);
   int (*orphaned_l0_count)(void);
   int64_t (*total_count)(void);
   int (*pool_status)(aimee_db2_pool_status_t *status);
   int (*embedding_refusals)(aimee_db2_embedding_refusals_t *status);
   int (*postgres_status)(aimee_db2_postgres_status_t *status);
   int (*reembed_status)(aimee_db2_reembed_status_t *status);
   int (*reembed_clear)(void);
   int (*reembed_clear_maintenance)(int force, int *was_in_progress, int *recorded, int *running);
   const char *(*embedder_serving_id)(void);
   int (*dimension_reset)(uint32_t target_dimension, uint32_t force, uint32_t dry_run,
                          aimee_db2_dimension_reset_t *status);
} aimee_db2_module_backend_t;

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data);

#endif /* AIMEE_DB2_MODULE_ADAPTER_H */
