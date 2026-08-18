#include "module_adapter.h"

#include <aimee/db2/module_api.h>

#include "c/db2.h"
#include "c/db2_pool.h"
#include "c/memory_query.h"

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

static int production_dimension_reset(uint32_t target_dimension, uint32_t force, uint32_t dry_run,
                                      aimee_db2_dimension_reset_t *status)
{
   if (!status)
      return -1;
   db2_reembed_plan_t plan = {0};
   int result = db2_dim_change_reset((int)target_dimension, (int)force, (int)dry_run, &plan);
   status->recorded_dimension = plan.recorded_dim >= 0 ? (uint32_t)plan.recorded_dim : UINT32_MAX;
   status->target_dimension = plan.target_dim >= 0 ? (uint32_t)plan.target_dim : UINT32_MAX;
   status->tables_discovered = plan.n_tables >= 0 ? (uint32_t)plan.n_tables : UINT32_MAX;
   status->tables_dropped = plan.n_dropped >= 0 ? (uint32_t)plan.n_dropped : UINT32_MAX;
   status->rows_cleared = plan.rows_cleared >= 0 ? (uint64_t)plan.rows_cleared : UINT64_MAX;
   status->curator_requeued = plan.curator_requeued;
   status->evidence_requeued = plan.evidence_requeued;
   return result;
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
       .is_initialized = db2_is_initialized,
       .health_probe = db2_health_probe,
       .kb_health_probe = db2_kb_health_probe,
       .embedding_dimension = db2_embedding_dim,
       .level3_count = db2_memory_count_l3,
       .level2_count = db2_memory_count_l2,
       .pool_status = production_pool_status,
       .embedding_refusals = production_embedding_refusals,
       .postgres_status = production_postgres_status,
       .reembed_status = production_reembed_status,
       .reembed_clear = db2_reembed_in_progress_clear,
       .reembed_clear_maintenance = db2_reembed_clear_maintenance,
       .embedder_serving_id = db2_embedder_serving_id,
       .dimension_reset = production_dimension_reset,
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
   if (!invocation || !response_len || !response_body ||
       (invocation->stage_id != AIMEE_DB2_STAGE_HEALTH &&
        invocation->stage_id != AIMEE_DB2_STAGE_LEVEL3_COUNT))
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   const aimee_db2_module_backend_t *backend = user_data;
   if (!backend)
      backend = production_backend();

   if (invocation->stage_id == AIMEE_DB2_STAGE_LEVEL3_COUNT)
   {
      if (aimee_db2_level3_count_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_LEVEL3_COUNT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->level3_count)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int raw_count = backend->level3_count();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (raw_count < 0 || (uint32_t)raw_count > AIMEE_DB2_LEVEL3_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_level3_count_reply_encode((uint32_t)raw_count, response_body,
                                                 response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_level2_count_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_LEVEL2_COUNT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->level2_count)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int raw_count = backend->level2_count();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (raw_count < 0 || (uint32_t)raw_count > AIMEE_DB2_LEVEL2_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_level2_count_reply_encode((uint32_t)raw_count, response_body,
                                                 response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }

   if (aimee_db2_health_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->is_initialized || !backend->health_probe ||
          !backend->kb_health_probe)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int schema_ok = 0, have_pg_trgm = 0, kb_tables_ok = 0;
      if (backend->is_initialized() <= 0 || backend->health_probe(&schema_ok, &have_pg_trgm) != 0 ||
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

   if (aimee_db2_embedder_serving_id_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_EMBEDDER_SERVING_ID_RESPONSE_MAX_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->embedder_serving_id)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      const char *serving_id = backend->embedder_serving_id();
      uint32_t result = serving_id ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_INVALID_STATE;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_embedder_serving_id_reply_encode(
              result, result == AIMEE_DB2_RESULT_OK ? serving_id : NULL, response_body,
              response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   uint32_t maintenance_force = 0u;
   if (aimee_db2_reembed_clear_maintenance_request_decode(request_body, request_len,
                                                          &maintenance_force) == 0)
   {
      if (response_capacity < AIMEE_DB2_REEMBED_MAINT_CLEAR_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->reembed_clear_maintenance)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int was = 0, recorded = 0, running = 0;
      int backend_result =
          backend->reembed_clear_maintenance((int)maintenance_force, &was, &recorded, &running);
      aimee_db2_reembed_clear_maintenance_t status = {
          .was_in_progress = was >= 0 ? (uint32_t)was : 2u,
          .recorded_dimension = recorded >= 0 ? (uint32_t)recorded : UINT32_MAX,
          .running_dimension = running >= 0 ? (uint32_t)running : UINT32_MAX,
      };
      uint32_t result = AIMEE_DB2_RESULT_INVALID_STATE;
      const aimee_db2_reembed_clear_maintenance_t *reply_status = NULL;
      if (aimee_db2_reembed_clear_maintenance_valid(&status))
      {
         if (backend_result == 0)
         {
            result = AIMEE_DB2_RESULT_OK;
            reply_status = &status;
         }
         else if (backend_result == -1 && status.recorded_dimension > 0u &&
                  status.recorded_dimension != status.running_dimension)
         {
            result = AIMEE_DB2_RESULT_CONFLICT;
            reply_status = &status;
         }
      }
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_reembed_clear_maintenance_reply_encode(result, reply_status, response_body,
                                                           response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   uint32_t reset_target = 0u, reset_force = 0u, reset_dry_run = 0u;
   if (aimee_db2_dimension_reset_request_decode(request_body, request_len, &reset_target,
                                                &reset_force, &reset_dry_run) == 0)
   {
      if (response_capacity < AIMEE_DB2_DIMENSION_RESET_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->dimension_reset)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      aimee_db2_dimension_reset_t status = {0};
      int backend_result =
          backend->dimension_reset(reset_target, reset_force, reset_dry_run, &status);
      uint32_t result = backend_result == 0    ? AIMEE_DB2_RESULT_OK
                        : backend_result == -2 ? AIMEE_DB2_RESULT_CONFLICT
                        : backend_result == -3 ? AIMEE_DB2_RESULT_DENIED
                                               : AIMEE_DB2_RESULT_INVALID_STATE;
      const aimee_db2_dimension_reset_t *reply_status =
          result == AIMEE_DB2_RESULT_INVALID_STATE ? NULL : &status;
      if (reply_status && !aimee_db2_dimension_reset_valid(reply_status))
      {
         result = AIMEE_DB2_RESULT_INVALID_STATE;
         reply_status = NULL;
      }
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_dimension_reset_reply_encode(result, reply_status, response_body,
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
