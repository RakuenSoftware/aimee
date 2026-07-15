/* aimee_ir_shadow.h -- SHADOW-MODE observation of the canonical-IR path on live
 * traffic. The legacy translator still serves every response; the shadow just
 * parses the same request into the IR, rebuilds it same-protocol, and records
 * whether the round-trip is faithful (ir_rebuild_mismatch = a BUG). This proves
 * IR fidelity on real Claude Code traffic BEFORE any flag flips traffic onto the
 * IR path. Config-only gate (never request-controlled). Never affects the turn. */
#ifndef DEC_AIMEE_IR_SHADOW_H
#define DEC_AIMEE_IR_SHADOW_H 1

#include "aimee_ir.h" /* aimee_wire_t */

struct cJSON;

/* Observe one inbound request in shadow mode: no-op unless AIMEE_IR_SHADOW is set.
 * Parses `req` for the given frontend wire, rebuilds it, updates the ir_* metrics,
 * and logs the first few mismatches. Safe on the hot path (void, fail-silent). */
void aimee_ir_shadow_observe_request(const struct cJSON *req, aimee_wire_t frontend);

/* Shadow-compare the two provider bodies for the SAME request: what the IR built
 * (`ir_body`) vs what the legacy translator would have sent (`legacy_body`).
 * Counts ir_body_match / ir_body_mismatch and logs a capped, TRUNCATED diff.
 *
 * This is the evidence that retires the legacy translators. The existing
 * observe_request only proves the IR round-trips the CLIENT shape; it says nothing
 * about whether the IR sends the provider the same thing legacy would. Deleting
 * legacy on "it worked for me" is not the same as proving equivalence on real
 * traffic.
 *
 * No-op unless AIMEE_IR_SHADOW is set — building the legacy body costs real work,
 * so it must never run on the hot path by default. Either body may be NULL (a
 * build failure), which counts as a mismatch. */
void aimee_ir_shadow_compare_bodies(const char *ir_body, const char *legacy_body,
                                    aimee_wire_t frontend);

/* 1 when shadow mode is on, so callers can skip building the comparison body. */
int aimee_ir_shadow_enabled(void);

#endif /* DEC_AIMEE_IR_SHADOW_H */
