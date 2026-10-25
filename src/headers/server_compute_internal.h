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
   unsigned references; /* registry lock; creator plus retained callbacks/repliers */
} delegation_mailbox_t;
/* Cross-TU declarations for the server_compute cluster. Formerly file-local
 * statics shared by textual .inc inclusion. */

/* promoted cross-TU (former .inc statics) */
int delegate_agent_uses_mistral_path(const agent_t *agent);
delegation_mailbox_t *mailbox_acquire(const char *delegation_id);
/* Find returns an owned lease. Put it after queuing; release retires the creator. */
delegation_mailbox_t *mailbox_find(const char *delegation_id);
void mailbox_put(delegation_mailbox_t *mb);
void mailbox_release(delegation_mailbox_t *mb);
/* 0 queued, 1 closed. Caller holds a creator or find lease. */
int mailbox_reply(delegation_mailbox_t *mb, const char *content);
int mailbox_wait(delegation_mailbox_t *mb, char *out, size_t out_len, int timeout_secs);

extern __thread delegation_mailbox_t *tl_mailbox;
extern __thread char tl_parent_delegation_id[64];
extern __thread int tl_delegation_depth;
extern delegation_mailbox_t g_mailboxes[MAX_ACTIVE_DELEGATIONS];
extern pthread_mutex_t g_mailbox_lock;
#endif /* SERVER_COMPUTE_INTERNAL_H */
