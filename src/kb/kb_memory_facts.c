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
#include "fact_grounding.h"
#include "rel_types.h"           /* seed ontology: rel_types_seed_* (extractor constraint, §7) */
#include "memory.h"              /* memory_t */
#include "memory_ontology.h"     /* NODE_* */

#include <ctype.h>
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
 * than recall: a wrong fact is repeated back forever.
 *
 * This used to be a floor on the model's self-reported confidence (>= 0.6).
 * Measured across 18 extraction models on the Tier-A benchmark, that number
 * carries almost no signal: most models write exactly 0.0 or exactly 0.9 and
 * nothing between, several write 0.0 for every fact including the ones they get
 * right, and for one model the low-confidence facts are MORE accurate than the
 * confident ones. The floor silently discarded everything four models extracted
 * — Qwen3-0.6B commits nothing at 0.6, while 40% of what it extracts is correct.
 *
 * Replaced with a check on the text instead of on the model's opinion of itself:
 * a fact commits only if both of its endpoints can be traced back to the note.
 * That needs no calibration, behaves identically for every provider, and catches
 * the failure that actually matters here — an invented entity. A hallucinated
 * person carries a confidence of 0.9 just as happily as a real one. */

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
 * catch-all), which stays a distinguishable provisional candidate for §7.2.
 *
 * The closing sentence grants reasoning explicitly, and that phrasing is load
 * bearing. It used to read "No prose, no markdown.", which gemma-4-E4B applies to
 * its own thought channel and not merely to its answer: reasoning drops to zero on
 * every note, silently, with valid JSON still coming back.
 *
 * What that costs is smaller than it was once reported. The widely-quoted "+0.084
 * F1 from thinking" came from a 70-note sweep with no interval on it. Paired over
 * 955 notes of the 10k corpus the strict-F1 delta is +0.010, CI [-0.020, +0.040] —
 * indistinguishable. The gain is real but shows up elsewhere: scored on entity
 * pairs rather than exact predicate names, recall goes 0.712 -> 0.828 at flat
 * precision, because a reasoning model finds more facts AND names them more
 * variably, and strict scoring charges that variance twice. Abstention gets worse
 * (0.907 -> 0.870). See MEASUREMENT_LOG.md defect 32.
 *
 * Rescoping the constraint to the answer ("the answer itself must be a JSON object
 * only") does NOT fix it — still 0/20 — so the model is not drawing that
 * distinction and the exemption has to be stated outright. Deleting the sentence
 * restores thinking but costs the guardrail: 14 of 20 answers come back fenced in
 * ```json. Granting reasoning and constraining the answer keeps both, and fences
 * least of the three (0 of 20).
 *
 * Measured on gemma-4-E4B under two independent builds — Unsloth UD-Q4_K_XL and
 * the stock ggml-org Q8_0, which ship different chat templates — so this is a
 * property of the model rather than of one vendor's quantisation. E2B never had
 * the behaviour at all. See bench/tier-a/harness/probe_thinking_prompt.py. */
#define MF_SYSTEM_PROMPT_TMPL                                                                \
   "You extract durable facts from a single remembered note. Return ONLY a JSON "                  \
   "object: {\"facts\":[{\"subject\":\"\",\"relation\":\"\",\"object\":\"\","                      \
   "\"confidence\":0.0,\"negated\":false}]}. Each fact is a stable "                               \
   "subject-relation-object triple "                                                               \
   "grounded strictly in the note. For relation, choose the single nearest fit "                   \
   "from these canonical predicates when one reasonably applies: %s. If NONE fits, "               \
   "emit a concise snake_case predicate of your own (e.g. drives, founded, "                       \
   "mentors) — NEVER a generic catch-all such as "                                                 \
   "\"other\"/\"unknown\"/\"misc\". subject is the entity the fact is about "                      \
   "(use \"user\" for the note's author when it is first-person). "                                \
   "confidence is 0..1. Extract only durable, generalizable facts; skip transient "                \
   "state, feelings, plans, and one-off events. BUT an event that ESTABLISHES a "                  \
   "durable state yields that state: \"joined X\" gives membership of X, \"moved "                 \
   "to Y\" gives location Y, \"was promoted to Z\" gives role Z. Record the "                      \
   "resulting state, not the event. If the note RETRACTS or DENIES "                               \
   "something (\"no longer\", \"did not\", \"never\", \"is not\", \"has left\", "                  \
   "\"was removed\"), emit the ORIGINAL fact it retracts with \"negated\":true - "                 \
   "use the same canonical relation the positive fact would use, NEVER a negative "                \
   "predicate of your own such as not_member_of or removed_from. \"Kestrel Freight "               \
   "is no longer a customer\" is {\"subject\":\"Kestrel Freight\",\"relation\":"                   \
   "\"member_of\",\"object\":\"customer\",\"negated\":true}. A note that MOVES "                   \
   "something gives both: the new fact, and the old one negated. A RENAME is NOT "                 \
   "a retraction: \"A is now called B\" means A and B are the same thing, so emit "                \
   "also_known_as with negated FALSE. For an ordinary "                                            \
   "fact that is simply true, omit \"negated\" or set it false. "                                  \
   "If the note asserts no durable fact, return exactly {\"facts\":[]} - the "                     \
   "wrapper object is ALWAYS required, never a bare []. Reason first if it helps; "                 \
   "the answer that follows must be the JSON object only, no prose, no markdown."

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
static int mf_commit_facts(const char *llm_json, const char *note)
{
   if (!llm_json)
      return 0;
   /* Models often wrap the JSON in ```json ... ``` fences or add a sentence of
    * prose despite instructions. Parse the outermost {...} object so a fenced or
    * prefixed response still yields facts. */
   /* A bare "[]" is a CORRECT abstention, not a malformed response. Small models
    * write it instead of {"facts":[]} on notes that assert no durable fact —
    * 297 of 1000 notes in the tier-A small-corpus run, of which 184 were notes
    * whose gold is deliberately empty. Both spellings mean "nothing to commit",
    * and both return 0 here, so the two are indistinguishable to the caller and
    * to anyone reading a log. Report the abstention explicitly so a future
    * "the model emits nothing" investigation can tell refusal from garbage. */
   const char *p = llm_json;
   while (*p && isspace((unsigned char)*p))
      p++;
   if (p[0] == '[' && p[1] == ']')
   {
      aimee_log(LOG_DEBUG, "kb.memory.facts",
                "note yielded an explicit empty extraction ('[]')");
      return 0;
   }
   const char *start = strchr(llm_json, '{');
   const char *end = strrchr(llm_json, '}');
   if (!start || !end || end < start)
   {
      aimee_log(LOG_WARN, "kb.memory.facts",
                "response contained no JSON object - nothing committed");
      return 0;
   }
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

   char note_norm[4096];
   fact_norm_text(note, note_norm, sizeof(note_norm));

   int committed = 0;
   int retracted = 0;
   int ungrounded = 0;
   int malformed = 0;
   cJSON *f = NULL;
   cJSON_ArrayForEach(f, facts)
   {
      if (!cJSON_IsObject(f))
         continue;
      const cJSON *subj_j = cJSON_GetObjectItemCaseSensitive(f, "subject");
      const cJSON *rel_j = cJSON_GetObjectItemCaseSensitive(f, "relation");
      const cJSON *obj_j = cJSON_GetObjectItemCaseSensitive(f, "object");
      const cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(f, "confidence");
      const cJSON *neg_j = cJSON_GetObjectItemCaseSensitive(f, "negated");
      const char *subject = cJSON_IsString(subj_j) ? subj_j->valuestring : "";
      const char *raw_relation = cJSON_IsString(rel_j) ? rel_j->valuestring : "";
      const char *object = cJSON_IsString(obj_j) ? obj_j->valuestring : "";
      double conf = cJSON_IsNumber(conf_j) ? conf_j->valuedouble : 0.0;
      /* Polarity on the ORIGINAL fact, rather than a negative predicate of the
       * model's own invention. An absent field is false, so a model that never
       * emits it behaves exactly as before. */
      int negated = cJSON_IsTrue(neg_j);

      if (!subject[0] || !raw_relation[0] || !object[0])
      {
         malformed++;
         continue;
      }
      /* Both endpoints must be traceable to the note. Counted, not silent: a
       * drop that commits nothing used to look exactly like "the model cannot
       * extract". */
      if (!fact_grounded(subject, note_norm) || !fact_grounded(object, note_norm))
      {
         ungrounded++;
         continue;
      }
      (void)conf; /* recorded by the model, not trusted — see the note above */

      /* Fold a known synonym onto its canonical seed relation before anything
       * downstream sees it: the gate would otherwise return NOVEL for "has_ip"
       * and stage a provisional rel_type on a Class-C edge, when we already
       * model exactly that relation as device_has_ip. Entities have had this
       * (db2_entity_alias_bind); relations have not. Unknown labels pass through
       * normalized, so a genuinely new predicate still stages for §7.2. */
      char relation_buf[REL_TYPE_NAME_MAX];
      rel_type_canonicalize(raw_relation, relation_buf, sizeof(relation_buf));
      const char *relation = relation_buf[0] ? relation_buf : raw_relation;

      /* Endpoint kinds are ADOPTED FROM THE RELATION, not validated against it.
       *
       * Say that plainly because the shape of the code invites the opposite
       * reading. The extractor returns no kinds, so the object starts as
       * NODE_OTHER — "nobody told me" — and 14 of the 17 seed relations do not
       * list NODE_OTHER as an allowed tail, so the branch below rewrites it on
       * EVERY extraction. FACT_GATE_REJECT_KIND is therefore unreachable for
       * objects on this path: the gate cannot refuse a mistyped object, it
       * relabels it.
       *
       * That is deliberate and it stays. Without it a seed relation drops every
       * fact it extracts, which is the bug this replaced. What is NOT true is
       * that the kinds are checked, and the previous comment implied they were.
       *
       * Inference was considered and measured rather than assumed: across 1333
       * seed-relation triples from two 1k runs, the only tail a text rule can
       * confidently judge is device_has_ip, and all 96 of those objects are
       * valid IPs. A text-based kind check would have caught zero mistyped
       * objects, so it would be cost without benefit. The mistyped triples that
       * DO occur ("member_of enterprise", "has_role <contract>") are wrong in a
       * way no kind rule can see — the object is a plausible string of the
       * declared kind.
       *
       * The stored kinds are inert: nothing reads entity_edges.subject_kind or
       * .object_kind back, for gating or recall. If that changes, this is where
       * the fabricated value comes from. */
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
      /* A retraction deactivates the edge it names instead of asserting a new
       * one. Routed to the same API the pattern extractor already uses
       * (fact_ingest.c), which is where the bitemporal supersede semantics, the
       * immutable-edge refusal and the §4/§5 authority guard live — a
       * MODEL-authority retraction cannot touch a user-stated Class-A edge.
       *
       * Passing `object` matters and is the reason polarity rides on the
       * original fact rather than on a not_* predicate: the header is explicit
       * that `target` scopes the retraction to the specific
       * {source, relation, target} edge, where NULL retracts every current value
       * of (source, relation). A model that says "no longer in
       * drivers/staging/comedi" should not blank the file's location outright.
       *
       * Until now nothing on the LLM path could reach this. "I no longer work at
       * X" was handled by memory_pattern_is_retraction() for first-person user
       * attributes, and everything else -- every third-party fact, which is most
       * of them -- was dropped on the floor by a prompt that told the model a
       * retraction had nothing durable to record. */
      if (negated)
      {
         int n = db2_fact_retract(subject, relation, object, FACT_AUTHORITY_MODEL);
         if (n > 0)
            retracted += n;
         else if (n == FACT_RETRACT_IMMUTABLE)
            aimee_log(LOG_INFO, "kb.memory.facts",
                      "retraction refused as immutable: %s %s %s", subject, relation, object);
         continue;
      }
      fact_gate_verdict_t v =
          db2_fact_commit(subject, subj_kind, relation, object, obj_kind, FACT_AUTHORITY_MODEL, 1);
      if (v == FACT_GATE_ACCEPT || v == FACT_GATE_NOVEL)
         committed++;
   }
   /* A provider whose confidences are uninformative drops every fact here, and
    * the job still reports success — that combination once looked exactly like
    * "the model cannot extract". Say so instead of committing silently. */
   if (ungrounded > 0 || malformed > 0)
      aimee_log(committed == 0 && retracted == 0 ? LOG_WARN : LOG_INFO, "kb.memory.facts",
                "dropped %d ungrounded + %d malformed fact(s), committed %d%s",
                ungrounded, malformed, committed,
                committed == 0 && retracted == 0 ? " - NOTHING committed for this note" : "");
   /* Retractions are reported separately: a note that only deactivates edges
    * commits nothing, and without this it is indistinguishable from a note the
    * extractor failed on. */
   if (retracted > 0)
      aimee_log(LOG_INFO, "kb.memory.facts", "retracted %d edge(s) for this note", retracted);
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

   /* The job row and source memory are already copied locally. Release the
    * worker's lazy pool lease before the potentially multi-minute LLM call;
    * retry/done writes below safely re-acquire it. */
   db2_lease_release_idle();

   char err[MF_ERRBUF] = "";
   char *resp = kb_curator_llm_run(cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, sys_prompt, request_json,
                                   NULL, "", MF_LLM_OUT_CAP, err, sizeof(err));
   free(request_json);
   if (!resp)
   {
      mf_mark_retry_or_fail(job->job_id, job->attempts, err[0] ? err : "llm run failed");
      return -1;
   }

   int n = mf_commit_facts(resp, mem.content);
   free(resp);
   mf_mark_done(job->job_id);
   if (n > 0)
      aimee_log(LOG_INFO, "kb.memory.facts", "memory %lld -> %d typed fact(s)",
                (long long)job->memory_id, n);
   return n;
}

int kb_memory_facts_drain(const config_t *cfg, int batch)
{
   if (!cfg || !config_typed_facts_enabled() || batch <= 0)
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
