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
#include <aimee/db2/vector_contract.h>

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
}
