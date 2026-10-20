/* Derived original-call admission cache. The durable session log is the source;
 * existing governed actions remain the only effect/receipt owner. */
#ifndef AIMEE_SERVER_NATIVE_TOOL_SOURCE_H
#define AIMEE_SERVER_NATIVE_TOOL_SOURCE_H
#include "server_session_capture.h"
#include <aimee/ir/aimee_ir.h>
typedef struct server_native_tool_source server_native_tool_source_t;
typedef struct
{
   aimee_request_t original; /* One original assistant call and its advertised function. */
   char sequence[32], digest[65];
   int block_index;
} server_native_tool_claim_t;
/* Caller retains the capture until close; no browser connection is retained.
 * Both sessions must have stable existing host-owned incarnation identities. */
server_native_tool_source_t *server_native_tool_source_open(server_session_capture_t *capture,
                                                            const char *source_session,
                                                            const char *actor);
void server_native_tool_source_close(server_native_tool_source_t *source);
/* Canonical comparison-only hash chain over writer-committed envelopes. Empty
 * previous starts a response. Never replaces opaque arguments with this view.
 */
int server_native_tool_source_digest(const char *previous, const struct cJSON *envelope,
                                     char digest[65]);
/* Called only after the original source prefix commits and before releasing
 * tool completion. An immutable response prefix commitment is not an effect
 * permit. Complete message snapshots repeat, rather than re-admit, stream
 * calls. */
int server_native_tool_source_record(server_native_tool_source_t *source,
                                     const aimee_request_t *catalog, const aimee_block_t *call,
                                     const char *message_id, const char *first_sequence,
                                     const char *last_sequence, const char *digest,
                                     int block_index);
/* Match exactly one unclaimed original call, read back its entire committed
 * source prefix, recheck session incarnations, then claim once on the retained
 * host turn. Output owns the original arguments, never the SDK's proposed copy.
 * out is zero-initialized or previously owned; failure leaves it unchanged.
 * No tools execute here. Caller still applies current policy and effect owner.
 */
int server_native_tool_source_claim(server_native_tool_source_t *source,
                                    const aimee_block_t *proposed, server_native_tool_claim_t *out);
/* Host-bound SDK reference, not an argument proposal. The caller must bind the
 * SDK tool alias explicitly to this namespace/name. The original ID must occur
 * exactly once in this source, including already claimed calls. Exact committed
 * prefix/incarnation proof and the same once-only claim still apply. Returned
 * original arguments are the only execution source; no effect is authorized. */
int server_native_tool_source_claim_reference(server_native_tool_source_t *source, const char *id,
                                              const char *space, const char *name,
                                              server_native_tool_claim_t *out);
void server_native_tool_claim_free(server_native_tool_claim_t *claim);
#endif
