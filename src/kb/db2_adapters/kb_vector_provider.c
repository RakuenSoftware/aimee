/* kb_vector_provider.c: the KB subscribes to vector provider announcements and
 * hands them to db2.
 *
 * This file exists so that db2 does not. Calling obs_bus_observe_kind() from
 * inside the db2 data layer worked and was wrong in a way the link-closure gate
 * caught immediately: it gave db2's C boundary a dependency on the AUDIT
 * module's surface, which standalone db2 would then have had to carry or replace
 * with an injected contract.
 *
 * The dependency also pointed the wrong way. db2 owns what a CAPABILITIES
 * announcement MEANS -- decode it, decide whether that provider may serve reads.
 * Where announcements come from is the host's business, exactly as where
 * connections come from is.
 *
 * So the host subscribes, and passes on the two things only it can know: which
 * principal the bus authenticated, and which attachment handle sent it. Those
 * come from the FRAME. Nothing here or downstream reads an identity out of the
 * payload, because a provider that could name its own principal could name
 * somebody else's.
 */
#include "modules/db2/c/memory_vectors.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/db2/vector_contract.h>

/* The codebase's module-call seam, for its deadline arithmetic: its own header
 * says every new consumer should use it rather than hand-rolling a fourth copy
 * of the same clock_gettime. */
#include "headers/module_json_call.h"
#include "log.h"

static void on_capabilities(uint32_t event_kind, uint32_t principal_ref, uint32_t src_handle,
                            uint64_t sequence, const uint8_t *payload, uint32_t payload_len,
                            void *ctx)
{
   (void)event_kind;
   (void)ctx;
   /* The return value is deliberately dropped. db2 counts what it accepted and
    * what it could not read, and those counters are the operator's view;
    * re-reporting each one here would be a log line per announcement from a
    * provider that is simply announcing on a timer. */
   (void)pgvec_memory_vector_on_capabilities(principal_ref, src_handle, sequence, payload,
                                             payload_len);
}

int kb_vector_provider_observe(void)
{
   return obs_bus_observe_kind(AIMEE_VECTOR_EVENT_CAPABILITIES, on_capabilities, NULL);
}

/* How long a search waits on a provider.
 *
 * A transport property, not a search property, which is why db2 does not carry
 * it: how long this host is willing to wait on a peer on its own bus is the
 * host's decision. Two seconds because this sits inside a user-facing memory
 * recall -- a provider that has not answered by then has already cost more than
 * the acceleration it exists to provide, and the route's policy decides what
 * happens next. */
#define KB_VECTOR_SEARCH_TIMEOUT_MS 2000

static int bus_call(void *context, uint32_t event_kind, uint32_t stage_id, const uint8_t *request,
                    uint32_t request_len, uint8_t *response, uint32_t response_capacity,
                    uint32_t *response_len)
{
   (void)context;
   /* Asked before sending. "No provider is serving this kind" is the ordinary
    * state of a deployment that has none, and it is not an error worth the cost
    * of a call that can only fail. */
   if (!obs_bus_module_available(event_kind))
      return -1;
   aimee_module_call_result_t rc = obs_bus_module_call(
       event_kind, stage_id, 0, aimee_module_call_deadline_ns(KB_VECTOR_SEARCH_TIMEOUT_MS), request,
       request_len, response, response_capacity, response_len, NULL, NULL);
   /* Every failure is one failure to db2, deliberately. The route's decision is
    * the same for all of them -- a search that did not come back is a search
    * that did not come back -- and the distinctions that matter to an operator
    * belong in a log line, not in a return code the route would have to learn to
    * interpret. */
   if (rc != AIMEE_MODULE_CALL_OK)
   {
      aimee_log(LOG_WARN, "vector_provider", "search call failed: %s",
                aimee_module_call_result_name(rc));
      return -1;
   }
   return 0;
}

int kb_vector_provider_install_transport(void)
{
   /* Fallback disabled: this deployment attached a provider, and the reason to
    * attach one is vectors pgvector cannot hold. Re-issuing a failed search
    * against pgvector would search a corpus missing exactly those vectors and
    * return a short answer that looks complete. A deployment mid-migration, with
    * its vectors in both, is the case that wants fallback -- and that is a
    * choice to make explicitly when there is somewhere to express it, not a
    * default to inherit. */
   return pgvec_memory_vector_set_transport(bus_call, NULL, 0);
}

void kb_vector_provider_start(void)
{
   /* Not fatal. A KB that cannot observe announcements searches with pgvector,
    * which is what every deployment without a provider does anyway; refusing to
    * start would turn an optional accelerator into a boot dependency.
    *
    * Logged, because the deployment this matters to is the one that DID attach a
    * provider and is wondering why nothing is using it. */
   if (kb_vector_provider_observe() != 0)
      aimee_log(LOG_WARN, "vector_provider",
                "not observing provider announcements; vector search stays on pgvector");
   /* Also not fatal, and a different sentence on purpose: this failing does not
    * mean nothing was detected, it means a detected provider cannot be reached.
    * An operator reading "no transport" while the selection log line says a
    * provider was selected has been told exactly what is wrong. */
   if (kb_vector_provider_install_transport() != 0)
      aimee_log(LOG_WARN, "vector_provider",
                "no search transport installed; a selected provider will serve no searches");
}
