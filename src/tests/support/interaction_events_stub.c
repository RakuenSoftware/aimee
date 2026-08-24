/* interaction_events_stub.c: record nothing, successfully.
 *
 * failover.c records a failover event through db1_interaction_event_record, so
 * anything linking failover.o needs that symbol. It used to be satisfied by
 * modules/db1/interaction_events.o -- the real implementation, made inert by
 * stubbing db1_conn to NULL so it had no connection to write through.
 *
 * That trick died with the C store. The implementation is a bus client now, in
 * db1_client/telemetry.c, and it reaches a separate process: there is no handle
 * to withhold, and linking it would make a unit test about HTTP backoff depend
 * on a running module.
 *
 * So the dependency is cut where it belongs, at the call. A test that asserts
 * how retries back off, when a stall caps them, and that a body is byte-
 * identical across attempts has no business either recording telemetry or
 * proving it was recorded.
 */

#include "db1_client/interaction_events.h"

int db1_interaction_event_record(const char *session_id, const char *type_name, const char *actor,
                                 const char *payload_json, const char *outcome)
{
   (void)session_id;
   (void)type_name;
   (void)actor;
   (void)payload_json;
   (void)outcome;
   return 0;
}
