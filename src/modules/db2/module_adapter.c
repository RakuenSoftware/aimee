#include "module_adapter.h"

#include <aimee/db2/module_api.h>

#include "c/db2.h"

static const aimee_db2_module_backend_t *production_backend(void)
{
   static const aimee_db2_module_backend_t backend = {
       .health_probe = db2_health_probe,
       .kb_health_probe = db2_kb_health_probe,
   };
   return &backend;
}

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   if (response_len)
      *response_len = 0;
   if (!invocation || !response_len || invocation->stage_id != AIMEE_DB2_STAGE_HEALTH ||
       !response_body || response_capacity < AIMEE_DB2_RESPONSE_LEN ||
       aimee_db2_health_request_decode(request_body, request_len) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   const aimee_db2_module_backend_t *backend = user_data;
   if (!backend)
      backend = production_backend();
   if (!backend || !backend->health_probe || !backend->kb_health_probe)
      return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;

   int schema_ok = 0, have_pg_trgm = 0, kb_tables_ok = 0;
   if (backend->health_probe(&schema_ok, &have_pg_trgm) != 0 ||
       backend->kb_health_probe(&kb_tables_ok) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   uint32_t flags = (schema_ok ? AIMEE_DB2_FLAG_SCHEMA : 0u) |
                    (have_pg_trgm ? AIMEE_DB2_FLAG_PG_TRGM : 0u) |
                    (kb_tables_ok ? AIMEE_DB2_FLAG_KB_TABLES : 0u);
   if (aimee_db2_health_response_encode(flags, response_body, response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_DB2_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
