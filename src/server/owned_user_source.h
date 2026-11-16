#ifndef AIMEE_OWNED_USER_SOURCE_H
#define AIMEE_OWNED_USER_SOURCE_H
#include <stddef.h>
/* Optional private communication hook. The compiled authenticated HTTP entry
 * invokes prepare; no JSON field selects the source-only mode or a User role.
 * Prepare is VOLATILE ONLY: it must not mutate a module, copy unscreened input
 * to durable storage, or claim that an input has been admitted. Original-input
 * owner capture/consume performs the existing actual User/source checks.
 *
 * On success frame is a malloc-owned canonical input frame with exact stable
 * operation/session metadata. purpose is opaque nonwire private state; the
 * private owner must bind/copy it during capture and validate it at consume.
 * Release restores the preparing thread's previous purpose and destroys it.
 * It must also accept a partial non-NULL purpose on prepare failure. Neither
 * callbacks nor caller may infer a successful module mutation from admission.
 */
extern int aimee_module_owned_user_source_prepare_v1(const char *headers, const char *opaque_body,
                                                     size_t body_length, char **frame,
                                                     size_t *frame_length, void **purpose)
    __attribute__((weak));
extern void aimee_module_owned_user_source_release_v1(void *purpose) __attribute__((weak));
/* Separate completed-action contract. Zero declines selection WITHOUT effect
 * and with empty output; one supplies bounded completed DATA; two supplies
 * bounded unconfirmed DATA; negative refuses. Selection/authorization/semantics
 * belong entirely to the installed module. The caller provides no authority.
 * This callback may perform a module action, unlike volatile prepare_v1.
 * Output is malloc-owned on every return; capacity is its actual owned readable
 * extent, even if a malformed length is returned. Valid output has capacity
 * == length+1 <=16385. The host never wipes or reads an invalid length. Callback
 * must use scoped cleanup on
 * cancellation/exit and must preserve the caller's cancellation state/type. */
extern int aimee_module_owned_user_action_v1(const char *, const char *, size_t, char **, size_t *,
                                             size_t *) __attribute__((weak));
extern const unsigned aimee_owned_user_action_handoff_contract_v1;
#endif
