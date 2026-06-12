/* server_compute_impl.h: private declarations shared between server_compute.c and its
 * platform implementations (posix/, windows/). */
#ifndef DEC_SERVER_COMPUTE_IMPL_H
#define DEC_SERVER_COMPUTE_IMPL_H 1

#include "aimee.h"
#include "server.h"
#include "cJSON.h"
#include <pthread.h>

#define CLAUDE_LINE_MAX (256 * 1024)

/* Context for async compute work */
typedef struct
{
   server_ctx_t *server;
   int conn_fd;
   int conn_alive; /* 0 once a write to conn_fd fails (client disconnected) */
   int compute_grant;
   int compute_budget_acquired;
   int compute_executor_threads;
   int background_job_id;
   int coord_task_id; /* non-zero when this worker is executing a coord task */
   int async_slot;
   pthread_mutex_t *write_mutex;
   /* The originating connection's capability set, captured at ctx creation.
    * CAPS_ALL ⟺ a fully-trusted local UDS peer (co-located, same filesystem);
    * anything less is a remote/TCP peer whose client-supplied cwd is a FOREIGN
    * path the server must not open on its own filesystem (workspace-resource-
    * plane AC #6 — the worktree_cwd-trust hole). */
   uint32_t conn_caps;
   /* WP-C.0: the attested vault principal + transport, copied from the conn
    * while it is live (create_compute_ctx) so the detached worker — which runs
    * after conn_fd is closed and no thread-local survives — can resolve the
    * right per-user vault. Empty principal => un-attested => no vault. This is
    * the ONLY identity key the worker trusts (never the body session_id). */
   attested_transport_t attested_transport;
   char vault_principal[VAULT_PRINCIPAL_MAX];
   cJSON *req; /* owned by this context */
   /* Unified-presence turn arbitration. When a chat request carries an
    * attach_id and a presence exists for the session, handle_chat_send_stream
    * acquires the per-session turn lock before dispatch; the pooled worker
    * releases it on completion. Zeroed (unlocked) by default, so requests
    * without an attach_id behave exactly as before. */
   int presence_locked;
   char presence_session[128];
   char presence_turn_id[64];
   /* When >1 surface is attached to the session at turn start, the chat
    * worker also mirrors each text delta to the presence-event ring as a
    * turn_delta (so a second attached surface sees the live token stream, not
    * just the turn boundaries). 0 in the common single-surface case → the
    * stream_event hot path pays only one int check. */
   int presence_emit_deltas;
} compute_ctx_t;

/* Defined in server_compute.c; used by platform implementations */
int write_all(int fd, const char *data, size_t len);
void compute_ctx_begin_budget(compute_ctx_t *cctx);
compute_ctx_t *create_compute_ctx(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
void compute_respond(compute_ctx_t *cctx, cJSON *resp);
void compute_error(compute_ctx_t *cctx, const char *message);
void compute_ok(compute_ctx_t *cctx);
void compute_ctx_release_budget(compute_ctx_t *cctx);
void compute_ctx_free(compute_ctx_t *cctx);
int tool_execute_dispatch(server_ctx_t *ctx, compute_ctx_t *cctx);
/* Dispatch a coord task as a background delegate worker. Returns 0 on success, -1 on failure. */
int server_compute_dispatch_coord_task(server_ctx_t *ctx, int task_id, const char *role,
                                       const char *prompt, const char *files_json, const char *cwd);
/* Delegate worker thread entry point — non-static so skill_jobs.c can reference it. */
void delegate_worker(void *arg);
/* Fire-and-forget: spawn a skill review delegate for session_id. */
void server_compute_skill_review_async(server_ctx_t *ctx, const char *session_id);
/* Fire-and-forget: run the skill curator cycle on the compute pool. */
void server_compute_skill_curator_async(server_ctx_t *ctx);

/* Platform-provided: chat.send_stream worker thread entry point */
void chat_stream_worker(void *arg);

/* Portable fd-writability check used inside write_all.
 * POSIX: poll() with 30s timeout; Windows: return -1 immediately. */
#ifdef AIMEE_POSIX
#include <poll.h>
static inline int platform_wait_writable(int fd)
{
   struct pollfd pfd = {.fd = fd, .events = POLLOUT};
   return poll(&pfd, 1, 30000) > 0 ? 0 : -1;
}
#else
static inline int platform_wait_writable(int fd)
{
   (void)fd;
   return -1;
}
#endif

#endif /* DEC_SERVER_COMPUTE_IMPL_H */
