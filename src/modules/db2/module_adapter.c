#include "module_adapter.h"

#include <aimee/db2/module_api.h>

#include "c/db2.h"
#include "c/db2_pool.h"

static int production_pool_status(aimee_db2_pool_status_t *status)
{
   int size = 0, in_use = 0, waiters = 0;
   long grants = 0, timeouts = 0, stuck = 0, poisoned = 0;
   db2_pool_stats(&size, &in_use, &waiters, &grants, &timeouts, &stuck, &poisoned);
   if (!status || size < 1 || size > (int)AIMEE_DB2_POOL_SIZE_MAX || in_use < 0 || in_use > size ||
       waiters < 0 || grants < 0 || timeouts < 0 || stuck < 0 || poisoned < 0)
      return -1;
   *status = (aimee_db2_pool_status_t){
       .size = (uint32_t)size,
       .in_use = (uint32_t)in_use,
       .waiters = (uint32_t)waiters,
       .lease_grants = (uint64_t)grants,
       .lease_timeouts = (uint64_t)timeouts,
       .stuck = (uint64_t)stuck,
       .poisoned = (uint64_t)poisoned,
   };
   return 0;
}

static int production_embedding_refusals(aimee_db2_embedding_refusals_t *status)
{
   long long refused = db2_embedding_dim_refused_count();
   int offered = db2_embedding_dim_last_offered();
   if (!status || refused < 0 || offered < 0 ||
       (uint32_t)offered > AIMEE_DB2_EMBEDDING_OFFERED_MAX || ((refused == 0) != (offered == 0)))
      return -1;
   *status = (aimee_db2_embedding_refusals_t){
       .refused_count = (uint64_t)refused,
       .last_offered = (uint32_t)offered,
   };
   return 0;
}

static int production_postgres_status(aimee_db2_postgres_status_t *status)
{
   int active = -1, maximum = -1, replica = -1;
   int64_t lag = -1;
   if (!status || db2_pg_stat_summary(&active, &maximum, &replica, &lag) != 0 || active < -1 ||
       maximum < -1 || replica < -1 || replica > 1 || lag < -1 || (lag >= 0 && replica != 1))
      return -1;
   *status = (aimee_db2_postgres_status_t){0};
   if (active >= 0)
   {
      status->available |= AIMEE_DB2_POSTGRES_AVAILABLE_ACTIVE;
      status->active_connections = (uint32_t)active;
   }
   if (maximum >= 0)
   {
      status->available |= AIMEE_DB2_POSTGRES_AVAILABLE_MAX;
      status->max_connections = (uint32_t)maximum;
   }
   if (replica >= 0)
   {
      status->available |= AIMEE_DB2_POSTGRES_AVAILABLE_ROLE;
      status->is_replica = (uint32_t)replica;
   }
   if (lag >= 0)
   {
      status->available |= AIMEE_DB2_POSTGRES_AVAILABLE_LAG;
      status->replica_lag_bytes = (uint64_t)lag;
   }
   return aimee_db2_postgres_status_valid(status) ? 0 : -1;
}

static int production_reembed_status(aimee_db2_reembed_status_t *status)
{
   if (!status)
      return -1;
   int target = 0;
   long started = 0;
   int result = db2_reembed_in_progress_get(&target, &started);
   *status = (aimee_db2_reembed_status_t){0};
   if (result == 0)
      return 0;
   if (result != 1 || target < (int)AIMEE_DB2_REEMBED_DIMENSION_MIN ||
       target > (int)AIMEE_DB2_REEMBED_DIMENSION_MAX || started <= 0)
      return -1;
   status->target_dimension = (uint32_t)target;
   status->started_epoch = (uint64_t)started;
   return 1;
}

static const aimee_db2_module_backend_t *production_backend(void)
{
   static const aimee_db2_module_backend_t backend = {
       .health_probe = db2_health_probe,
       .kb_health_probe = db2_kb_health_probe,
       .embedding_dimension = db2_embedding_dim,
       .pool_status = production_pool_status,
       .embedding_refusals = production_embedding_refusals,
       .postgres_status = production_postgres_status,
       .reembed_status = production_reembed_status,
       .reembed_clear = db2_reembed_in_progress_clear,
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
       !response_body)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   const aimee_db2_module_backend_t *backend = user_data;
   if (!backend)
      backend = production_backend();
   if (aimee_db2_health_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
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

   if (aimee_db2_embedding_dimension_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_EMBEDDING_DIMENSION_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->embedding_dimension)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int raw_dimension = backend->embedding_dimension();
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;

      uint32_t result = AIMEE_DB2_RESULT_INVALID_STATE;
      uint32_t dimension = 0u;
      if (raw_dimension >= (int)AIMEE_DB2_EMBEDDING_DIMENSION_MIN &&
          raw_dimension <= (int)AIMEE_DB2_EMBEDDING_DIMENSION_MAX)
      {
         result = AIMEE_DB2_RESULT_OK;
         dimension = (uint32_t)raw_dimension;
      }
      if (aimee_db2_embedding_dimension_reply_encode(result, dimension, response_body,
                                                     response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_pool_status_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_POOL_STATUS_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->pool_status)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      aimee_db2_pool_status_t status = {0};
      uint32_t result =
          backend->pool_status(&status) == 0 ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_INVALID_STATE;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_pool_status_reply_encode(result, result == AIMEE_DB2_RESULT_OK ? &status : NULL,
                                             response_body, response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_embedding_refusals_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_EMBEDDING_REFUSALS_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->embedding_refusals)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      aimee_db2_embedding_refusals_t status = {0};
      uint32_t result = backend->embedding_refusals(&status) == 0 ? AIMEE_DB2_RESULT_OK
                                                                  : AIMEE_DB2_RESULT_INVALID_STATE;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_embedding_refusals_reply_encode(
              result, result == AIMEE_DB2_RESULT_OK ? &status : NULL, response_body,
              response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_postgres_status_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_POSTGRES_STATUS_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->postgres_status)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      aimee_db2_postgres_status_t status = {0};
      uint32_t result = backend->postgres_status(&status) == 0 ? AIMEE_DB2_RESULT_OK
                                                               : AIMEE_DB2_RESULT_INVALID_STATE;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_postgres_status_reply_encode(
              result, result == AIMEE_DB2_RESULT_OK ? &status : NULL, response_body,
              response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_reembed_status_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_REEMBED_STATUS_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->reembed_status)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      aimee_db2_reembed_status_t status = {0};
      int backend_result = backend->reembed_status(&status);
      uint32_t result = backend_result == 1   ? AIMEE_DB2_RESULT_OK
                        : backend_result == 0 ? AIMEE_DB2_RESULT_NOT_FOUND
                                              : AIMEE_DB2_RESULT_INVALID_STATE;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_reembed_status_reply_encode(
              result, result == AIMEE_DB2_RESULT_OK ? &status : NULL, response_body,
              response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_reembed_clear_request_decode(request_body, request_len) != 0 ||
       response_capacity < AIMEE_DB2_REEMBED_CLEAR_RESPONSE_LEN)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (!backend || !backend->reembed_clear)
      return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
   uint32_t result =
       backend->reembed_clear() == 0 ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_INVALID_STATE;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   if (aimee_db2_reembed_clear_reply_encode(result, response_body, response_capacity,
                                            response_len) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   return AIMEE_MODULE_STATUS_OK;
}
