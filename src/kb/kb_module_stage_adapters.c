#define _POSIX_C_SOURCE 200809L

#include "kb_module_stage_adapters.h"

#include "kb_curator_grounding.h"
#include "kb_mdl.h"
#include "kb_route_acl.h"

#include <aimee/audit/audit_worm_chain.h>
#include <aimee/audit/obs_bus.h>
#include <aimee/control-web/module_api.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/db2/client.h>
#include <aimee/db2/host_contracts.h>
#include <aimee/kb-synthesis/module_api.h>
#include <aimee/memory/module_api.h>
#include <aimee/postgres/module_api.h>

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#define KB_MODULE_STAGE_DEADLINE_NS (500ULL * 1000000ULL)

static atomic_uint_fast64_t next_trace = 1;

static uint64_t monotonic_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int call_module(uint32_t event_kind, uint32_t stage_id, const void *request,
                       uint32_t request_len, void *response, uint32_t response_capacity,
                       uint32_t *response_len)
{
   uint64_t now = monotonic_ns();
   if (!now)
      return -1;
   uint64_t trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   if (trace == 0)
      trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   return obs_bus_module_call(event_kind, stage_id, trace, now + KB_MODULE_STAGE_DEADLINE_NS,
                              request, request_len, response, response_capacity, response_len, NULL,
                              NULL) == AIMEE_MODULE_CALL_OK
              ? 0
              : -1;
}

static aimee_module_call_result_t
call_db2(void *context, uint32_t event_kind, uint32_t stage_id, uint64_t trace_id,
         uint64_t deadline_ns, const void *request_body, uint32_t request_len, void *response_body,
         uint32_t response_capacity, uint32_t *response_len, aimee_module_cancelled_fn cancelled,
         void *cancel_context)
{
   (void)context;
   return obs_bus_module_call(event_kind, stage_id, trace_id, deadline_ns, request_body,
                              request_len, response_body, response_capacity, response_len,
                              cancelled, cancel_context);
}

static int grounding_decide(aimee_kb_synthesis_claim_kind_t claim_kind, const char *const *claims,
                            uint32_t claim_count, const char *const *callees, uint32_t callee_count,
                            aimee_kb_synthesis_grounding_decision_t *decision)
{
   uint8_t request[AIMEE_KB_SYNTHESIS_REQUEST_LEN];
   uint8_t response[AIMEE_KB_SYNTHESIS_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (!decision || aimee_kb_synthesis_request_encode(claim_kind, claims, claim_count, callees,
                                                      callee_count, request, sizeof(request)) != 0)
      return -1;
   if (call_module(AIMEE_KB_SYNTHESIS_EVENT_GROUNDING, AIMEE_KB_SYNTHESIS_STAGE_GROUNDING, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_kb_synthesis_response_decode(response, response_len, decision);
}

static int control_web_authorize(const char *method, const char *path, int *allowed)
{
   uint8_t request[AIMEE_CONTROL_WEB_REQUEST_LEN];
   uint8_t response[AIMEE_CONTROL_WEB_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (!allowed ||
       aimee_control_web_request_encode(AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN, method, path,
                                        request, sizeof(request)) != 0 ||
       call_module(AIMEE_CONTROL_WEB_EVENT_AUTHORIZE, AIMEE_CONTROL_WEB_STAGE_AUTHORIZE, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_control_web_response_decode(response, response_len, allowed);
}

static int score_mdl(const char *candidate, const char *evidence, double *l_candidate,
                     double *l_residual, double *total)
{
   if (!l_candidate || !l_residual || !total)
      return -1;
   kb_mdl_score_t score = {0};
   if (kb_mdl_score(candidate, evidence, &score) != 0)
      return -1;
   *l_candidate = score.l_candidate;
   *l_residual = score.l_residual;
   *total = score.total;
   return 0;
}

static int check_fact_gate(int head_kind, const char *rel_type, int tail_kind, int *verdict)
{
   uint8_t request[AIMEE_MEMORY_GATE_REQUEST_LEN], response[AIMEE_MEMORY_GATE_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_memory_fact_verdict_t result;
   if (!verdict)
      return -1;
   if (aimee_memory_gate_request_encode((uint32_t)head_kind, rel_type, (uint32_t)tail_kind, request,
                                        sizeof(request)) != 0)
   {
      *verdict = AIMEE_DB2_FACT_GATE_BADARG;
      return 0;
   }
   if (call_module(AIMEE_MEMORY_EVENT_WRITE, AIMEE_MEMORY_STAGE_WRITE, request, sizeof(request),
                   response, sizeof(response), &response_len) != 0 ||
       aimee_memory_gate_response_decode(response, response_len, &result) != 0)
      return -1;
   switch (result)
   {
   case AIMEE_MEMORY_FACT_ACCEPT:
      *verdict = AIMEE_DB2_FACT_GATE_ACCEPT;
      return 0;
   case AIMEE_MEMORY_FACT_REJECT_KIND:
      *verdict = AIMEE_DB2_FACT_GATE_REJECT_KIND;
      return 0;
   case AIMEE_MEMORY_FACT_NOVEL:
      *verdict = AIMEE_DB2_FACT_GATE_NOVEL;
      return 0;
   case AIMEE_MEMORY_FACT_BADARG:
      *verdict = AIMEE_DB2_FACT_GATE_BADARG;
      return 0;
   default:
      return -1;
   }
}

int kb_module_postgres_health_probe(int *schema_ok, int *have_pg_trgm, int *kb_tables_ok)
{
   uint8_t request[AIMEE_POSTGRES_REQUEST_LEN];
   uint8_t response[AIMEE_POSTGRES_RESPONSE_LEN] = {0};
   uint32_t response_len = 0;
   if (schema_ok)
      *schema_ok = 0;
   if (have_pg_trgm)
      *have_pg_trgm = 0;
   if (kb_tables_ok)
      *kb_tables_ok = 0;
   if (aimee_postgres_health_request_encode(request, sizeof(request)) != 0 ||
       call_module(AIMEE_POSTGRES_EVENT_HEALTH, AIMEE_POSTGRES_STAGE_HEALTH, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_postgres_health_response_decode(response, response_len, schema_ok, have_pg_trgm,
                                                kb_tables_ok);
}

int kb_module_db2_health_probe(int *schema_ok, int *have_pg_trgm, int *kb_tables_ok)
{
   uint64_t now = monotonic_ns();
   if (!now)
      return -1;
   uint64_t trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   if (trace == 0)
      trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   return aimee_db2_health_call(call_db2, NULL, trace, now + KB_MODULE_STAGE_DEADLINE_NS, schema_ok,
                                have_pg_trgm, kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_OK
              ? 0
              : -1;
}

void kb_module_stage_adapters_configure(void)
{
   aimee_db2_register_audit_hash_provider(audit_worm_row_hash);
   aimee_db2_register_mdl_score_provider(score_mdl);
   aimee_db2_register_fact_gate_provider(check_fact_gate);
   kb_curator_grounding_register_provider(grounding_decide);
   kb_route_acl_register_authorization_provider(control_web_authorize);
}
