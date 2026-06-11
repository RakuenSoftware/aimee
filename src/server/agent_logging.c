/* agent_logging.c: agent_log + token_audit recording.
 *
 * Extracted from agent_runtime.c (which sits at the line-count limit). Owns the
 * per-call audit write shared by the internal agent path (agent_log_call) and the
 * ingress handlers (agent_record_token_audit), plus the per-thread ingress-source
 * override used by ingress workers such as /v1/runs. */
#include "aimee.h"
#include "agent_exec.h"
#include "config.h" /* session_id(), config_load */
#include "db1.h"    /* token_audit + agent_log inserts */
#include "token_tracker.h"
#include "request_context.h" /* per-request id / principal / idempotency */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* Exported by server_compute when the call happens inside a delegate worker;
 * weak-stub returns NULL elsewhere (CLI, tests). */
const char *delegation_active_id(void);

/* ── Asynchronous audit writer (ingress_audit_async rollout knob) ────────────
 * When enabled, an audit row is copied into a bounded ring and inserted by a
 * single background thread, so the request is not blocked on the DB write. The
 * thread is started lazily on first async use; tests (which never enable it)
 * never spawn it. If the ring is full the caller falls back to an inline insert,
 * so a row is never dropped. Strings are fully materialized into the entry — the
 * source db1_token_audit_row_t points at caller/stack memory that does not
 * outlive the call. */
#define AUDIT_ASYNC_SLOTS 256

typedef struct
{
   char session_id[128], delegation_id[128], project_name[64], tool_name[128], role[64];
   char model[128], source[64], requested_model[128], stop_reason[40], usage_kind[24];
   char request_id[64], idempotency_key[128], principal[128], served_model[128], metadata[256];
   long long agent_log_id;
   int attempt;
   int duration_ms;
   int prompt_tokens, completion_tokens, cache_write_tokens, cache_read_tokens;
   double estimated_cost_usd;
} audit_async_entry_t;

static audit_async_entry_t g_audit_ring[AUDIT_ASYNC_SLOTS];
static int g_audit_head, g_audit_tail; /* tail == head => empty */
static pthread_mutex_t g_audit_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_audit_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_audit_drained = PTHREAD_COND_INITIALIZER;
static long g_audit_pending; /* enqueued but not yet inserted by the writer */
static pthread_t g_audit_thread;
static int g_audit_thread_started;

static void audit_entry_to_row(const audit_async_entry_t *e, db1_token_audit_row_t *row)
{
   memset(row, 0, sizeof(*row));
   row->session_id = e->session_id;
   row->delegation_id = e->delegation_id;
   row->project_name = e->project_name;
   row->tool_name = e->tool_name;
   row->role = e->role;
   row->model = e->model;
   row->source = e->source;
   row->requested_model = e->requested_model;
   row->stop_reason = e->stop_reason;
   row->usage_kind = e->usage_kind;
   row->agent_log_id = e->agent_log_id;
   row->request_id = e->request_id;
   row->idempotency_key = e->idempotency_key;
   row->attempt = e->attempt;
   row->principal = e->principal;
   row->served_model = e->served_model;
   row->duration_ms = e->duration_ms;
   row->metadata = e->metadata;
   row->prompt_tokens = e->prompt_tokens;
   row->completion_tokens = e->completion_tokens;
   row->cache_write_tokens = e->cache_write_tokens;
   row->cache_read_tokens = e->cache_read_tokens;
   row->estimated_cost_usd = e->estimated_cost_usd;
}

static void *audit_writer_main(void *arg)
{
   (void)arg;
   for (;;)
   {
      audit_async_entry_t e;
      pthread_mutex_lock(&g_audit_mtx);
      while (g_audit_tail == g_audit_head)
         pthread_cond_wait(&g_audit_cv, &g_audit_mtx);
      e = g_audit_ring[g_audit_tail];
      g_audit_tail = (g_audit_tail + 1) % AUDIT_ASYNC_SLOTS;
      pthread_mutex_unlock(&g_audit_mtx);

      db1_token_audit_row_t row;
      audit_entry_to_row(&e, &row);
      (void)db1_token_audit_insert(&row);

      /* Mark the item drained so a flush (clean shutdown / tests) can wait for
       * the queue to empty. */
      pthread_mutex_lock(&g_audit_mtx);
      if (g_audit_pending > 0)
         g_audit_pending--;
      pthread_cond_broadcast(&g_audit_drained);
      pthread_mutex_unlock(&g_audit_mtx);
   }
   return NULL;
}

/* Copy `row` into the ring for the background writer. Returns 0 on success, -1
 * when the ring is full (caller inserts inline). */
static int audit_async_enqueue(const db1_token_audit_row_t *row)
{
   pthread_mutex_lock(&g_audit_mtx);
   int next = (g_audit_head + 1) % AUDIT_ASYNC_SLOTS;
   if (next == g_audit_tail)
   {
      pthread_mutex_unlock(&g_audit_mtx);
      return -1; /* full — fall back to inline */
   }
   if (!g_audit_thread_started)
   {
      if (pthread_create(&g_audit_thread, NULL, audit_writer_main, NULL) != 0)
      {
         pthread_mutex_unlock(&g_audit_mtx);
         return -1;
      }
      pthread_detach(g_audit_thread);
      g_audit_thread_started = 1;
   }
   audit_async_entry_t *e = &g_audit_ring[g_audit_head];
   memset(e, 0, sizeof(*e));
#define CPY(dst, src) snprintf((dst), sizeof(dst), "%s", (src) ? (src) : "")
   CPY(e->session_id, row->session_id);
   CPY(e->delegation_id, row->delegation_id);
   CPY(e->project_name, row->project_name);
   CPY(e->tool_name, row->tool_name);
   CPY(e->role, row->role);
   CPY(e->model, row->model);
   CPY(e->source, row->source);
   CPY(e->requested_model, row->requested_model);
   CPY(e->stop_reason, row->stop_reason);
   CPY(e->usage_kind, row->usage_kind);
   CPY(e->request_id, row->request_id);
   CPY(e->idempotency_key, row->idempotency_key);
   CPY(e->principal, row->principal);
   CPY(e->served_model, row->served_model);
   CPY(e->metadata, row->metadata);
#undef CPY
   e->agent_log_id = row->agent_log_id;
   e->attempt = row->attempt;
   e->duration_ms = row->duration_ms;
   e->prompt_tokens = row->prompt_tokens;
   e->completion_tokens = row->completion_tokens;
   e->cache_write_tokens = row->cache_write_tokens;
   e->cache_read_tokens = row->cache_read_tokens;
   e->estimated_cost_usd = row->estimated_cost_usd;
   g_audit_head = next;
   g_audit_pending++;
   pthread_cond_signal(&g_audit_cv);
   pthread_mutex_unlock(&g_audit_mtx);
   return 0;
}

/* Public async-audit API (rollout knob + validation). enqueue_row copies the row
 * into the ring for the background writer, returning 0 when enqueued or -1 when
 * full (caller inserts inline so a row is never dropped). flush blocks until the
 * queue has fully drained — used for clean shutdown and by the load test to
 * count landed rows. */
int agent_audit_async_enqueue_row(const void *row)
{
   if (!row)
      return -1;
   return audit_async_enqueue((const db1_token_audit_row_t *)row);
}

void agent_audit_async_flush(void)
{
   pthread_mutex_lock(&g_audit_mtx);
   while (g_audit_pending > 0)
      pthread_cond_wait(&g_audit_drained, &g_audit_mtx);
   pthread_mutex_unlock(&g_audit_mtx);
}

/* Per-thread ingress source: the /v1/runs worker sets this so spend logged via
 * agent_log_call (source="agent") from inside the run loop is retagged with the
 * ingress origin instead — distinguishing it from internal execution, no 2nd row.
 * The runs worker is a dedicated detached thread, so it is scoped to that turn. */
static __thread char g_ingress_source[40] = "";

/* The agent_log row id for the call currently being logged, set by agent_log_call
 * so the token_audit row it writes links back 1:1. 0 outside agent_log_call (e.g.
 * direct ingress writes, which have no agent_log row). */
static __thread long long g_agent_log_id = 0;

void agent_set_ingress_source(const char *source)
{
   snprintf(g_ingress_source, sizeof(g_ingress_source), "%s", source ? source : "");
}

int agent_ingress_accounting_enabled(void)
{
   config_t cfg;
   return config_load(&cfg) == 0 && cfg.ingress_usage_accounting_enabled;
}

void agent_record_token_audit(const agent_result_t *result, const char *role, const char *source)
{
   agent_record_token_audit_kind(result, role, source, "realized");
}

void agent_record_token_audit_kind(const agent_result_t *result, const char *role,
                                   const char *source, const char *usage_kind)
{
   if (!result)
      return;

   /* Per-request idempotency + attribution (#3): the request context carries the
    * ingress request id, the client principal/account boundary, and — when a
    * trusted proxy stamped one — a per-client source (e.g. "webchat"). */
   const request_context_t *rctx = request_context_get();

   /* Effective turn-origin source precedence: (1) a thread-scoped override set by
    * an ingress worker such as /v1/runs, (2) a trusted request-supplied source
    * (X-Aimee-Source via a trusted proxy) — but ONLY refining an ingress row, so
    * direct ingress rows distinguish webchat from another OpenAI-compatible client
    * while internal "agent" rows on the same request thread stay "agent", (3) the
    * caller's constant. */
   const char *base_source = source ? source : "";
   const char *eff_source;
   if (g_ingress_source[0])
      eff_source = g_ingress_source;
   else if (rctx && rctx->source[0] && strcmp(base_source, "agent") != 0)
      eff_source = rctx->source;
   else
      eff_source = base_source;

   /* ingress_audit_async rollout knob: an ingress row may be handed to the
    * background writer so the response is not blocked on the DB insert. The
    * master accounting gate (ingress_usage_accounting_enabled) is enforced by the
    * ingress call sites (see agent_ingress_accounting_enabled), NOT here, so this
    * shared primitive carries no policy. "is ingress" is any non-internal origin
    * (anything but the internal "agent"/empty source). The config load is paid
    * only for ingress rows and is mtime-cached. */
   int is_ingress = eff_source[0] && strcmp(eff_source, "agent") != 0;
   int async = 0;
   if (is_ingress)
   {
      config_t gcfg;
      if (config_load(&gcfg) == 0)
         async = gcfg.ingress_audit_async;
   }

   token_usage_t usage = {
       .input_tokens = result->prompt_tokens,
       .output_tokens = result->completion_tokens,
       .cache_write_tokens = result->cache_write_tokens,
       .cache_read_tokens = result->cache_read_tokens,
   };
   /* Bill against the model actually served to the provider, not the agent
    * identity: an agent named "codex" may serve "gpt-5.4", and a turn-0 400 may
    * have swapped in the fallback model. Fall back to the agent name only when
    * no served model was recorded (e.g. the dedup cache-hit path). */
   const char *bill_model = result->model[0] ? result->model : result->agent_name;
   double cost = token_estimate_cost(bill_model, &usage);
   /* Tagging the audit row with the active delegation id lets cost-fold attribute
    * a child's spend back to the parent without contaminating session_id sums. */
   const char *deleg_id = delegation_active_id();
   /* Per-CLIENT attribution for ingress rows, in one consistent field so
    * top_sessions distinguishes distinct clients (it groups by session_id):
    *   (1) a trusted X-Aimee-Session-Key (e.g. "webchat:alice") — distinguishes
    *       clients that share a constant trusted principal like "webchat";
    *   (2) else, for ingress, the principal (e.g. "uid:1000") — distinguishes
    *       clients that share the server's process-wide session;
    *   (3) else the server session().
    * Internal agent rows (not ingress) always use session() so internal work is
    * grouped by its real session, not by the requesting principal. */
   const char *eff_session;
   if (rctx && rctx->session_key[0])
      eff_session = rctx->session_key;
   else if (is_ingress && rctx && rctx->principal[0])
      eff_session = rctx->principal;
   else
      eff_session = session_id();
   db1_token_audit_row_t row = {
       .session_id = eff_session,
       .delegation_id = deleg_id ? deleg_id : "",
       .project_name = "",
       .tool_name = result->agent_name,
       .role = role ? role : "",
       /* The served model (consistent with the cost key above), so the by-model
        * breakdown attributes spend to the real model rather than the agent. */
       .model = bill_model,
       .source = eff_source,
       .requested_model = result->requested_model,
       .stop_reason = result->stop_reason,
       .agent_log_id = g_agent_log_id,
       .usage_kind = usage_kind ? usage_kind : "realized",
       .request_id = rctx ? rctx->request_id : "",
       .idempotency_key = rctx ? rctx->idempotency_key : "",
       .attempt = 0,
       .principal = rctx ? rctx->principal : "",
       /* Served model (what aimee selected) distinct from the provider-reported
        * `model`; falls back to model when not separately recorded. */
       .served_model = result->served_model[0] ? result->served_model : bill_model,
       .duration_ms = result->latency_ms,
       .prompt_tokens = usage.input_tokens,
       .completion_tokens = usage.output_tokens,
       .cache_write_tokens = usage.cache_write_tokens,
       .cache_read_tokens = usage.cache_read_tokens,
       .estimated_cost_usd = cost,
   };
   /* Async only for ingress rows (where the response is waiting); fall back to an
    * inline insert when async is off or the ring is full. */
   if (!async || audit_async_enqueue(&row) != 0)
      (void)db1_token_audit_insert(&row);
}

void agent_log_call(const agent_result_t *result, const char *role)
{
   db1_agent_log_insert_row_t row = {
       .agent_name = result->agent_name,
       .role = role ? role : "",
       .prompt_tokens = result->prompt_tokens,
       .completion_tokens = result->completion_tokens,
       .latency_ms = result->latency_ms,
       .success = result->success,
       .error = result->error[0] ? result->error : NULL,
       .turns = result->turns,
       .tool_calls = result->tool_calls,
       .confidence = result->confidence,
       .session_id = NULL,
   };
   long long log_id = db1_agent_log_insert(&row);

   /* Link the cost row to this agent_log row (1:1) for agent stats, then clear. */
   g_agent_log_id = log_id > 0 ? log_id : 0;
   /* Internal agent/delegate execution; tag the cost row accordingly. */
   agent_record_token_audit(result, role, "agent");
   g_agent_log_id = 0;
}
