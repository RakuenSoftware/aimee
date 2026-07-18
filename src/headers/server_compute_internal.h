#ifndef SERVER_COMPUTE_INTERNAL_H
#define SERVER_COMPUTE_INTERNAL_H
#include <stddef.h>
#include "cJSON.h"
#include "server.h"
#include "delegate_ensemble.h"
#include <pthread.h>
#define DELEGATION_INPUT_TIMEOUT 60 /* seconds */
#define MAX_ACTIVE_DELEGATIONS   32
typedef struct
{
   char delegation_id[64];
   pthread_mutex_t lock;
   pthread_cond_t reply_ready;
   char reply[4096];
   int has_reply;
   int active;
} delegation_mailbox_t;
/* Cross-TU declarations for the server_compute cluster. Formerly file-local
 * statics shared by textual .inc inclusion. */

/* promoted cross-TU (former .inc statics) */
int delegate_agent_uses_mistral_path(const agent_t *agent);
delegation_mailbox_t *mailbox_acquire(const char *delegation_id);
delegation_mailbox_t *mailbox_find(const char *delegation_id);
void mailbox_release(delegation_mailbox_t *mb);
void mailbox_reply(delegation_mailbox_t *mb, const char *content);
int mailbox_wait(delegation_mailbox_t *mb, char *out, size_t out_len, int timeout_secs);

extern __thread delegation_mailbox_t *tl_mailbox;
extern __thread char tl_parent_delegation_id[64];
extern __thread int tl_delegation_depth;
extern delegation_mailbox_t g_mailboxes[MAX_ACTIVE_DELEGATIONS];
extern pthread_mutex_t g_mailbox_lock;
typedef struct
{
   char *rendered;
   int truncated;
   char questions[ROUNDTABLE_MAX_QUESTIONS][512];
   const char *question_ptrs[ROUNDTABLE_MAX_QUESTIONS];
   int question_count;
} normalized_roundtable_brief_t;

/* promoted cross-TU (former .inc statics) */
void add_roundtable_arrays(cJSON *resp, const roundtable_result_t *result);
int normalize_roundtable_brief(cJSON *req, normalized_roundtable_brief_t *out, char *err,
                               size_t err_n);

#define ROUNDTABLE_MAX_ROUNDS_REQUEST 16

#define ROUNDTABLE_BRIEF_MAX_BYTES (256 * 1024)
#endif /* SERVER_COMPUTE_INTERNAL_H */
