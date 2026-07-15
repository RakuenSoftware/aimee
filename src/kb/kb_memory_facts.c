/* kb_memory_facts.c: background LLM extraction of typed facts from memories.
 * See kb_memory_facts.h. Mirrors the claim/done/fail job lifecycle of
 * kb_curator_extract.c against kb_async_jobs (kind='memory_facts'). */
#include "kb_memory_facts.h"
#include "kb_curator_sidecar.h"

#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "kb_curator_llm.h"
#include "log.h"

#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "db2/fact_lifecycle.h"  /* FACT_AUTHORITY_MODEL */
#include "db2/memory_query.h"    /* db2_memory_get */
#include "db2/rel_types_store.h" /* db2_fact_commit */
#include "db2/fact_ingest.h"     /* db2_fact_ingest_text (offline pattern extraction) */
#include "rel_types.h"           /* seed ontology: rel_types_seed_* (extractor constraint, §7) */
#include "memory.h"              /* memory_t */
#include "memory_ontology.h"     /* NODE_* */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <time.h>    /* time() — reclaim throttle */

#define MF_ERRBUF       256
#define MF_LLM_OUT_CAP  8192
#define MF_MAX_ATTEMPTS 3
/* A job left in 'running' longer than this lease was orphaned (worker crash/
 * restart or a wedged LLM call). Mirrors the curator stages' lease. */
#define MF_STALE_LEASE "-15 minutes"
/* Reclaim runs at most this often (throttled; the drain calls it per batch). */
#define MF_RECLAIM_EVERY_S 60
/* Auto-injected into every turn (ingress_preinject), so precision matters more
 * than recall: only commit facts the model is confident are durable. */
#define MF_CONF_FLOOR 0.6

/* The model must return ONLY durable, generalizable subject-relation-object
 * facts -- not transient state, opinions, or one-off events. relation is a short
 * snake_case predicate; object is the value. Conservative by design: an empty
 * list is the right answer when the text asserts no durable fact. */
/* Extraction prompt template. The `%s` is filled with the canonical relation list
 * from the seed ontology (rel_types.c) at run time — this is the autonomous
 * reconciliation step (proposal §7): the model is bound to relations the write
 * gate already treats as durable, so an extracted fact commits ACTIVE and
 * recallable instead of being stranded as a provisional Class-C edge. When no
 * seed relation fits, the model emits its own concise predicate (never a generic
 * catch-all), which stays a distinguishable provisional candidate for §7.2. */
#define MF_SYSTEM_PROMPT_TMPL                                                                      \
   "You extract durable facts from a single remembered note. Return ONLY a JSON "                  \
   "object: {\"facts\":[{\"subject\":\"\",\"relation\":\"\",\"object\":\"\","                      \
   "\"confidence\":0.0}]}. Each fact is a stable subject-relation-object triple "                  \
   "grounded strictly in the note. For relation, choose the single nearest fit "                   \
   "from these canonical predicates when one reasonably applies: %s. If NONE fits, "               \
   "emit a concise snake_case predicate of your own (e.g. drives, founded, "                       \
   "mentors) — NEVER a generic catch-all such as "                                                 \
   "\"other\"/\"unknown\"/\"misc\". subject is the entity the fact is about "                      \
   "(use \"user\" for the note's author when it is first-person). "                                \
   "confidence is 0..1. Extract only durable, generalizable facts; skip transient "                \
   "state, feelings, plans, and one-off events. If the note asserts no durable "                   \
   "fact, return an empty list. No prose, no markdown."

/* Build the extraction system prompt, binding the model to the canonical relation
 * set (autonomous reconciliation, §7). Sourced from the seed ontology so it stays
 * in lockstep with the write gate — no second copy of the relation list. */
static void mf_build_system_prompt(char *buf, size_t cap)
{
   char rels[768];
   size_t p = 0;
   int n = rel_types_seed_count();
   for (int i = 0; i < n && p < sizeof(rels) - 1; i++)
   {
      const rel_type_def_t *d = rel_types_seed_at(i);
      if (!d || !d->rel_type || !d->rel_type[0])
         continue;
      p += (size_t)snprintf(rels + p, sizeof(rels) - p, "%s%s", p ? ", " : "", d->rel_type);
   }
   if (!p) /* defensive: an empty seed would leave the model unconstrained */
      snprintf(rels, sizeof(rels), "works_for, has_role, lives_in, born_in");
   snprintf(buf, cap, MF_SYSTEM_PROMPT_TMPL, rels);
}

typedef struct
{
   int64_t job_id;
   int64_t memory_id;
   int attempts;
} mf_job_t;

static int mf_claim_job(mf_job_t *out)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "UPDATE kb_async_jobs"
                            " SET status = 'running',"
                            "     claimed_by = 'kb.memory.facts',"
                            "     claimed_at = pg_now_text(),"
                            "     attempts   = attempts + 1,"
                            "     updated_at = pg_now_text()"
                            " WHERE id = ("
                            "   SELECT id FROM kb_async_jobs"
                            "   WHERE kind = 'memory_facts' AND status = 'pending'"
                            /* Skip jobs still serving their retry backoff. */
                            "     AND (next_attempt_at = '' OR next_attempt_at <= ?1)"
                            "   ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED"
                            " )"
                            " RETURNING id, document_id, attempts";

   char err[MF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   char now_text[32];
   kb_curator_now_text(now_text, sizeof(now_text));
   aimee_pg_bind_text(st, "?1", now_text);

   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->job_id = aimee_pg_column_int64(st, 0);
      out->memory_id = aimee_pg_column_int64(st, 1);
      out->attempts = aimee_pg_column_int(st, 2);
      found = 1;
   }
   aimee_pg_finalize(st);
   return found;
}

static void mf_mark_done(int64_t job_id)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[MF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE kb_async_jobs SET status='done', updated_at=pg_now_text() WHERE id=?1", err,
       sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", job_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

static void mf_mark_retry_or_fail(int64_t job_id, int attempts, const char *error_msg)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   const char *new_status = (attempts >= MF_MAX_ATTEMPTS) ? "failed" : "pending";
   char err[MF_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "UPDATE kb_async_jobs SET status=?1, last_error=?2, next_attempt_at=?4,"
                        " updated_at=pg_now_text() WHERE id=?3",
                        err, sizeof(err));
   if (!st)
      return;
   char next_at[32];
   kb_curator_next_attempt_at(attempts, next_at, sizeof(next_at));
   aimee_pg_bind_text(st, "?4", next_at);
   aimee_pg_bind_text(st, "?1", new_status);
   aimee_pg_bind_text(st, "?2", error_msg ? error_msg : "");
   aimee_pg_bind_int64(st, "?3", job_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

/* Reclaim memory_facts jobs orphaned in 'running'. Identical in shape and
 * rationale to the extract_doc reclaim in kb_curator_extract.c: mf_claim_job
 * only ever selects status='pending', so a job whose worker crashed or whose
 * LLM call wedged stays 'running' forever — never retried, and pinning a db2
 * pool member past its ceiling. Reset rows older than the lease to 'pending',
 * or to 'failed' once attempts are exhausted so a poison job cannot loop.
 * attempts is preserved (incremented at claim time).
 *
 * Scoped to kind='memory_facts' so it cannot disturb the other stages sharing
 * kb_async_jobs. Throttled — the drain calls this once per batch. Unlike the
 * extract_doc reclaim, the throttle needs no lock: kb_memory_facts_drain has a
 * single caller on the drain thread (no worker pool), so this is single-entry. */
static void mf_reclaim_stale_running(void)
{
   static time_t last_run = 0;
   time_t now = time(NULL);
   if (last_run != 0 && now - last_run < MF_RECLAIM_EVERY_S)
      return;

   void *conn = db2_conn();
   if (!conn)
      return; /* never ran: leave the throttle unarmed so the next call retries */
   char err[MF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE kb_async_jobs"
       " SET status = CASE WHEN attempts >= ?1 THEN 'failed' ELSE 'pending' END,"
       "     claimed_by = '', claimed_at = '',"
       /* Preserve an existing diagnostic; see the extract_doc reclaim. */
       "     last_error = CASE WHEN attempts >= ?1 AND last_error = ''"
       "                       THEN 'stale running lease reclaimed after max attempts'"
       "                       ELSE last_error END,"
       "     updated_at = pg_now_text()"
       " WHERE kind = 'memory_facts' AND status = 'running'"
       "   AND claimed_at <> '' AND claimed_at < pg_now_text(?2)",
       err, sizeof(err));
   if (!st)
   {
      aimee_log(LOG_WARN, "kb.memory.facts", "stale-lease reclaim could not prepare: %s", err);
      return;
   }
   aimee_pg_bind_int(st, "?1", MF_MAX_ATTEMPTS);
   aimee_pg_bind_text(st, "?2", MF_STALE_LEASE);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      last_run = now; /* only a completed UPDATE arms the throttle */
   else
      aimee_log(LOG_WARN, "kb.memory.facts", "stale-lease reclaim failed: %s", err);
   aimee_pg_finalize(st);
}

/* Subject kind: a first-person/user subject or a capitalized name is a person;
 * otherwise NODE_OTHER. The gate only enforces kinds for its seed relations, so
 * an imperfect guess at most costs a skipped seed-relation commit. */
static memory_node_kind_t mf_subject_kind(const char *subject)
{
   if (!subject || !subject[0])
      return NODE_OTHER;
   if (strcasecmp(subject, "user") == 0 || strcasecmp(subject, "i") == 0)
      return NODE_PERSON;
   return (subject[0] >= 'A' && subject[0] <= 'Z') ? NODE_PERSON : NODE_OTHER;
}

/* Parse {"facts":[...]} and commit each triple above the confidence floor.
 * Returns the number committed (ACCEPT or NOVEL). */
static int mf_commit_facts(const char *llm_json)
{
   if (!llm_json)
      return 0;
   /* Models often wrap the JSON in ```json ... ``` fences or add a sentence of
    * prose despite instructions. Parse the outermost {...} object so a fenced or
    * prefixed response still yields facts. */
   const char *start = strchr(llm_json, '{');
   const char *end = strrchr(llm_json, '}');
   if (!start || !end || end < start)
      return 0;
   size_t span = (size_t)(end - start) + 1;
   char *obj = malloc(span + 1);
   if (!obj)
      return 0;
   memcpy(obj, start, span);
   obj[span] = '\0';
   cJSON *root = cJSON_Parse(obj);
   free(obj);
   if (!root)
      return 0;
   cJSON *facts = cJSON_GetObjectItemCaseSensitive(root, "facts");
   if (!cJSON_IsArray(facts))
   {
      cJSON_Delete(root);
      return 0;
   }

   int committed = 0;
   cJSON *f = NULL;
   cJSON_ArrayForEach(f, facts)
   {
      if (!cJSON_IsObject(f))
         continue;
      const cJSON *subj_j = cJSON_GetObjectItemCaseSensitive(f, "subject");
      const cJSON *rel_j = cJSON_GetObjectItemCaseSensitive(f, "relation");
      const cJSON *obj_j = cJSON_GetObjectItemCaseSensitive(f, "object");
      const cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(f, "confidence");
      const char *subject = cJSON_IsString(subj_j) ? subj_j->valuestring : "";
      const char *relation = cJSON_IsString(rel_j) ? rel_j->valuestring : "";
      const char *object = cJSON_IsString(obj_j) ? obj_j->valuestring : "";
      double conf = cJSON_IsNumber(conf_j) ? conf_j->valuedouble : 0.0;

      if (!subject[0] || !relation[0] || !object[0] || conf < MF_CONF_FLOOR)
         continue;

      /* The extractor supplies no node kinds, so guess: subject via
       * mf_subject_kind, object OTHER (unknown). But the kind gate REJECTS a
       * seed relation whose endpoint kind mismatches its ontology def (e.g.
       * works_for wants tail=ORG) — a wrong guess silently drops every such
       * fact. For a known seed relation, take the endpoint kinds the relation
       * itself implies (its def's canonical kinds) whenever the guess is not
       * already allowed; novel relations keep the guess (not kind-checked). */
      memory_node_kind_t subj_kind = mf_subject_kind(subject);
      memory_node_kind_t obj_kind = NODE_OTHER;
      const rel_type_def_t *sdef = rel_types_seed_lookup(relation);
      if (sdef)
      {
         if (!rel_type_kind_allowed(sdef, 1, subj_kind) && sdef->head_kind_count > 0)
            subj_kind = sdef->head_kinds[0];
         if (!rel_type_kind_allowed(sdef, 0, obj_kind) && sdef->tail_kind_count > 0)
            obj_kind = sdef->tail_kinds[0];
      }
      fact_gate_verdict_t v =
          db2_fact_commit(subject, subj_kind, relation, object, obj_kind, FACT_AUTHORITY_MODEL, 1);
      if (v == FACT_GATE_ACCEPT || v == FACT_GATE_NOVEL)
         committed++;
   }
   cJSON_Delete(root);
   return committed;
}

static int mf_process_one(const config_t *cfg, const mf_job_t *job)
{
   memory_t mem;
   memset(&mem, 0, sizeof(mem));
   if (db2_memory_get(job->memory_id, &mem) != 0 || !mem.content[0])
   {
      /* Memory gone or empty -- nothing to extract; treat as done. */
      mf_mark_done(job->job_id);
      return 0;
   }

   /* Deterministic pattern-first extraction, moved off the synchronous store/turn
    * path to the drain: high-precision regex triples committed idempotently. Runs
    * before the LLM pass so obvious facts ("my name is X") still land even if the
    * LLM sidecar is unavailable or the job later exhausts its retries. */
   (void)db2_fact_ingest_text(mem.content, FACT_AUTHORITY_USER, 1);

   cJSON *req = cJSON_CreateObject();
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "content", mem.content);
   char *request_json = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!request_json)
      return -1;

   char sys_prompt[2560];
   mf_build_system_prompt(sys_prompt, sizeof(sys_prompt));

   char err[MF_ERRBUF] = "";
   char *resp = kb_curator_llm_run(cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, sys_prompt, request_json,
                                   NULL, "", MF_LLM_OUT_CAP, err, sizeof(err));
   free(request_json);
   if (!resp)
   {
      mf_mark_retry_or_fail(job->job_id, job->attempts, err[0] ? err : "llm run failed");
      return -1;
   }

   int n = mf_commit_facts(resp);
   free(resp);
   mf_mark_done(job->job_id);
   if (n > 0)
      aimee_log(LOG_INFO, "kb.memory.facts", "memory %lld -> %d typed fact(s)",
                (long long)job->memory_id, n);
   return n;
}

int kb_memory_facts_drain(const config_t *cfg, int batch)
{
   if (!cfg || !cfg->typed_facts_enabled || batch <= 0)
      return 0;
   if (!db2_conn())
      return 0;

   /* Recover jobs orphaned in 'running' before claiming fresh work, so a crash/
    * restart or a previously-wedged LLM call cannot permanently strand them. */
   mf_reclaim_stale_running();

   int processed = 0;
   for (int i = 0; i < batch; i++)
   {
      mf_job_t job;
      memset(&job, 0, sizeof(job));
      if (!mf_claim_job(&job))
         break;
      (void)mf_process_one(cfg, &job);
      processed++;
   }
   return processed;
}
