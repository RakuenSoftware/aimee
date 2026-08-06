/* module_json_call.h: the C core's general path for ingesting a JSON reply from
 * a module.
 *
 * Modules are separate Go processes reached over the event bus. The fixed-size
 * binary stages (aimee_<mod>_request_encode / _response_decode) suit a small
 * decision -- a confidence bucket, a sink mask -- but a module that returns
 * anything shaped, or anything whose size is not known at compile time, needs a
 * JSON round trip instead.
 *
 * Three call sites grew that round trip independently (sandbox learned-package
 * load/observe, roundtable review, workflow control), each with its own body
 * cap, deadline arithmetic, buffer handling and failure naming. The plumbing is
 * identical every time; only the error CHANNEL differs, because one renders an
 * HTTP status, one an API error object, and one is best-effort and silent. So
 * this owns the plumbing and hands the outcome back for the caller to render.
 *
 * As module logic moves out of C and into Go, this is the seam every new
 * consumer should use rather than hand-rolling a fourth copy.
 */
#ifndef AIMEE_MODULE_JSON_CALL_H
#define AIMEE_MODULE_JSON_CALL_H

#include <stddef.h>
#include <stdint.h>

#include <aimee/audit/obs_bus.h>

#include "cJSON.h"

/* One JSON request/response round trip to a module stage.
 *
 * Takes ownership of `request` unconditionally -- it is deleted before this
 * returns, on every path including failure -- so a caller never double-frees a
 * body it built inline.
 *
 * Returns the parsed reply, which the caller must cJSON_Delete, or NULL on any
 * failure. When `result` is non-NULL it always receives the outcome, so the
 * caller can name it with aimee_module_call_result_name() in whatever channel it
 * owns. A NULL return with AIMEE_MODULE_CALL_OK means the module answered but
 * the body did not parse as JSON.
 *
 * `max_body` bounds both the request and the response buffer. A request that
 * would exceed it fails without touching the bus.
 *
 * `timeout_ms` <= 0 means no deadline.
 */
cJSON *aimee_module_json_call(uint32_t event_kind, uint32_t stage_id, cJSON *request,
                              size_t max_body, int timeout_ms, aimee_module_call_result_t *result);

/* The same round trip for a caller that already holds a serialized body.
 *
 * Two of the three consumers build their request as a string directly rather
 * than as a cJSON tree, so making the tree the only entry point would force them
 * to build a tree purely to print it again. `body` is borrowed, not owned: the
 * caller frees it as before.
 */
cJSON *aimee_module_json_call_raw(uint32_t event_kind, uint32_t stage_id, const char *body,
                                  size_t body_len, size_t max_body, int timeout_ms,
                                  aimee_module_call_result_t *result);

/* Deadline arithmetic for a module call: a CLOCK_MONOTONIC nanosecond stamp
 * timeout_ms in the future, or 0 for "no deadline". Exposed because callers that
 * compute a deadline once and make several calls should not each re-derive it.
 */
uint64_t aimee_module_call_deadline_ns(int timeout_ms);

#endif /* AIMEE_MODULE_JSON_CALL_H */
