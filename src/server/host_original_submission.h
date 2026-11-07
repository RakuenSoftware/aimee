#ifndef HOST_ORIGINAL_SUBMISSION_H
#define HOST_ORIGINAL_SUBMISSION_H
#include "host_original_text.h"
struct cJSON;
typedef struct host_original_submission host_original_submission;
typedef struct
{
   char *text;
   struct cJSON *attachment;
   size_t text_len, text_capacity;
} host_original_submission_result;
typedef struct
{
   /* Capture runs only at a compiled host entry, with its original identity
    * live. Private owner code resolves authority; no request field grants
    * authority. This strict DOM and raw headers are borrowed for this call only.
    */
   host_text_status (*capture)(const char *headers, const struct cJSON *, void **, void *);
   /* Consume runs once on the worker before canonical/history/memory producers.
    * Success transfers malloc/cJSON-owned results, valid until result_destroy.
    * text_capacity must be the actual allocated extent and equal text_len+1;
    * the sole terminating NUL is at text_len.
    * Selected plaintext must not occur in the result. Owner retains no ownership
    * of these buffers, but may bind their identities during this worker scope.
    */
   host_text_status (*consume)(void *, const struct cJSON *, host_original_submission_result *,
                               void *);
   /* Revoke fresh-origin authority. admitted is documentary status only. */
   void (*finish)(void *, int admitted, void *);
   void (*destroy)(void *, void *);
} host_original_submission_owner;
/* Optional bounded opaque module DATA; never an authority, permission or
 * admission receipt. A callback returns normally without IO/Native calls or
 * nested host calls. Any output is owned even on refusal and must be malloc.
 * This separate versioned API leaves owner-contract-v1's layout unchanged. */
typedef struct {
   char *json;
   size_t length, capacity;
} host_original_submission_outcome;
typedef int (*host_original_submission_outcome_fn)(const void *, host_original_submission_outcome *, void *);
int host_original_submission_install_outcome_provider_v1(const host_original_submission_owner *, host_original_submission_outcome_fn, void *);
/* Only registered source-only caps, on their consuming worker, before release.
 * Caller owns returned strict JSON object; NULL preserves default behavior. */
struct cJSON *host_original_submission_outcome_json_v1(const host_original_submission *);
extern const unsigned aimee_original_input_outcome_callback_contract_v1;
/* Optional owner-verified, already-delivered INPUT disposition. Registration is
 * startup-only and exact-owner-bound. Callback only copies a cached receipt:
 * no IO, Native calls, nested host calls, or new source/model authority. */
int host_original_submission_install_delivered_provider_v1(const host_original_submission_owner *, host_original_submission_outcome_fn, void *);
struct cJSON *host_original_submission_delivered_json_v1(const host_original_submission *);
extern const unsigned aimee_original_input_delivered_callback_contract_v1;

/* Documentary evidence is immutable metadata, never a fresh-origin capability.
 */
typedef struct host_original_documentary host_original_documentary;
/* Callbacks return normally, without longjmp/pthread_exit or cancellation
 * state/type changes. A non-NULL retain output is owned even on refusal and
 * must be releasable. Neither callback performs model/external effects. */
typedef struct
{
   unsigned abi_version;
   int (*retain)(void *owner, void **private_document, void *state);
   void (*release)(void *private_document, void *state);
} host_original_documentary_hooks;
/* Fixed startup hooks; install before the first capture. No per-call selector.
 */
int host_original_submission_install_documentary_hooks(const host_original_documentary_hooks *,
                                                       void *);
int host_original_submission_documentary_required(void);
/* Same worker, after successful finish. Output must be an empty owned slot. */
host_text_status host_original_submission_documentary_retain(host_original_submission *,
                                                             host_original_documentary **);
/* Explicit reference ownership for inherited execution-context snapshots. */
int host_original_documentary_retain(const host_original_documentary *,
                                     host_original_documentary **);
/* Borrow only while holding a retained reference; never serialize this pointer.
 */
const void *host_original_documentary_borrow(const host_original_documentary *);
void host_original_documentary_release(host_original_documentary **);
extern const unsigned aimee_original_submission_documentary_contract_v1;

/* Startup only, before the first capture attempt. No runtime replacement. */
int host_original_submission_required(void);
int host_original_submission_install(const host_original_submission_owner *, void *);
host_text_status host_original_submission_capture(const char *headers, const char *frame,
                                                  size_t length, host_original_submission **);
/* Compiled authenticated User-source entry only. This mode grants no authority
 * and never permits passthrough when the original admission owner is absent.
 * Immutable on the registered capsule; no JSON field can select it. */
host_text_status host_original_submission_capture_user_source(const char *headers, const char *frame,
                                                              size_t length, host_original_submission **);
int host_original_submission_user_source_only(const host_original_submission *);
extern const unsigned aimee_original_user_source_only_contract_v1;
host_text_status host_original_submission_consume(host_original_submission *, const struct cJSON *,
                                                  host_original_submission_result *);
void host_original_submission_finish(host_original_submission *, int admitted);
/* Destroy only after the consuming worker returns/joins, or before dispatch.
 * finish must run on the consuming worker while its private TLS scope is live.
 */
void host_original_submission_destroy(host_original_submission **);
void host_original_submission_result_destroy(host_original_submission_result *);
/* Scoped pointer transfer on the authenticated entry thread. Neither the slot
 * nor the capsule is serialized into request JSON. Caller restores prior slot.
 */
host_original_submission **host_original_submission_pending_bind(host_original_submission **);
host_original_submission *host_original_submission_pending_take(void);
extern const unsigned aimee_original_submission_handoff_contract_v1;
#endif
