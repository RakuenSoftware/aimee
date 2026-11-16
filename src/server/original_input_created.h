#ifndef ORIGINAL_INPUT_CREATED_H
#define ORIGINAL_INPUT_CREATED_H
#include "db1_client/primary_sessions.h"
#include <aimee/ir/aimee_ir.h>
#include <stddef.h>
typedef struct server_input_created_cap server_input_created_cap_t;
/* All pointers are borrowed only during the synchronous created callback.
 * Text is generic input, not a declaration that it contains no PII. */
typedef struct
{
   unsigned schema_version;
   const char *session, *principal, *execution_session, *operation_uuid;
   const char *text, *opaque, *text_sha256;
   size_t text_len, opaque_len;
   db1_original_input_receipt_t receipt;
   const void *owner_cookie;
} server_input_created_view_t;
typedef int (*server_input_owner_admit_fn)(const char *, const char *, const char *,
                                           const aimee_message_t *, const void **, void *);
typedef int (*server_input_created_fn)(const server_input_created_cap_t *, void *);
/* Startup only, mutually exclusive with legacy admit-only installation. */
int server_input_attachment_install_owner(server_input_owner_admit_fn, server_input_created_fn,
                                          void *);
int server_input_created_owner_configured(void);
/* Valid only on the callback thread while that exact opaque cap is active.
 * Pointer identity is checked before dereference; no public constructor. */
int server_input_created_view(const server_input_created_cap_t *, server_input_created_view_t *);
#endif

/* Installed callbacks are synchronous and must return normally. They must not
 * call longjmp/pthread_exit or alter pthread cancellation state/type. Deferred
 * cancellation is held across transport/callback custody and delivered after
 * capability/phase/reference cleanup, with logger RAM-abort cleanup installed.
 * No callback may retain borrowed cap/view pointers beyond its invocation. */
