/* fact_mutation.c: mandatory authority/evidence/lifecycle seam for semantic facts. */
#include "fact_mutation.h"

#include "db2_internal.h"
#include "db_postgres.h"
#include "kb_audit_worm.h"
#include "platform_random.h"
#include "../headers/kb_identity.h"
#include "../headers/kb_reqctx.h"
#include "../headers/rel_types.h"
#include "fact_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FM_ERRBUF    256
#define FM_STATE_MAX 64

/* fact_mutation is linked into non-KB test/server binaries too.  Request context
 * is a KB-only facility, so keep these optional; background mutations never
 * consult them. */
extern const kb_principal_t *kb_reqctx_actor(void) __attribute__((weak));
extern const kb_request_context_t *kb_reqctx_resolved(void) __attribute__((weak));
extern int kb_reqctx_verified_scope(const char **kind, const char **id) __attribute__((weak));
extern int kb_identity_key(const kb_principal_t *, char *, size_t) __attribute__((weak));

typedef struct
{
   int found;
   int64_t id;
   char lifecycle[24];
   char superseded_at[32];
   char invalidated_at[32];
   int suppressed;
   double confidence;
   int authority_rank;
   int version;
} fm_state_t;

typedef struct
{
   int64_t id;
   int existed;
   fm_state_t before;
   char kind[32];
   char key[128];
   char action[32];
} fm_rollback_item_t;

static void fm_now(char out[32])
{
   time_t now = time(NULL);
   struct tm tmv;
   gmtime_r(&now, &tmv);
   strftime(out, 32, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void fm_commit_id(char out[FACT_COMMIT_ID_MAX])
{
   unsigned char raw[16];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      memset(raw, 0, sizeof(raw));
   snprintf(out, FACT_COMMIT_ID_MAX,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x%02x%02x%02x%02x",
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10],
            raw[11], raw[12], raw[13], raw[14], raw[15]);
}

static int fm_actor_ok(const fact_actor_t *actor)
{
   return actor && actor->principal[0] && actor->role[0] && actor->rank >= FACT_ACTOR_MODEL &&
          actor->rank <= FACT_ACTOR_OPERATOR;
}

static int fm_tombstone_blocks(void *conn, const char *source, const char *relation,
                               const char *target)
{
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT 1 FROM memory_rejection_tombstones WHERE object_kind='fact' AND active=1"
       " AND source=?1 AND relation=?2 AND target=?3 LIMIT 1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", source);
   aimee_pg_bind_text(st, "?2", relation);
   aimee_pg_bind_text(st, "?3", target);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW ? 1 : step == AIMEE_PG_DONE ? 0 : -1;
}

static int fm_tombstone_add_assertion(void *conn, int64_t assertion_id, const fact_actor_t *actor,
                                      const char *reason)
{
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO memory_rejection_tombstones(object_kind,source,relation,target,"
       " authority_rank,reason,rejected_by)"
       " SELECT 'fact',source,relation,target,?2,?3,?4 FROM entity_edges WHERE id=?1"
       " ON CONFLICT DO NOTHING",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", assertion_id);
   aimee_pg_bind_int(st, "?2", (int)actor->rank);
   aimee_pg_bind_text(st, "?3", reason ? reason : "explicit rejection");
   aimee_pg_bind_text(st, "?4", actor->principal);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

static int fm_tombstone_restore_assertion(void *conn, int64_t assertion_id,
                                          const fact_actor_t *actor)
{
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "UPDATE memory_rejection_tombstones AS t SET "
                        "active=0,restored_at=pg_now_text(),restored_by=?2"
                        " WHERE t.object_kind='fact' AND t.active=1 AND EXISTS"
                        " (SELECT 1 FROM entity_edges e WHERE e.id=?1 AND e.source=t.source"
                        "  AND e.relation=t.relation AND e.target=t.target)",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", assertion_id);
   aimee_pg_bind_text(st, "?2", actor->principal);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

int db2_fact_actor_internal(fact_actor_rank_t rank, fact_actor_t *out)
{
   if (!out || rank == FACT_ACTOR_OPERATOR ||
       (rank != FACT_ACTOR_MODEL && rank != FACT_ACTOR_SYSTEM && rank != FACT_ACTOR_USER))
      return -1;
   memset(out, 0, sizeof(*out));
   out->rank = rank;
   snprintf(out->transport_identity, sizeof(out->transport_identity), "internal");
   if (rank == FACT_ACTOR_MODEL)
   {
      snprintf(out->principal, sizeof(out->principal), "system:model-inference");
      snprintf(out->role, sizeof(out->role), "model");
   }
   else if (rank == FACT_ACTOR_SYSTEM)
   {
      snprintf(out->principal, sizeof(out->principal), "system:kb-maintenance");
      snprintf(out->role, sizeof(out->role), "system");
   }
   else
   {
      /* This represents authenticated user evidence already captured by the KB
       * ingest boundary.  It is not an operator and cannot erase/review. */
      snprintf(out->principal, sizeof(out->principal), "source:authenticated-user");
      snprintf(out->role, sizeof(out->role), "user");
   }
   return 0;
}

int db2_fact_actor_from_request(int require_operator, fact_actor_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   const kb_principal_t *p = kb_reqctx_actor ? kb_reqctx_actor() : NULL;
   if (p && p->authenticated && kb_identity_key &&
       kb_identity_key(p, out->principal, sizeof(out->principal)) == 0)
   {
      out->authenticated = 1;
      out->rank = require_operator ? FACT_ACTOR_OPERATOR : FACT_ACTOR_USER;
      snprintf(out->role, sizeof(out->role), "%s", require_operator ? "operator" : "user");
      const kb_request_context_t *resolved = kb_reqctx_resolved ? kb_reqctx_resolved() : NULL;
      if (resolved && resolved->has_transport)
         (void)kb_identity_key(&resolved->transport, out->transport_identity,
                               sizeof(out->transport_identity));
      if (!out->transport_identity[0])
         snprintf(out->transport_identity, sizeof(out->transport_identity), "%s", out->principal);
      return 0;
   }

   /* A console-admin service credential is verifier-authenticated but is not a
    * tenancy actor.  Its verified scope, rather than request JSON, is its
    * operator identity. */
   const char *kind = NULL, *id = NULL;
   if (require_operator && kb_reqctx_verified_scope && kb_reqctx_verified_scope(&kind, &id) &&
       kind && strcmp(kind, "console-admin") == 0)
   {
      snprintf(out->principal, sizeof(out->principal), "scope:console-admin:%s", id ? id : "");
      snprintf(out->role, sizeof(out->role), "operator");
      snprintf(out->transport_identity, sizeof(out->transport_identity), "scope:console-admin:%s",
               id ? id : "");
      out->rank = FACT_ACTOR_OPERATOR;
      out->authenticated = 1;
      return 0;
   }
   return -1;
}

int db2_fact_actor_capture_memory(int64_t memory_id, int user_authority)
{
   if (memory_id <= 0)
      return -1;
   fact_actor_t actor;
   /* A note stored at MODEL authority is model-composed text, whoever was
    * authenticated when it was stored. Recording the request's identity here
    * would let an authenticated person's agent-composed note carry a USER actor,
    * and the drain reads THIS row rather than provenance_category -- so those
    * facts entered at Class A. See the header. */
   if (!user_authority)
   {
      if (db2_fact_actor_internal(FACT_ACTOR_MODEL, &actor) != 0)
         return -1;
   }
   else if (db2_fact_actor_from_request(0, &actor) != 0 &&
            db2_fact_actor_internal(FACT_ACTOR_MODEL, &actor) != 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO memory_fact_actors(memory_id,actor_principal,actor_role,authority_rank,"
       " authenticated,transport_identity) VALUES(?1,?2,?3,?4,?5,?6)"
       " ON CONFLICT(memory_id) DO NOTHING",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", actor.principal);
   aimee_pg_bind_text(st, "?3", actor.role);
   aimee_pg_bind_int(st, "?4", (int)actor.rank);
   aimee_pg_bind_int(st, "?5", actor.authenticated ? 1 : 0);
   aimee_pg_bind_text(st, "?6",
                      actor.transport_identity[0] ? actor.transport_identity : "internal");
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

int db2_fact_actor_for_memory(int64_t memory_id, fact_actor_t *out)
{
   if (memory_id <= 0 || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT actor_principal,actor_role,authority_rank,authenticated,"
                        " transport_identity"
                        " FROM memory_fact_actors WHERE memory_id=?1 LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      snprintf(out->principal, sizeof(out->principal), "%s", aimee_pg_column_text(st, 0));
      snprintf(out->role, sizeof(out->role), "%s", aimee_pg_column_text(st, 1));
      out->rank = (fact_actor_rank_t)aimee_pg_column_int(st, 2);
      out->authenticated = aimee_pg_column_int(st, 3);
      snprintf(out->transport_identity, sizeof(out->transport_identity), "%s",
               aimee_pg_column_text(st, 4));
      found = fm_actor_ok(out);
   }
   aimee_pg_finalize(st);
   return found ? 0 : -1;
}

static int fm_kind_ok(const char *kind)
{
   return !kind || !kind[0] || strcmp(kind, FACT_KIND_WORLD_FACT) == 0 ||
          strcmp(kind, FACT_KIND_EPISODE) == 0 || strcmp(kind, FACT_KIND_EXPERIENCE) == 0 ||
          strcmp(kind, FACT_KIND_MENTAL_MODEL) == 0 || strcmp(kind, FACT_KIND_PREFERENCE) == 0 ||
          strcmp(kind, FACT_KIND_INSTRUCTION) == 0 || strcmp(kind, FACT_KIND_POLICY) == 0 ||
          strcmp(kind, FACT_KIND_HYPOTHESIS) == 0;
}

static void fm_copy(char *out, size_t cap, const char *s)
{
   snprintf(out, cap, "%s", s ? s : "");
}

static void fm_state_row(aimee_pg_stmt_t *st, fm_state_t *s)
{
   memset(s, 0, sizeof(*s));
   s->found = 1;
   s->id = aimee_pg_column_int64(st, 0);
   fm_copy(s->lifecycle, sizeof(s->lifecycle), aimee_pg_column_text(st, 1));
   fm_copy(s->superseded_at, sizeof(s->superseded_at), aimee_pg_column_text(st, 2));
   fm_copy(s->invalidated_at, sizeof(s->invalidated_at), aimee_pg_column_text(st, 3));
   s->suppressed = aimee_pg_column_int(st, 4);
   s->confidence = aimee_pg_column_double(st, 5);
   s->authority_rank = aimee_pg_column_int(st, 6);
   s->version = aimee_pg_column_int(st, 7);
}

typedef struct
{
   int64_t id;
   char *source;
   char *relation;
   char *target;
} fm_identity_backfill_row_t;

static int fm_backfill_identity(void *conn)
{
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       conn,
       "SELECT id,source,relation,target FROM entity_edges WHERE edge_class='semantic'"
       " AND (identity_key='' OR identity_subject_key='') ORDER BY id ASC",
       err, sizeof(err));
   if (!q)
      return -1;
   fm_identity_backfill_row_t *rows = NULL;
   size_t count = 0, cap = 0;
   aimee_pg_step_t step;
   while ((step = aimee_pg_step(q, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      if (count == cap)
      {
         size_t next = cap ? cap * 2 : 64;
         fm_identity_backfill_row_t *grown = realloc(rows, next * sizeof(*rows));
         if (!grown)
         {
            step = AIMEE_PG_ERR;
            break;
         }
         rows = grown;
         cap = next;
      }
      rows[count].id = aimee_pg_column_int64(q, 0);
      rows[count].source = strdup(aimee_pg_column_text(q, 1));
      rows[count].relation = strdup(aimee_pg_column_text(q, 2));
      rows[count].target = strdup(aimee_pg_column_text(q, 3));
      if (!rows[count].source || !rows[count].relation || !rows[count].target)
      {
         step = AIMEE_PG_ERR;
         count++;
         break;
      }
      count++;
   }
   aimee_pg_finalize(q);
   int ok = step != AIMEE_PG_ERR;
   for (size_t i = 0; ok && i < count; i++)
   {
      char identity[FACT_IDENTITY_KEY_MAX], subject[FACT_IDENTITY_KEY_MAX];
      if (fact_identity_key(rows[i].source, rows[i].relation, rows[i].target, identity,
                            sizeof(identity)) == 0 ||
          fact_identity_subject_key(rows[i].source, rows[i].relation, subject, sizeof(subject)) ==
              0)
         continue; /* invalid legacy text stays on the literal compatibility arm */
      aimee_pg_stmt_t *u = aimee_pg_prepare(
          conn,
          "UPDATE entity_edges SET identity_key=?1,identity_subject_key=?2 WHERE id=?3"
          " AND (identity_key='' OR identity_subject_key='')",
          err, sizeof(err));
      if (!u)
      {
         ok = 0;
         break;
      }
      aimee_pg_bind_text(u, "?1", identity);
      aimee_pg_bind_text(u, "?2", subject);
      aimee_pg_bind_int64(u, "?3", rows[i].id);
      ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(u);
   }
   for (size_t i = 0; i < count; i++)
   {
      free(rows[i].source);
      free(rows[i].relation);
      free(rows[i].target);
   }
   free(rows);
   return ok ? 0 : -1;
}

/* Match on the normalized identity when one can be computed, and on the literal
 * triple otherwise.
 *
 * The literal arm is not a fallback for convenience -- it is what keeps a
 * partially backfilled store correct. Rows written before identity_key existed
 * carry '', and dropping the literal comparison would make every one of them
 * invisible to this lookup, silently un-rejecting every rejection recorded
 * before the migration. Both arms are ORed for exactly as long as that is true.
 *
 * Deliberately does NOT filter by lifecycle: a re-assertion has to *find* the
 * dead row so the caller can apply the authority rules to it. A lookup that
 * skipped invalidated rows would report "no such fact" and insert a clean new
 * one, which is the tombstone bypass this whole path exists to prevent. */
static int fm_load_exact(void *conn, const char *source, const char *relation, const char *target,
                         fm_state_t *out)
{
   memset(out, 0, sizeof(*out));
   char ikey[FACT_IDENTITY_KEY_MAX];
   size_t ikey_len = fact_identity_key(source, relation, target, ikey, sizeof(ikey));

   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       ikey_len > 0
           ? "SELECT id,lifecycle_state,superseded_at,invalidated_at,suppressed,confidence,"
             " authority_rank,version FROM entity_edges WHERE edge_class='semantic'"
             " AND ((identity_key<>'' AND identity_key=?4)"
             "   OR (identity_key='' AND source=?1 AND relation=?2 AND target=?3))"
             " ORDER BY id DESC LIMIT 1"
           : "SELECT id,lifecycle_state,superseded_at,invalidated_at,suppressed,confidence,"
             " authority_rank,version FROM entity_edges WHERE source=?1 AND relation=?2"
             " AND target=?3 AND edge_class='semantic' ORDER BY id DESC LIMIT 1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", source);
   aimee_pg_bind_text(st, "?2", relation);
   aimee_pg_bind_text(st, "?3", target);
   if (ikey_len > 0)
      aimee_pg_bind_text(st, "?4", ikey);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ROW)
      fm_state_row(st, out);
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ERR ? -1 : 0;
}

static int fm_commit_open(void *conn, const fact_actor_t *actor, const char *operation,
                          int reversible, char id[FACT_COMMIT_ID_MAX])
{
   fm_commit_id(id);
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *ctx = aimee_pg_prepare(
       conn,
       "SELECT set_config('aimee.principal',?1,true),set_config('aimee.authority',?2,true),"
       " set_config('aimee.transport_identity',?3,true)",
       err, sizeof(err));
   if (!ctx)
      return -1;
   aimee_pg_bind_text(ctx, "?1", actor->principal);
   aimee_pg_bind_text(ctx, "?2", actor->role);
   aimee_pg_bind_text(ctx, "?3",
                      actor->transport_identity[0] ? actor->transport_identity : "internal");
   int ctx_ok = aimee_pg_step(ctx, err, sizeof(err)) == AIMEE_PG_ROW;
   aimee_pg_finalize(ctx);
   if (!ctx_ok)
      return -1;
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,"
       " authority_rank,status,reversible) VALUES(?1,?2,?3,?4,?5,'open',?6)",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_bind_text(st, "?2", operation);
   aimee_pg_bind_text(st, "?3", actor->principal);
   aimee_pg_bind_text(st, "?4", actor->role);
   aimee_pg_bind_int(st, "?5", (int)actor->rank);
   aimee_pg_bind_int(st, "?6", reversible ? 1 : 0);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

static int fm_commit_parent(void *conn, const char *commit_id, const char *parent_commit_id)
{
   if (!parent_commit_id || !parent_commit_id[0])
      return 0;
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "UPDATE fact_graph_commits SET parent_commit_id=?2,origin_ref=?2"
                        " WHERE commit_id=?1 AND status='open'",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", commit_id);
   aimee_pg_bind_text(st, "?2", parent_commit_id);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE && aimee_pg_stmt_changes(st) == 1;
   aimee_pg_finalize(st);
   if (ok)
   {
      st = aimee_pg_prepare(conn, "SELECT set_config('aimee.correlation_id',?1,true)", err,
                            sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", parent_commit_id);
      ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW;
      aimee_pg_finalize(st);
   }
   return ok ? 0 : -1;
}

static int fm_commit_finish(void *conn, const fact_actor_t *actor, const char *commit_id,
                            const char *operation, const char *subject, const char *status)
{
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE fact_graph_commits SET status=?2,closed_at=to_char(CURRENT_TIMESTAMP,"
       " 'YYYY-MM-DD HH24:MI:SS') WHERE commit_id=?1 AND status='open'",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", commit_id);
   aimee_pg_bind_text(st, "?2", status);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE && aimee_pg_stmt_changes(st) == 1;
   aimee_pg_finalize(st);
   if (!ok)
      return -1;
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
   /* PostgreSQL owns the security boundary: the narrow definer reads the
    * canonical actor/operation/status from this changeset and writes only a
    * durable outbox intent. The separately credentialed WORM process constructs
    * the chain later; this runtime connection never receives chain INSERT. */
   st = aimee_pg_prepare(conn, "SELECT kb_fact_commit_worm_seal(?1,?2)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", commit_id);
   aimee_pg_bind_text(st, "?2", subject ? subject : "");
   ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW;
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
#else
   /* The SQLite test shim has no stored procedures or worker process. Retain
    * its local append solely as a deterministic unit-test implementation. */
   char detail[160];
   snprintf(detail, sizeof(detail), "commit_id=%s", commit_id);
   return db2_kb_audit_append_in_txn(conn, actor->role, actor->principal, operation,
                                     subject ? subject : "", "allow", detail);
#endif
}

static int fm_change(void *conn, const char *commit_id, int64_t assertion_id, const char *action,
                     const fm_state_t *before, const fm_state_t *after, const char *detail)
{
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO fact_graph_changes(commit_id,assertion_id,object_kind,object_key,action,"
       " existed_before,existed_after,before_lifecycle,after_lifecycle,before_superseded_at,"
       " after_superseded_at,before_invalidated_at,after_invalidated_at,before_suppressed,"
       " after_suppressed,before_confidence,after_confidence,before_authority_rank,"
       " after_authority_rank,before_version,after_version,diff_detail)"
       " VALUES(?1,?2,'assertion',?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,"
       " ?17,?18,?19,?20,?21)",
       err, sizeof(err));
   if (!st)
      return -1;
   char key[32];
   snprintf(key, sizeof(key), "%lld", (long long)assertion_id);
   aimee_pg_bind_text(st, "?1", commit_id);
   aimee_pg_bind_int64(st, "?2", assertion_id);
   aimee_pg_bind_text(st, "?3", key);
   aimee_pg_bind_text(st, "?4", action);
   aimee_pg_bind_int(st, "?5", before && before->found ? 1 : 0);
   aimee_pg_bind_int(st, "?6", after && after->found ? 1 : 0);
   aimee_pg_bind_text(st, "?7", before ? before->lifecycle : "");
   aimee_pg_bind_text(st, "?8", after ? after->lifecycle : "");
   aimee_pg_bind_text(st, "?9", before ? before->superseded_at : "");
   aimee_pg_bind_text(st, "?10", after ? after->superseded_at : "");
   aimee_pg_bind_text(st, "?11", before ? before->invalidated_at : "");
   aimee_pg_bind_text(st, "?12", after ? after->invalidated_at : "");
   aimee_pg_bind_int(st, "?13", before ? before->suppressed : 0);
   aimee_pg_bind_int(st, "?14", after ? after->suppressed : 0);
   aimee_pg_bind_double(st, "?15", before ? before->confidence : 0.0);
   aimee_pg_bind_double(st, "?16", after ? after->confidence : 0.0);
   aimee_pg_bind_int(st, "?17", before ? before->authority_rank : 0);
   aimee_pg_bind_int(st, "?18", after ? after->authority_rank : 0);
   aimee_pg_bind_int(st, "?19", before ? before->version : 0);
   aimee_pg_bind_int(st, "?20", after ? after->version : 0);
   aimee_pg_bind_text(st, "?21", detail ? detail : "");
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

static int fm_external_change(void *conn, const char *commit_id, const char *object_kind,
                              const char *object_key, const char *action, const char *before_state,
                              const char *after_state, const char *detail)
{
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO fact_graph_changes(commit_id,assertion_id,object_kind,object_key,action,"
       " existed_before,existed_after,before_lifecycle,after_lifecycle,diff_detail)"
       " VALUES(?1,0,?2,?3,?4,1,1,?5,?6,?7)",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", commit_id);
   aimee_pg_bind_text(st, "?2", object_kind);
   aimee_pg_bind_text(st, "?3", object_key);
   aimee_pg_bind_text(st, "?4", action);
   aimee_pg_bind_text(st, "?5", before_state ? before_state : "");
   aimee_pg_bind_text(st, "?6", after_state ? after_state : "");
   aimee_pg_bind_text(st, "?7", detail ? detail : "external graph transition");
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

static int fm_evidence_add(void *conn, int64_t assertion_id, const fact_actor_t *actor,
                           const fact_evidence_input_t *e, const char *commit_id)
{
   const char *source_kind =
       e && e->source_kind && e->source_kind[0] ? e->source_kind : "observation";
   const char *source_id = e && e->source_id && e->source_id[0] ? e->source_id : commit_id;
   const char *stance =
       e && e->stance && strcmp(e->stance, "contradicts") == 0 ? "contradicts" : "supports";
   char now[32];
   fm_now(now);
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO fact_evidence(assertion_id,source_kind,source_id,source_span,evidence_hash,"
       " actor_principal,observed_at,ingest_run_id,commit_id,stance)"
       " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) ON CONFLICT DO NOTHING",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", assertion_id);
   aimee_pg_bind_text(st, "?2", source_kind);
   aimee_pg_bind_text(st, "?3", source_id);
   aimee_pg_bind_text(st, "?4", e && e->source_span ? e->source_span : "");
   aimee_pg_bind_text(st, "?5", e && e->evidence_hash ? e->evidence_hash : "");
   aimee_pg_bind_text(st, "?6",
                      e && e->actor_principal && e->actor_principal[0] ? e->actor_principal
                                                                       : actor->principal);
   aimee_pg_bind_text(st, "?7", e && e->observed_at && e->observed_at[0] ? e->observed_at : now);
   aimee_pg_bind_text(st, "?8", e && e->ingest_run_id ? e->ingest_run_id : "");
   aimee_pg_bind_text(st, "?9", commit_id);
   aimee_pg_bind_text(st, "?10", stance);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   int changed = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return ok ? changed : -1;
}

/* A source mention is an immutable event, not a command that may be replayed to
 * change lifecycle state.  In particular, retrying an ingest job after an
 * operator rolled its batch back must not reactivate the invalidated assertion.
 * Match the fact_evidence uniqueness key before opening a graph commit so exact
 * delivery retries are true no-ops (no version bump and no empty audit commit).
 */
static int fm_evidence_exists(void *conn, int64_t assertion_id, const fact_evidence_input_t *e)
{
   if (!e || !e->source_id || !e->source_id[0])
      return 0;
   const char *source_kind = e->source_kind && e->source_kind[0] ? e->source_kind : "observation";
   const char *stance =
       e->stance && strcmp(e->stance, "contradicts") == 0 ? "contradicts" : "supports";
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT 1 FROM fact_evidence WHERE assertion_id=?1 AND source_kind=?2 AND source_id=?3"
       " AND source_span=?4 AND evidence_hash=?5 AND stance=?6 LIMIT 1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", assertion_id);
   aimee_pg_bind_text(st, "?2", source_kind);
   aimee_pg_bind_text(st, "?3", e->source_id);
   aimee_pg_bind_text(st, "?4", e->source_span ? e->source_span : "");
   aimee_pg_bind_text(st, "?5", e->evidence_hash ? e->evidence_hash : "");
   aimee_pg_bind_text(st, "?6", stance);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW ? 1 : step == AIMEE_PG_DONE ? 0 : -1;
}

static int fm_begin(void *conn)
{
   if (!conn || aimee_pg_in_transaction(conn))
      return -1;
   char err[FM_ERRBUF] = "";
   return aimee_pg_exec(conn, "BEGIN", err, sizeof(err));
}

static int fm_end(void *conn, int ok)
{
   char err[FM_ERRBUF] = "";
   if (!ok)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return 0;
}

int db2_fact_graph_record_external_in_txn(const fact_actor_t *actor, const char *operation,
                                          const char *object_kind, const char *object_key,
                                          const char *action, const char *before_state,
                                          const char *after_state, int reversible,
                                          char commit_id[FACT_COMMIT_ID_MAX])
{
   if (commit_id)
      commit_id[0] = '\0';
   void *conn = db2_conn();
   if (!fm_actor_ok(actor) || !operation || !operation[0] || !object_kind || !object_kind[0] ||
       !object_key || !object_key[0] || !action || !action[0] || !conn ||
       !aimee_pg_in_transaction(conn))
      return -1;
   char cid[FACT_COMMIT_ID_MAX];
   if (fm_commit_open(conn, actor, operation, reversible, cid) != 0)
      return -1;
   if (fm_external_change(conn, cid, object_kind, object_key, action, before_state, after_state,
                          "external graph transition") != 0 ||
       fm_commit_finish(conn, actor, cid, operation, object_key, "applied") != 0)
      return -1;
   if (commit_id)
      fm_copy(commit_id, FACT_COMMIT_ID_MAX, cid);
   return 0;
}

int db2_fact_mutation_assert(const fact_actor_t *actor, const fact_assertion_input_t *in,
                             fact_mutation_result_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!fm_actor_ok(actor) || !in || !in->source || !in->source[0] || !in->relation ||
       !in->relation[0] || !in->target || !in->target[0] || !fm_kind_ok(in->assertion_kind))
      return -1;
   void *conn = db2_conn();
   if (fm_begin(conn) != 0)
      return -1;
   if (fm_backfill_identity(conn) != 0)
      return fm_end(conn, 0);

   int tombstoned = fm_tombstone_blocks(conn, in->source, in->relation, in->target);
   if (tombstoned != 0)
   {
      (void)fm_end(conn, 0);
      return tombstoned > 0 ? FACT_MUTATION_TOMBSTONED : -1;
   }

   char now[32];
   fm_now(now);
   const char *desired =
       actor->rank >= FACT_ACTOR_SYSTEM ? FACT_LIFECYCLE_PERSISTENT : FACT_LIFECYCLE_CANDIDATE;
   const char *akind =
       in->assertion_kind && in->assertion_kind[0] ? in->assertion_kind : FACT_KIND_WORLD_FACT;
   const char *cls = in->confidence_class && in->confidence_class[0] ? in->confidence_class : "C";
   double conf = in->confidence;
   if (conf < 0.0)
      conf = 0.0;
   if (conf > 1.0)
      conf = 1.0;

   fm_state_t exact;
   if (fm_load_exact(conn, in->source, in->relation, in->target, &exact) != 0)
      return fm_end(conn, 0);

   if (exact.found)
   {
      int replay = fm_evidence_exists(conn, exact.id, in->evidence);
      if (replay < 0)
         return fm_end(conn, 0);
      if (replay)
      {
         if (fm_end(conn, 1) != 0)
            return -1;
         if (out)
         {
            out->assertion_id = exact.id;
            fm_copy(out->lifecycle, sizeof(out->lifecycle), exact.lifecycle);
         }
         return 0;
      }
   }

   char commit_id[FACT_COMMIT_ID_MAX];
   if (fm_commit_open(conn, actor, "fact.assert", 1, commit_id) != 0)
      return fm_end(conn, 0);
   if (in->evidence && in->evidence->ingest_run_id && in->evidence->ingest_run_id[0] &&
       fm_commit_parent(conn, commit_id, in->evidence->ingest_run_id) != 0)
      return fm_end(conn, 0);

   int64_t assertion_id = exact.id;
   int changed = 0, quarantined = 0;
   fm_state_t after;
   memset(&after, 0, sizeof(after));

   if (exact.found)
   {
      const char *next_lifecycle = exact.lifecycle;
      int reactivate = (strcmp(exact.lifecycle, FACT_LIFECYCLE_INVALIDATED) == 0 ||
                        strcmp(exact.lifecycle, FACT_LIFECYCLE_SUPERSEDED) == 0) &&
                       (int)actor->rank >= exact.authority_rank;
      int promote_candidate = strcmp(exact.lifecycle, FACT_LIFECYCLE_CANDIDATE) == 0 &&
                              actor->rank >= FACT_ACTOR_SYSTEM &&
                              (int)actor->rank >= exact.authority_rank;
      if (reactivate || promote_candidate)
         next_lifecycle = desired;

      /* Re-activating an exact historical/candidate value is still a functional
       * correction. Supersede every incumbent atomically, or keep this value in
       * quarantine when any incumbent outranks the actor. */
      if ((reactivate || promote_candidate) &&
          (in->functional || rel_type_is_functional(in->relation)))
      {
         fm_state_t priors[FM_STATE_MAX + 1];
         int np = 0;
         char err[FM_ERRBUF] = "";
         aimee_pg_stmt_t *q = aimee_pg_prepare(
             conn,
             "SELECT id,lifecycle_state,superseded_at,invalidated_at,suppressed,confidence,"
             " authority_rank,version FROM entity_edges WHERE edge_class='semantic'"
             " AND ((identity_subject_key<>'' AND identity_subject_key=?4)"
             "   OR (identity_subject_key='' AND source=?1 AND relation=?2))"
             " AND id<>?3 AND superseded_at='' AND invalidated_at=''"
             " AND suppressed=0 AND lifecycle_state IN ('persistent','promoted')"
             " ORDER BY authority_rank DESC,id DESC",
             err, sizeof(err));
         if (!q)
            return fm_end(conn, 0);
         aimee_pg_bind_text(q, "?1", in->source);
         aimee_pg_bind_text(q, "?2", in->relation);
         aimee_pg_bind_int64(q, "?3", exact.id);
         {
            char sk[FACT_IDENTITY_KEY_MAX];
            if (fact_identity_subject_key(in->source, in->relation, sk, sizeof(sk)) == 0)
               sk[0] = '\0';
            aimee_pg_bind_text(q, "?4", sk);
         }
         while (np <= FM_STATE_MAX && aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
            fm_state_row(q, &priors[np++]);
         aimee_pg_finalize(q);
         if (np > FM_STATE_MAX)
            return fm_end(conn, 0); /* bounded safety walk fails closed, never partially corrects */
         const rel_type_def_t *def = rel_types_seed_lookup(in->relation);
         correction_behavior_t behavior = def ? def->correction_behavior : CORR_SUPERSEDE;
         for (int i = 0; i < np; i++)
            if ((int)actor->rank < priors[i].authority_rank ||
                (behavior == CORR_IMMUTABLE && actor->rank != FACT_ACTOR_OPERATOR))
               quarantined = 1;
         if (quarantined)
         {
            reactivate = 0;
            promote_candidate = 0;
            next_lifecycle = FACT_LIFECYCLE_CANDIDATE;
         }
         for (int i = 0; !quarantined && i < np; i++)
         {
            fm_state_t prior_after = priors[i];
            fm_copy(prior_after.lifecycle, sizeof(prior_after.lifecycle),
                    FACT_LIFECYCLE_SUPERSEDED);
            fm_copy(prior_after.superseded_at, sizeof(prior_after.superseded_at), now);
            prior_after.version++;
            aimee_pg_stmt_t *u = aimee_pg_prepare(
                conn,
                "UPDATE entity_edges SET lifecycle_state='superseded',superseded_at=?2,"
                " version=version+1,commit_id=?3 WHERE id=?1",
                err, sizeof(err));
            if (!u)
               return fm_end(conn, 0);
            aimee_pg_bind_int64(u, "?1", priors[i].id);
            aimee_pg_bind_text(u, "?2", now);
            aimee_pg_bind_text(u, "?3", commit_id);
            int ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE;
            aimee_pg_finalize(u);
            if (!ok || fm_change(conn, commit_id, priors[i].id, "supersede", &priors[i],
                                 &prior_after, "functional relation correction") != 0)
               return fm_end(conn, 0);
         }
      }
      int next_rank = exact.authority_rank;
      if ((int)actor->rank > next_rank)
         next_rank = (int)actor->rank;
      double next_conf = conf > exact.confidence ? conf : exact.confidence;
      changed = reactivate || strcmp(next_lifecycle, exact.lifecycle) != 0 ||
                next_rank != exact.authority_rank || next_conf != exact.confidence;

      char err[FM_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn,
          "UPDATE entity_edges SET lifecycle_state=?2,superseded_at=CASE WHEN ?3=1 THEN '' ELSE"
          " superseded_at END,invalidated_at=CASE WHEN ?4=1 THEN '' ELSE invalidated_at END,"
          " suppressed=CASE WHEN ?5=1 THEN 0 ELSE suppressed END,confidence=?6,"
          " confidence_class=CASE WHEN ?7>authority_rank THEN ?8 ELSE confidence_class END,"
          " authority_rank=?9,actor_principal=CASE WHEN ?10>authority_rank THEN ?11 ELSE"
          " actor_principal END,version=version+?12,commit_id=?13 WHERE id=?1",
          err, sizeof(err));
      if (!st)
         return fm_end(conn, 0);
      aimee_pg_bind_int64(st, "?1", assertion_id);
      aimee_pg_bind_text(st, "?2", next_lifecycle);
      aimee_pg_bind_int(st, "?3", reactivate);
      aimee_pg_bind_int(st, "?4", reactivate);
      aimee_pg_bind_int(st, "?5", reactivate);
      aimee_pg_bind_double(st, "?6", next_conf);
      aimee_pg_bind_int(st, "?7", (int)actor->rank);
      aimee_pg_bind_text(st, "?8", cls);
      aimee_pg_bind_int(st, "?9", next_rank);
      aimee_pg_bind_int(st, "?10", (int)actor->rank);
      aimee_pg_bind_text(st, "?11", actor->principal);
      aimee_pg_bind_int(st, "?12", changed ? 1 : 0);
      aimee_pg_bind_text(st, "?13", commit_id);
      int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(st);
      if (!ok || fm_load_exact(conn, in->source, in->relation, in->target, &after) != 0 ||
          !after.found)
         return fm_end(conn, 0);
      if (fm_change(conn, commit_id, assertion_id, "corroborate", &exact, &after,
                    "independent evidence mention") != 0)
         return fm_end(conn, 0);
   }
   else
   {
      int64_t prior_version_id = 0;
      if (in->functional || rel_type_is_functional(in->relation))
      {
         fm_state_t priors[FM_STATE_MAX + 1];
         int np = 0;
         char err[FM_ERRBUF] = "";
         aimee_pg_stmt_t *q = aimee_pg_prepare(
             conn,
             /* Incumbents are matched on the normalized (subject, predicate)
              * key, with the incoming value excluded by its normalized identity
              * rather than by literal target text. Keyed literally, a rephrased
              * incumbent was invisible here and survived the correction, so a
              * functional relation ended up with two current objects that were
              * the same fact spelled two ways. The literal arm still applies to
              * rows written before the backfill. */
             "SELECT id,lifecycle_state,superseded_at,invalidated_at,suppressed,confidence,"
             " authority_rank,version FROM entity_edges WHERE edge_class='semantic'"
             " AND ((identity_subject_key<>'' AND identity_subject_key=?4)"
             "   OR (identity_subject_key='' AND source=?1 AND relation=?2))"
             " AND NOT ((identity_key<>'' AND identity_key=?5)"
             "       OR (identity_key='' AND target=?3))"
             " AND superseded_at=''"
             " AND invalidated_at='' AND suppressed=0 AND lifecycle_state IN"
             " ('candidate','persistent','promoted') ORDER BY authority_rank DESC,id DESC",
             err, sizeof(err));
         if (!q)
            return fm_end(conn, 0);
         {
            char sk[FACT_IDENTITY_KEY_MAX];
            char ik[FACT_IDENTITY_KEY_MAX];
            if (fact_identity_subject_key(in->source, in->relation, sk, sizeof(sk)) == 0)
               sk[0] = '\0';
            if (fact_identity_key(in->source, in->relation, in->target, ik, sizeof(ik)) == 0)
               ik[0] = '\0';
            aimee_pg_bind_text(q, "?4", sk);
            aimee_pg_bind_text(q, "?5", ik);
         }
         aimee_pg_bind_text(q, "?1", in->source);
         aimee_pg_bind_text(q, "?2", in->relation);
         aimee_pg_bind_text(q, "?3", in->target);
         while (np <= FM_STATE_MAX && aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
            fm_state_row(q, &priors[np++]);
         aimee_pg_finalize(q);
         if (np > FM_STATE_MAX)
            return fm_end(conn, 0); /* no partial supersession beyond the audit buffer */

         const rel_type_def_t *def = rel_types_seed_lookup(in->relation);
         correction_behavior_t behavior = def ? def->correction_behavior : CORR_SUPERSEDE;
         int replace_all = 1;
         for (int i = 0; i < np; i++)
         {
            int may_replace = (int)actor->rank >= priors[i].authority_rank &&
                              (behavior != CORR_IMMUTABLE || actor->rank == FACT_ACTOR_OPERATOR);
            if (!may_replace)
            {
               quarantined = 1;
               replace_all = 0;
               break;
            }
         }
         for (int i = 0; replace_all && i < np; i++)
         {
            fm_state_t prior_after = priors[i];
            fm_copy(prior_after.lifecycle, sizeof(prior_after.lifecycle),
                    FACT_LIFECYCLE_SUPERSEDED);
            fm_copy(prior_after.superseded_at, sizeof(prior_after.superseded_at), now);
            prior_after.version++;
            aimee_pg_stmt_t *u = aimee_pg_prepare(
                conn,
                "UPDATE entity_edges SET lifecycle_state='superseded',superseded_at=?2,"
                " version=version+1,commit_id=?3 WHERE id=?1",
                err, sizeof(err));
            if (!u)
               return fm_end(conn, 0);
            aimee_pg_bind_int64(u, "?1", priors[i].id);
            aimee_pg_bind_text(u, "?2", now);
            aimee_pg_bind_text(u, "?3", commit_id);
            int ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE;
            aimee_pg_finalize(u);
            if (!ok || fm_change(conn, commit_id, priors[i].id, "supersede", &priors[i],
                                 &prior_after, "functional relation correction") != 0)
               return fm_end(conn, 0);
            if (!prior_version_id)
               prior_version_id = priors[i].id;
         }
      }
      if (quarantined)
         desired = FACT_LIFECYCLE_CANDIDATE;

      char err[FM_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn,
          "INSERT INTO entity_edges(source,relation,target,weight,relation_id,subject_kind,"
          " object_kind,edge_class,confidence_class,confidence,asserted_at,valid_from,valid_until,"
          " assertion_kind,epistemic_kind,lifecycle_state,authority_rank,actor_principal,version,"
          " prior_version_id,commit_id,identity_key,identity_subject_key)"
          " VALUES(?1,?2,?3,1,?4,?5,?6,'semantic',?7,?8,"
          " ?9,?10,?11,?12,?13,?14,?15,?16,1,?17,?18,?19,?20) RETURNING id",
          err, sizeof(err));
      if (!st)
         return fm_end(conn, 0);
      aimee_pg_bind_text(st, "?1", in->source);
      aimee_pg_bind_text(st, "?2", in->relation);
      aimee_pg_bind_text(st, "?3", in->target);
      /* Identity is written at insert so every new row is keyed the way the
       * lookup above searches. An empty key (component missing after
       * normalization) leaves the row on the literal arm rather than colliding
       * every such row into one shared '' key. */
      {
         char ikey[FACT_IDENTITY_KEY_MAX];
         char skey[FACT_IDENTITY_KEY_MAX];
         if (fact_identity_key(in->source, in->relation, in->target, ikey, sizeof(ikey)) == 0)
            ikey[0] = '\0';
         if (fact_identity_subject_key(in->source, in->relation, skey, sizeof(skey)) == 0)
            skey[0] = '\0';
         aimee_pg_bind_text(st, "?19", ikey);
         aimee_pg_bind_text(st, "?20", skey);
      }
      aimee_pg_bind_int(st, "?4", in->relation_id);
      aimee_pg_bind_int(st, "?5", in->subject_kind);
      aimee_pg_bind_int(st, "?6", in->object_kind);
      aimee_pg_bind_text(st, "?7", cls);
      aimee_pg_bind_double(st, "?8", conf);
      aimee_pg_bind_text(st, "?9", now);
      aimee_pg_bind_text(st, "?10", in->valid_from ? in->valid_from : "");
      aimee_pg_bind_text(st, "?11", in->valid_until ? in->valid_until : "");
      aimee_pg_bind_text(st, "?12", akind);
      aimee_pg_bind_text(st, "?13", akind);
      aimee_pg_bind_text(st, "?14", desired);
      aimee_pg_bind_int(st, "?15", (int)actor->rank);
      aimee_pg_bind_text(st, "?16", actor->principal);
      aimee_pg_bind_int64(st, "?17", prior_version_id);
      aimee_pg_bind_text(st, "?18", commit_id);
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         assertion_id = aimee_pg_column_int64(st, 0);
      aimee_pg_finalize(st);
      if (assertion_id <= 0 ||
          fm_load_exact(conn, in->source, in->relation, in->target, &after) != 0 || !after.found)
         return fm_end(conn, 0);
      changed = 1;
      if (fm_change(conn, commit_id, assertion_id, "insert", NULL, &after,
                    quarantined ? "quarantined below incumbent authority" : "new assertion") != 0)
         return fm_end(conn, 0);
   }

   int evidence_added = fm_evidence_add(conn, assertion_id, actor, in->evidence, commit_id);
   if (evidence_added < 0)
      return fm_end(conn, 0);
   char subject[32];
   snprintf(subject, sizeof(subject), "%lld", (long long)assertion_id);
   if (fm_commit_finish(conn, actor, commit_id, "fact.assert", subject, "applied") != 0 ||
       fm_end(conn, 1) != 0)
      return -1;
   if (out)
   {
      out->assertion_id = assertion_id;
      fm_copy(out->commit_id, sizeof(out->commit_id), commit_id);
      fm_copy(out->lifecycle, sizeof(out->lifecycle), after.lifecycle);
      out->changed = changed;
      out->quarantined = quarantined;
      out->evidence_added = evidence_added > 0;
   }
   return 0;
}

static int fm_selector_sql(char *sql, size_t cap, const char *prefix, const char *suffix,
                           const char *relation, const char *target)
{
   int n = snprintf(
       sql, cap, "%s WHERE source=?1 AND edge_class='semantic'%s%s %s", prefix,
       relation && relation[0] ? " AND relation=?2" : "",
       target && target[0] ? (relation && relation[0] ? " AND target=?3" : " AND target=?2") : "",
       suffix ? suffix : "");
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

static void fm_bind_selector(aimee_pg_stmt_t *st, const char *source, const char *relation,
                             const char *target)
{
   aimee_pg_bind_text(st, "?1", source);
   if (relation && relation[0])
      aimee_pg_bind_text(st, "?2", relation);
   if (target && target[0])
      aimee_pg_bind_text(st, relation && relation[0] ? "?3" : "?2", target);
}

int db2_fact_mutation_invalidate(const fact_actor_t *actor, const char *source,
                                 const char *relation, const char *target,
                                 fact_mutation_result_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!fm_actor_ok(actor) || !source || !source[0] || !relation || !relation[0])
      return -1;
   void *conn = db2_conn();
   if (fm_begin(conn) != 0)
      return -1;

   /* P6 routes by epistemic kind before relation correction behavior.  An
    * episode/experience is historical evidence and may only be annotated;
    * policy changes require governance authority. */
   char kind_sql[768], kind_err[FM_ERRBUF] = "";
   if (fm_selector_sql(kind_sql, sizeof(kind_sql), "SELECT epistemic_kind FROM entity_edges",
                       "AND superseded_at='' AND invalidated_at='' AND suppressed=0", relation,
                       target) != 0)
      return fm_end(conn, 0);
   aimee_pg_stmt_t *kind_q = aimee_pg_prepare(conn, kind_sql, kind_err, sizeof(kind_err));
   if (!kind_q)
      return fm_end(conn, 0);
   fm_bind_selector(kind_q, source, relation, target);
   int annotate_only = 0, operator_only = 0;
   while (aimee_pg_step(kind_q, kind_err, sizeof(kind_err)) == AIMEE_PG_ROW)
   {
      const char *kind = aimee_pg_column_text(kind_q, 0);
      if (strcmp(kind, FACT_KIND_EPISODE) == 0 || strcmp(kind, FACT_KIND_EXPERIENCE) == 0)
         annotate_only = 1;
      if (strcmp(kind, FACT_KIND_POLICY) == 0)
         operator_only = 1;
   }
   aimee_pg_finalize(kind_q);
   if (annotate_only || (operator_only && actor->rank != FACT_ACTOR_OPERATOR))
   {
      (void)fm_end(conn, 0);
      return annotate_only ? -2 : -1;
   }
   char commit_id[FACT_COMMIT_ID_MAX];
   if (fm_commit_open(conn, actor, "fact.invalidate", 1, commit_id) != 0)
      return fm_end(conn, 0);

   char sql[768], err[FM_ERRBUF] = "";
   if (fm_selector_sql(sql, sizeof(sql),
                       "SELECT id,lifecycle_state,superseded_at,invalidated_at,suppressed,"
                       "confidence,authority_rank,version FROM entity_edges",
                       "AND superseded_at='' AND invalidated_at='' AND suppressed=0", relation,
                       target) != 0)
      return fm_end(conn, 0);
   aimee_pg_stmt_t *q = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!q)
      return fm_end(conn, 0);
   fm_bind_selector(q, source, relation, target);
   fm_state_t rows[FM_STATE_MAX];
   int nr = 0;
   while (nr < FM_STATE_MAX && aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
      fm_state_row(q, &rows[nr++]);
   aimee_pg_finalize(q);
   char now[32];
   fm_now(now);
   int changed = 0;
   int64_t last_id = 0;
   for (int i = 0; i < nr; i++)
   {
      if ((int)actor->rank < rows[i].authority_rank)
         continue;
      fm_state_t after = rows[i];
      fm_copy(after.lifecycle, sizeof(after.lifecycle), FACT_LIFECYCLE_INVALIDATED);
      fm_copy(after.invalidated_at, sizeof(after.invalidated_at), now);
      after.version++;
      aimee_pg_stmt_t *u = aimee_pg_prepare(
          conn,
          "UPDATE entity_edges SET lifecycle_state='invalidated',invalidated_at=?2,"
          " version=version+1,commit_id=?3 WHERE id=?1",
          err, sizeof(err));
      if (!u)
         return fm_end(conn, 0);
      aimee_pg_bind_int64(u, "?1", rows[i].id);
      aimee_pg_bind_text(u, "?2", now);
      aimee_pg_bind_text(u, "?3", commit_id);
      int ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(u);
      if (!ok || fm_change(conn, commit_id, rows[i].id, "invalidate", &rows[i], &after,
                           "reversible correction") != 0)
         return fm_end(conn, 0);
      if (fm_tombstone_add_assertion(conn, rows[i].id, actor, "explicit fact invalidation") != 0)
         return fm_end(conn, 0);
      changed++;
      last_id = rows[i].id;
   }
   char subject[32];
   snprintf(subject, sizeof(subject), "%lld", (long long)last_id);
   if (fm_commit_finish(conn, actor, commit_id, "fact.invalidate", subject, "applied") != 0 ||
       fm_end(conn, 1) != 0)
      return -1;
   if (out)
   {
      out->assertion_id = last_id;
      out->changed = changed;
      fm_copy(out->commit_id, sizeof(out->commit_id), commit_id);
      fm_copy(out->lifecycle, sizeof(out->lifecycle), FACT_LIFECYCLE_INVALIDATED);
   }
   return changed;
}

int db2_fact_mutation_annotate(const fact_actor_t *actor, int64_t assertion_id,
                               const char *annotation, fact_mutation_result_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!fm_actor_ok(actor) || assertion_id <= 0 || !annotation || !annotation[0])
      return -1;
   void *conn = db2_conn();
   if (fm_begin(conn) != 0)
      return -1;
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       conn,
       "SELECT id,lifecycle_state,superseded_at,invalidated_at,suppressed,confidence,"
       " authority_rank,version,epistemic_kind FROM entity_edges WHERE id=?1"
       " AND edge_class='semantic' FOR UPDATE",
       err, sizeof(err));
   if (!q)
      return fm_end(conn, 0);
   aimee_pg_bind_int64(q, "?1", assertion_id);
   fm_state_t state;
   memset(&state, 0, sizeof(state));
   int eligible = 0;
   if (aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      fm_state_row(q, &state);
      const char *kind = aimee_pg_column_text(q, 8);
      eligible = strcmp(kind, FACT_KIND_EPISODE) == 0 || strcmp(kind, FACT_KIND_EXPERIENCE) == 0;
   }
   aimee_pg_finalize(q);
   if (!eligible)
      return fm_end(conn, 0);
   char commit_id[FACT_COMMIT_ID_MAX];
   if (fm_commit_open(conn, actor, "fact.annotate", 1, commit_id) != 0)
      return fm_end(conn, 0);
   aimee_pg_stmt_t *ins = aimee_pg_prepare(
       conn,
       "INSERT INTO epistemic_annotations(subject_kind,subject_id,annotation,actor,changeset_id)"
       " VALUES('assertion',?1,?2,?3,?4)",
       err, sizeof(err));
   if (!ins)
      return fm_end(conn, 0);
   char id_text[32];
   snprintf(id_text, sizeof(id_text), "%lld", (long long)assertion_id);
   aimee_pg_bind_text(ins, "?1", id_text);
   aimee_pg_bind_text(ins, "?2", annotation);
   aimee_pg_bind_text(ins, "?3", actor->principal);
   aimee_pg_bind_text(ins, "?4", commit_id);
   int ok = aimee_pg_step(ins, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(ins);
   if (!ok ||
       fm_change(conn, commit_id, assertion_id, "contradict", &state, &state,
                 "episode annotation; original retained") != 0 ||
       fm_commit_finish(conn, actor, commit_id, "fact.annotate", id_text, "applied") != 0 ||
       fm_end(conn, 1) != 0)
      return -1;
   if (out)
   {
      out->assertion_id = assertion_id;
      fm_copy(out->commit_id, sizeof(out->commit_id), commit_id);
      fm_copy(out->lifecycle, sizeof(out->lifecycle), state.lifecycle);
   }
   return 0;
}

static int fm_load_id(void *conn, int64_t id, fm_state_t *out)
{
   memset(out, 0, sizeof(*out));
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id,lifecycle_state,superseded_at,invalidated_at,suppressed,confidence,"
       " authority_rank,version FROM entity_edges WHERE id=?1 AND edge_class='semantic' LIMIT 1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ROW)
      fm_state_row(st, out);
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ERR ? -1 : 0;
}

int db2_fact_mutation_review(const fact_actor_t *actor, int64_t assertion_id,
                             fact_review_action_t action, fact_mutation_result_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!fm_actor_ok(actor) || actor->rank != FACT_ACTOR_OPERATOR || assertion_id <= 0 ||
       (action != FACT_REVIEW_APPROVE && action != FACT_REVIEW_REJECT &&
        action != FACT_REVIEW_UNDO))
      return -1;
   void *conn = db2_conn();
   if (fm_begin(conn) != 0)
      return -1;
   fm_state_t before;
   if (fm_load_id(conn, assertion_id, &before) != 0 || !before.found)
      return fm_end(conn, 0);
   char review_source[128] = "", review_relation[128] = "";
   {
      char err[FM_ERRBUF] = "";
      aimee_pg_stmt_t *q = aimee_pg_prepare(
          conn, "SELECT source,relation FROM entity_edges WHERE id=?1 AND edge_class='semantic'",
          err, sizeof(err));
      if (!q)
         return fm_end(conn, 0);
      aimee_pg_bind_int64(q, "?1", assertion_id);
      if (aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         fm_copy(review_source, sizeof(review_source), aimee_pg_column_text(q, 0));
         fm_copy(review_relation, sizeof(review_relation), aimee_pg_column_text(q, 1));
      }
      aimee_pg_finalize(q);
      if (!review_source[0] || !review_relation[0])
         return fm_end(conn, 0);
   }
   char prior[24], next[24], now[32];
   int64_t reverses = 0;
   char reverses_commit[FACT_COMMIT_ID_MAX] = "";
   int undo_authority_rank = before.authority_rank;
   fm_copy(prior, sizeof(prior), before.lifecycle);
   fm_now(now);
   const char *action_text = action == FACT_REVIEW_APPROVE
                                 ? "approve"
                                 : (action == FACT_REVIEW_REJECT ? "reject" : "undo");
   if (action == FACT_REVIEW_APPROVE)
      fm_copy(next, sizeof(next), FACT_LIFECYCLE_PROMOTED);
   else if (action == FACT_REVIEW_REJECT)
      fm_copy(next, sizeof(next), FACT_LIFECYCLE_INVALIDATED);
   else
   {
      char err[FM_ERRBUF] = "";
      aimee_pg_stmt_t *q =
          aimee_pg_prepare(conn,
                           "SELECT r.id,r.prior_lifecycle,r.commit_id,"
                           " (SELECT c.before_authority_rank FROM fact_graph_changes c"
                           "  WHERE c.commit_id=r.commit_id AND c.assertion_id=r.assertion_id"
                           "  ORDER BY c.id DESC LIMIT 1) FROM fact_review_actions r"
                           " WHERE r.assertion_id=?1"
                           " AND r.action IN ('approve','reject') AND NOT EXISTS"
                           " (SELECT 1 FROM fact_review_actions u WHERE u.reverses_review_id=r.id)"
                           " ORDER BY r.id DESC LIMIT 1",
                           err, sizeof(err));
      if (!q)
         return fm_end(conn, 0);
      aimee_pg_bind_int64(q, "?1", assertion_id);
      if (aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         reverses = aimee_pg_column_int64(q, 0);
         fm_copy(next, sizeof(next), aimee_pg_column_text(q, 1));
         fm_copy(reverses_commit, sizeof(reverses_commit), aimee_pg_column_text(q, 2));
         undo_authority_rank = aimee_pg_column_int(q, 3);
      }
      aimee_pg_finalize(q);
      if (!reverses)
         return fm_end(conn, 0);
   }
   if (action == FACT_REVIEW_APPROVE)
   {
      char target[2048] = "", qerr[FM_ERRBUF] = "";
      aimee_pg_stmt_t *tq = aimee_pg_prepare(
          conn, "SELECT target FROM entity_edges WHERE id=?1 AND edge_class='semantic'", qerr,
          sizeof(qerr));
      if (!tq)
         return fm_end(conn, 0);
      aimee_pg_bind_int64(tq, "?1", assertion_id);
      if (aimee_pg_step(tq, qerr, sizeof(qerr)) == AIMEE_PG_ROW)
         fm_copy(target, sizeof(target), aimee_pg_column_text(tq, 0));
      aimee_pg_finalize(tq);
      if (!target[0] || fm_tombstone_blocks(conn, review_source, review_relation, target) != 0)
         return fm_end(conn, 0);
   }
   char commit_id[FACT_COMMIT_ID_MAX];
   if (fm_commit_open(conn, actor, "fact.review", 1, commit_id) != 0)
      return fm_end(conn, 0);
   if (action == FACT_REVIEW_APPROVE && rel_type_is_functional(review_relation))
   {
      char err[FM_ERRBUF] = "";
      aimee_pg_stmt_t *q = aimee_pg_prepare(
          conn,
          "SELECT id,lifecycle_state,superseded_at,invalidated_at,suppressed,confidence,"
          " authority_rank,version FROM entity_edges WHERE source=?1 AND relation=?2 AND id<>?3"
          " AND edge_class='semantic' AND superseded_at='' AND invalidated_at='' AND suppressed=0"
          " AND lifecycle_state IN ('persistent','promoted') ORDER BY id",
          err, sizeof(err));
      if (!q)
         return fm_end(conn, 0);
      aimee_pg_bind_text(q, "?1", review_source);
      aimee_pg_bind_text(q, "?2", review_relation);
      aimee_pg_bind_int64(q, "?3", assertion_id);
      fm_state_t incumbents[FM_STATE_MAX];
      int ni = 0;
      while (ni < FM_STATE_MAX && aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
         fm_state_row(q, &incumbents[ni++]);
      aimee_pg_finalize(q);
      for (int i = 0; i < ni; i++)
      {
         fm_state_t incumbent_after = incumbents[i];
         fm_copy(incumbent_after.lifecycle, sizeof(incumbent_after.lifecycle),
                 FACT_LIFECYCLE_SUPERSEDED);
         fm_copy(incumbent_after.superseded_at, sizeof(incumbent_after.superseded_at), now);
         incumbent_after.version++;
         aimee_pg_stmt_t *u = aimee_pg_prepare(
             conn,
             "UPDATE entity_edges SET lifecycle_state='superseded',superseded_at=?2,"
             " version=version+1,commit_id=?3 WHERE id=?1",
             err, sizeof(err));
         if (!u)
            return fm_end(conn, 0);
         aimee_pg_bind_int64(u, "?1", incumbents[i].id);
         aimee_pg_bind_text(u, "?2", now);
         aimee_pg_bind_text(u, "?3", commit_id);
         int ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE;
         aimee_pg_finalize(u);
         if (!ok || fm_change(conn, commit_id, incumbents[i].id, "supersede", &incumbents[i],
                              &incumbent_after, "operator-approved correction") != 0)
            return fm_end(conn, 0);
      }
   }
   fm_state_t after = before;
   fm_copy(after.lifecycle, sizeof(after.lifecycle), next);
   if (strcmp(next, FACT_LIFECYCLE_INVALIDATED) == 0)
      fm_copy(after.invalidated_at, sizeof(after.invalidated_at), now);
   else
      after.invalidated_at[0] = '\0';
   after.authority_rank = action == FACT_REVIEW_UNDO ? undo_authority_rank : FACT_ACTOR_OPERATOR;
   after.version++;
   /* The DB trigger rejects any transition back to a recallable lifecycle while
    * the tombstone is active.  Deactivate it first in this same transaction;
    * rollback restores it if any later review write fails. */
   if (action == FACT_REVIEW_UNDO && fm_tombstone_restore_assertion(conn, assertion_id, actor) != 0)
      return fm_end(conn, 0);
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *u = aimee_pg_prepare(
       conn,
       "UPDATE entity_edges SET lifecycle_state=?2,invalidated_at=?3,authority_rank=?4,"
       " actor_principal=?5,version=version+1,commit_id=?6 WHERE id=?1",
       err, sizeof(err));
   if (!u)
      return fm_end(conn, 0);
   aimee_pg_bind_int64(u, "?1", assertion_id);
   aimee_pg_bind_text(u, "?2", next);
   aimee_pg_bind_text(u, "?3", after.invalidated_at);
   aimee_pg_bind_int(u, "?4", after.authority_rank);
   aimee_pg_bind_text(u, "?5", actor->principal);
   aimee_pg_bind_text(u, "?6", commit_id);
   int ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(u);
   if (!ok || fm_change(conn, commit_id, assertion_id, action_text, &before, &after,
                        "operator review") != 0)
      return fm_end(conn, 0);
   if (action == FACT_REVIEW_REJECT &&
       fm_tombstone_add_assertion(conn, assertion_id, actor, "operator review rejection") != 0)
      return fm_end(conn, 0);
   if (action == FACT_REVIEW_UNDO)
   {
      /* Restore any incumbents superseded by the reviewed approval. Refuse to
       * overwrite them if another commit has touched them since. */
      fm_state_t restore[FM_STATE_MAX];
      int nr = 0;
      aimee_pg_stmt_t *q = aimee_pg_prepare(
          conn,
          "SELECT assertion_id,before_lifecycle,before_superseded_at,before_invalidated_at,"
          " before_suppressed,before_confidence,before_authority_rank,before_version"
          " FROM fact_graph_changes WHERE commit_id=?1 AND assertion_id<>?2"
          " AND assertion_id>0 ORDER BY id DESC",
          err, sizeof(err));
      if (!q)
         return fm_end(conn, 0);
      aimee_pg_bind_text(q, "?1", reverses_commit);
      aimee_pg_bind_int64(q, "?2", assertion_id);
      while (nr < FM_STATE_MAX && aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         memset(&restore[nr], 0, sizeof(restore[nr]));
         restore[nr].found = 1;
         restore[nr].id = aimee_pg_column_int64(q, 0);
         fm_copy(restore[nr].lifecycle, sizeof(restore[nr].lifecycle), aimee_pg_column_text(q, 1));
         fm_copy(restore[nr].superseded_at, sizeof(restore[nr].superseded_at),
                 aimee_pg_column_text(q, 2));
         fm_copy(restore[nr].invalidated_at, sizeof(restore[nr].invalidated_at),
                 aimee_pg_column_text(q, 3));
         restore[nr].suppressed = aimee_pg_column_int(q, 4);
         restore[nr].confidence = aimee_pg_column_double(q, 5);
         restore[nr].authority_rank = aimee_pg_column_int(q, 6);
         restore[nr].version = aimee_pg_column_int(q, 7);
         nr++;
      }
      aimee_pg_finalize(q);
      for (int i = 0; i < nr; i++)
      {
         fm_state_t current;
         if (fm_load_id(conn, restore[i].id, &current) != 0 || !current.found)
            return fm_end(conn, 0);
         aimee_pg_stmt_t *ru = aimee_pg_prepare(
             conn,
             "UPDATE entity_edges SET lifecycle_state=?2,superseded_at=?3,invalidated_at=?4,"
             " suppressed=?5,confidence=?6,authority_rank=?7,version=?8,commit_id=?9"
             " WHERE id=?1 AND commit_id=?10",
             err, sizeof(err));
         if (!ru)
            return fm_end(conn, 0);
         aimee_pg_bind_int64(ru, "?1", restore[i].id);
         aimee_pg_bind_text(ru, "?2", restore[i].lifecycle);
         aimee_pg_bind_text(ru, "?3", restore[i].superseded_at);
         aimee_pg_bind_text(ru, "?4", restore[i].invalidated_at);
         aimee_pg_bind_int(ru, "?5", restore[i].suppressed);
         aimee_pg_bind_double(ru, "?6", restore[i].confidence);
         aimee_pg_bind_int(ru, "?7", restore[i].authority_rank);
         aimee_pg_bind_int(ru, "?8", restore[i].version);
         aimee_pg_bind_text(ru, "?9", commit_id);
         aimee_pg_bind_text(ru, "?10", reverses_commit);
         int restored =
             aimee_pg_step(ru, err, sizeof(err)) == AIMEE_PG_DONE && aimee_pg_stmt_changes(ru) == 1;
         aimee_pg_finalize(ru);
         if (!restored || fm_change(conn, commit_id, restore[i].id, "undo", &current, &restore[i],
                                    "restore approved incumbent") != 0)
            return fm_end(conn, 0);
      }
   }
   aimee_pg_stmt_t *ri = aimee_pg_prepare(
       conn,
       "INSERT INTO fact_review_actions(assertion_id,action,prior_lifecycle,new_lifecycle,"
       " actor_principal,commit_id,reverses_review_id) VALUES(?1,?2,?3,?4,?5,?6,?7)",
       err, sizeof(err));
   if (!ri)
      return fm_end(conn, 0);
   aimee_pg_bind_int64(ri, "?1", assertion_id);
   aimee_pg_bind_text(ri, "?2", action_text);
   aimee_pg_bind_text(ri, "?3", prior);
   aimee_pg_bind_text(ri, "?4", next);
   aimee_pg_bind_text(ri, "?5", actor->principal);
   aimee_pg_bind_text(ri, "?6", commit_id);
   aimee_pg_bind_int64(ri, "?7", reverses);
   ok = aimee_pg_step(ri, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(ri);
   char subject[32];
   snprintf(subject, sizeof(subject), "%lld", (long long)assertion_id);
   if (!ok || fm_commit_finish(conn, actor, commit_id, "fact.review", subject, "applied") != 0 ||
       fm_end(conn, 1) != 0)
      return -1;
   if (out)
   {
      out->assertion_id = assertion_id;
      out->changed = 1;
      fm_copy(out->lifecycle, sizeof(out->lifecycle), next);
      fm_copy(out->commit_id, sizeof(out->commit_id), commit_id);
   }
   return 0;
}

int db2_fact_commit_preview(const char *commit_id, fact_commit_change_t *out, int max)
{
   if (!commit_id || !commit_id[0] || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT assertion_id,object_kind,object_key,action,before_lifecycle,after_lifecycle,"
       " before_authority_rank,after_authority_rank,existed_before,existed_after,diff_detail"
       " FROM fact_graph_changes"
       " WHERE commit_id=?1 ORDER BY id LIMIT ?2",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", commit_id);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].assertion_id = aimee_pg_column_int64(st, 0);
      fm_copy(out[n].object_kind, sizeof(out[n].object_kind), aimee_pg_column_text(st, 1));
      fm_copy(out[n].object_key, sizeof(out[n].object_key), aimee_pg_column_text(st, 2));
      fm_copy(out[n].action, sizeof(out[n].action), aimee_pg_column_text(st, 3));
      fm_copy(out[n].before_lifecycle, sizeof(out[n].before_lifecycle),
              aimee_pg_column_text(st, 4));
      fm_copy(out[n].after_lifecycle, sizeof(out[n].after_lifecycle), aimee_pg_column_text(st, 5));
      out[n].before_authority_rank = aimee_pg_column_int(st, 6);
      out[n].after_authority_rank = aimee_pg_column_int(st, 7);
      out[n].existed_before = aimee_pg_column_int(st, 8);
      out[n].existed_after = aimee_pg_column_int(st, 9);
      fm_copy(out[n].detail, sizeof(out[n].detail), aimee_pg_column_text(st, 10));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_fact_ingest_run_preview(const char *ingest_run_id, fact_commit_change_t *out, int max)
{
   if (!ingest_run_id || !ingest_run_id[0] || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT ch.assertion_id,ch.object_kind,ch.object_key,ch.action,ch.before_lifecycle,"
       " ch.after_lifecycle,ch.before_authority_rank,ch.after_authority_rank,"
       " ch.existed_before,ch.existed_after,ch.diff_detail FROM fact_graph_changes ch"
       " JOIN fact_graph_commits c ON c.commit_id=ch.commit_id"
       " WHERE c.parent_commit_id=?1 ORDER BY ch.id LIMIT ?2",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", ingest_run_id);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].assertion_id = aimee_pg_column_int64(st, 0);
      fm_copy(out[n].object_kind, sizeof(out[n].object_kind), aimee_pg_column_text(st, 1));
      fm_copy(out[n].object_key, sizeof(out[n].object_key), aimee_pg_column_text(st, 2));
      fm_copy(out[n].action, sizeof(out[n].action), aimee_pg_column_text(st, 3));
      fm_copy(out[n].before_lifecycle, sizeof(out[n].before_lifecycle),
              aimee_pg_column_text(st, 4));
      fm_copy(out[n].after_lifecycle, sizeof(out[n].after_lifecycle), aimee_pg_column_text(st, 5));
      out[n].before_authority_rank = aimee_pg_column_int(st, 6);
      out[n].after_authority_rank = aimee_pg_column_int(st, 7);
      out[n].existed_before = aimee_pg_column_int(st, 8);
      out[n].existed_after = aimee_pg_column_int(st, 9);
      fm_copy(out[n].detail, sizeof(out[n].detail), aimee_pg_column_text(st, 10));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

/* Reverse the external graph transitions registered by the ontology and entity
 * modules without leaving this transaction.  The object key is deliberately a
 * stable surrogate (relation name or merge id), never assertion content. */
static int fm_rollback_external(void *conn, const char *kind, const char *key, const char *action)
{
   char err[FM_ERRBUF] = "";
   if (strcmp(kind, "relation") == 0 &&
       (strcmp(action, "promote") == 0 || strcmp(action, "map") == 0 ||
        strcmp(action, "reject") == 0))
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn,
          "UPDATE ontology_evaluations SET status='pending',mapped_to='',decided_at=''"
          " WHERE rel_type=?1",
          err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", key);
      int ok =
          aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE && aimee_pg_stmt_changes(st) == 1;
      aimee_pg_finalize(st);
      if (!ok)
         return -1;
      st = aimee_pg_prepare(conn, "UPDATE rel_types SET status='provisional' WHERE rel_type=?1",
                            err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", key);
      ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(st);
      return ok ? 0 : -1;
   }
   if (strcmp(kind, "entity_merge") != 0)
      return -1;
   char *end = NULL;
   long long merge_id = strtoll(key, &end, 10);
   if (merge_id <= 0 || !end || *end)
      return -1;
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       conn, "SELECT from_id,into_id,undone FROM entity_merges WHERE id=?1 LIMIT 1", err,
       sizeof(err));
   if (!q)
      return -1;
   aimee_pg_bind_int64(q, "?1", (int64_t)merge_id);
   int64_t from_id = 0, into_id = 0;
   int undone = -1;
   if (aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      from_id = aimee_pg_column_int64(q, 0);
      into_id = aimee_pg_column_int64(q, 1);
      undone = aimee_pg_column_int(q, 2);
   }
   aimee_pg_finalize(q);
   if (from_id <= 0 || into_id <= 0)
      return -1;
   int rollback_merge = strcmp(action, "merge") == 0;
   if ((!rollback_merge && strcmp(action, "unmerge") != 0) || (rollback_merge && undone != 0) ||
       (!rollback_merge && undone != 1))
      return -1;
   const char *sql = rollback_merge
                         ? "UPDATE entity_registry SET status='active',merged_into=0"
                           " WHERE canonical_id=?1 AND status='merged' AND merged_into=?2"
                         : "UPDATE entity_registry SET status='merged',merged_into=?2"
                           " WHERE canonical_id=?1 AND status='active'";
   aimee_pg_stmt_t *u = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!u)
      return -1;
   aimee_pg_bind_int64(u, "?1", from_id);
   aimee_pg_bind_int64(u, "?2", into_id);
   int ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE && aimee_pg_stmt_changes(u) == 1;
   aimee_pg_finalize(u);
   if (!ok)
      return -1;
   u = aimee_pg_prepare(conn, "UPDATE entity_merges SET undone=?2 WHERE id=?1", err, sizeof(err));
   if (!u)
      return -1;
   aimee_pg_bind_int64(u, "?1", (int64_t)merge_id);
   aimee_pg_bind_int(u, "?2", rollback_merge ? 1 : 0);
   ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(u);
   return ok ? 0 : -1;
}

int db2_fact_commit_rollback(const fact_actor_t *actor, const char *target_commit,
                             char rollback_id[FACT_COMMIT_ID_MAX])
{
   if (rollback_id)
      rollback_id[0] = '\0';
   if (!fm_actor_ok(actor) || actor->rank != FACT_ACTOR_OPERATOR || !target_commit ||
       !target_commit[0])
      return -1;
   void *conn = db2_conn();
   if (fm_begin(conn) != 0)
      return -1;
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *cq =
       aimee_pg_prepare(conn,
                        "SELECT 1 FROM fact_graph_commits WHERE commit_id=?1 AND status='applied'"
                        " AND reversible=1 LIMIT 1",
                        err, sizeof(err));
   if (!cq)
      return fm_end(conn, 0);
   aimee_pg_bind_text(cq, "?1", target_commit);
   int eligible = aimee_pg_step(cq, err, sizeof(err)) == AIMEE_PG_ROW;
   aimee_pg_finalize(cq);
   if (!eligible)
      return fm_end(conn, 0);
   /* Never replay an old before-image over a newer decision touching the same
    * assertion or external graph object.  The caller must roll back descendants
    * first, which makes rollback order explicit and deterministic. */
   cq = aimee_pg_prepare(
       conn,
       "SELECT 1 FROM fact_graph_changes t JOIN fact_graph_changes n ON n.id>t.id"
       " AND n.commit_id<>t.commit_id JOIN fact_graph_commits nc ON nc.commit_id=n.commit_id"
       " AND nc.status='applied' AND nc.operation NOT IN ('revert','fact.ingest.rollback')"
       " WHERE ((t.assertion_id>0 AND n.assertion_id=t.assertion_id)"
       " OR (t.assertion_id=0 AND n.assertion_id=0 AND n.object_kind=t.object_kind"
       " AND n.object_key=t.object_key)) AND t.commit_id=?1 LIMIT 1",
       err, sizeof(err));
   if (!cq)
      return fm_end(conn, 0);
   aimee_pg_bind_text(cq, "?1", target_commit);
   int has_descendant = aimee_pg_step(cq, err, sizeof(err)) == AIMEE_PG_ROW;
   aimee_pg_finalize(cq);
   if (has_descendant)
      return fm_end(conn, 0);
   char rid[FACT_COMMIT_ID_MAX];
   if (fm_commit_open(conn, actor, "revert", 1, rid) != 0)
      return fm_end(conn, 0);
   aimee_pg_stmt_t *revert_link = aimee_pg_prepare(
       conn, "UPDATE fact_graph_commits SET reverts_changeset=?2 WHERE commit_id=?1", err,
       sizeof(err));
   if (!revert_link)
      return fm_end(conn, 0);
   aimee_pg_bind_text(revert_link, "?1", rid);
   aimee_pg_bind_text(revert_link, "?2", target_commit);
   int linked = aimee_pg_step(revert_link, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(revert_link);
   if (!linked)
      return fm_end(conn, 0);

   aimee_pg_stmt_t *q = aimee_pg_prepare(
       conn,
       "SELECT "
       "assertion_id,object_kind,object_key,action,existed_before,before_lifecycle,before_"
       "superseded_at,"
       " before_invalidated_at,before_suppressed,before_confidence,before_authority_rank,"
       " before_version FROM fact_graph_changes WHERE commit_id=?1 ORDER BY id DESC",
       err, sizeof(err));
   if (!q)
      return fm_end(conn, 0);
   aimee_pg_bind_text(q, "?1", target_commit);
   int64_t ids[FM_STATE_MAX];
   fm_state_t befores[FM_STATE_MAX];
   int existed[FM_STATE_MAX];
   char kinds[FM_STATE_MAX][32], keys[FM_STATE_MAX][128], actions[FM_STATE_MAX][32];
   int n = 0;
   while (n < FM_STATE_MAX && aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      ids[n] = aimee_pg_column_int64(q, 0);
      fm_copy(kinds[n], sizeof(kinds[n]), aimee_pg_column_text(q, 1));
      fm_copy(keys[n], sizeof(keys[n]), aimee_pg_column_text(q, 2));
      fm_copy(actions[n], sizeof(actions[n]), aimee_pg_column_text(q, 3));
      existed[n] = aimee_pg_column_int(q, 4);
      memset(&befores[n], 0, sizeof(befores[n]));
      befores[n].found = existed[n];
      befores[n].id = ids[n];
      fm_copy(befores[n].lifecycle, sizeof(befores[n].lifecycle), aimee_pg_column_text(q, 5));
      fm_copy(befores[n].superseded_at, sizeof(befores[n].superseded_at),
              aimee_pg_column_text(q, 6));
      fm_copy(befores[n].invalidated_at, sizeof(befores[n].invalidated_at),
              aimee_pg_column_text(q, 7));
      befores[n].suppressed = aimee_pg_column_int(q, 8);
      befores[n].confidence = aimee_pg_column_double(q, 9);
      befores[n].authority_rank = aimee_pg_column_int(q, 10);
      befores[n].version = aimee_pg_column_int(q, 11);
      n++;
   }
   aimee_pg_finalize(q);
   char now[32];
   fm_now(now);
   for (int i = 0; i < n; i++)
   {
      if (ids[i] == 0)
      {
         if (fm_rollback_external(conn, kinds[i], keys[i], actions[i]) != 0 ||
             fm_external_change(conn, rid, kinds[i], keys[i], "rollback", "applied", "restored",
                                "external batch rollback") != 0)
            return fm_end(conn, 0);
         continue;
      }
      fm_state_t current;
      if (fm_load_id(conn, ids[i], &current) != 0 || !current.found)
         continue; /* erased rows cannot belong to a reversible commit */
      fm_state_t after;
      if (!existed[i])
      {
         after = current;
         fm_copy(after.lifecycle, sizeof(after.lifecycle), FACT_LIFECYCLE_INVALIDATED);
         fm_copy(after.invalidated_at, sizeof(after.invalidated_at), now);
         /* Rolling an insertion back is an operator decision about the triple,
          * not just about this commit's rows.  Record that authority so a later
          * drain that re-extracts the same triple from new text is refused by
          * the reactivate gate; the evidence-replay guard only covers an exact
          * redelivery of the same mention. */
         if ((int)actor->rank > after.authority_rank)
            after.authority_rank = (int)actor->rank;
         after.version++;
      }
      else
      {
         after = befores[i];
         after.found = 1;
      }
      aimee_pg_stmt_t *u = aimee_pg_prepare(
          conn,
          "UPDATE entity_edges SET lifecycle_state=?2,superseded_at=?3,invalidated_at=?4,"
          " suppressed=?5,confidence=?6,authority_rank=?7,version=?8,commit_id=?9 WHERE id=?1",
          err, sizeof(err));
      if (!u)
         return fm_end(conn, 0);
      aimee_pg_bind_int64(u, "?1", ids[i]);
      aimee_pg_bind_text(u, "?2", after.lifecycle);
      aimee_pg_bind_text(u, "?3", after.superseded_at);
      aimee_pg_bind_text(u, "?4", after.invalidated_at);
      aimee_pg_bind_int(u, "?5", after.suppressed);
      aimee_pg_bind_double(u, "?6", after.confidence);
      aimee_pg_bind_int(u, "?7", after.authority_rank);
      aimee_pg_bind_int(u, "?8", after.version);
      aimee_pg_bind_text(u, "?9", rid);
      int ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(u);
      if (!ok || fm_change(conn, rid, ids[i], "rollback", &current, &after, "batch rollback") != 0)
         return fm_end(conn, 0);
   }
   aimee_pg_stmt_t *ev = aimee_pg_prepare(
       conn, "UPDATE fact_evidence SET invalidated_at=?2 WHERE commit_id=?1 AND invalidated_at=''",
       err, sizeof(err));
   if (!ev)
      return fm_end(conn, 0);
   aimee_pg_bind_text(ev, "?1", target_commit);
   aimee_pg_bind_text(ev, "?2", now);
   int ok = aimee_pg_step(ev, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(ev);
   aimee_pg_stmt_t *mark = aimee_pg_prepare(
       conn,
       "UPDATE fact_graph_commits SET status='reverted',rolled_back_at=?2,rolled_back_by=?3"
       " WHERE commit_id=?1 AND status='applied'",
       err, sizeof(err));
   if (!mark)
      return fm_end(conn, 0);
   aimee_pg_bind_text(mark, "?1", target_commit);
   aimee_pg_bind_text(mark, "?2", now);
   aimee_pg_bind_text(mark, "?3", actor->principal);
   ok = ok && aimee_pg_step(mark, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(mark);
   if (!ok || fm_commit_finish(conn, actor, rid, "revert", target_commit, "applied") != 0 ||
       fm_end(conn, 1) != 0)
      return -1;
   if (rollback_id)
      fm_copy(rollback_id, FACT_COMMIT_ID_MAX, rid);
   return n;
}

int db2_fact_ingest_run_rollback(const fact_actor_t *actor, const char *ingest_run_id,
                                 char rollback_id[FACT_COMMIT_ID_MAX])
{
   if (rollback_id)
      rollback_id[0] = '\0';
   if (!fm_actor_ok(actor) || actor->rank != FACT_ACTOR_OPERATOR || !ingest_run_id ||
       !ingest_run_id[0])
      return -1;
   void *conn = db2_conn();
   if (fm_begin(conn) != 0)
      return -1;
   char err[FM_ERRBUF] = "";

   /* Every member must still be an applied reversible commit.  Mixing an
    * already-rolled-back member into a run would make the batch non-atomic. */
   aimee_pg_stmt_t *cq = aimee_pg_prepare(
       conn,
       "SELECT COUNT(*),SUM(CASE WHEN status='applied' AND reversible=1 THEN 1 ELSE 0 END)"
       " FROM fact_graph_commits WHERE parent_commit_id=?1",
       err, sizeof(err));
   if (!cq)
      return fm_end(conn, 0);
   aimee_pg_bind_text(cq, "?1", ingest_run_id);
   int total = 0, eligible = 0;
   if (aimee_pg_step(cq, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      total = aimee_pg_column_int(cq, 0);
      eligible = aimee_pg_column_int(cq, 1);
   }
   aimee_pg_finalize(cq);
   if (total <= 0 || total != eligible)
      return fm_end(conn, 0);

   /* Changes made after the run to any touched object must be rolled back
    * first.  Later changes inside this same run are intentionally allowed and
    * are replayed in reverse global order below. */
   cq = aimee_pg_prepare(
       conn,
       "SELECT 1 FROM fact_graph_changes t"
       " JOIN fact_graph_commits tc ON tc.commit_id=t.commit_id"
       " JOIN fact_graph_changes n ON n.id>t.id AND ((t.assertion_id>0 AND"
       " n.assertion_id=t.assertion_id) OR (t.assertion_id=0 AND n.assertion_id=0 AND"
       " n.object_kind=t.object_kind AND n.object_key=t.object_key))"
       " JOIN fact_graph_commits nc ON nc.commit_id=n.commit_id"
       " WHERE tc.parent_commit_id=?1 AND nc.status='applied'"
       " AND nc.operation NOT IN ('revert','fact.ingest.rollback')"
       " AND nc.parent_commit_id<>?1 LIMIT 1",
       err, sizeof(err));
   if (!cq)
      return fm_end(conn, 0);
   aimee_pg_bind_text(cq, "?1", ingest_run_id);
   int has_descendant = aimee_pg_step(cq, err, sizeof(err)) == AIMEE_PG_ROW;
   aimee_pg_finalize(cq);
   if (has_descendant)
      return fm_end(conn, 0);

   char rid[FACT_COMMIT_ID_MAX];
   if (fm_commit_open(conn, actor, "fact.ingest.rollback", 1, rid) != 0)
      return fm_end(conn, 0);

   aimee_pg_stmt_t *q = aimee_pg_prepare(
       conn,
       "SELECT ch.assertion_id,ch.object_kind,ch.object_key,ch.action,ch.existed_before,"
       " ch.before_lifecycle,ch.before_superseded_at,ch.before_invalidated_at,"
       " ch.before_suppressed,ch.before_confidence,ch.before_authority_rank,ch.before_version"
       " FROM fact_graph_changes ch JOIN fact_graph_commits c ON c.commit_id=ch.commit_id"
       " WHERE c.parent_commit_id=?1 ORDER BY ch.id DESC",
       err, sizeof(err));
   if (!q)
      return fm_end(conn, 0);
   aimee_pg_bind_text(q, "?1", ingest_run_id);
   fm_rollback_item_t *items = NULL;
   int n = 0, cap = 0, load_ok = 1;
   for (;;)
   {
      aimee_pg_step_t step = aimee_pg_step(q, err, sizeof(err));
      if (step == AIMEE_PG_DONE)
         break;
      if (step != AIMEE_PG_ROW)
      {
         load_ok = 0;
         break;
      }
      if (n == cap)
      {
         int next = cap ? cap * 2 : 32;
         fm_rollback_item_t *grown = realloc(items, (size_t)next * sizeof(*items));
         if (!grown)
         {
            load_ok = 0;
            break;
         }
         items = grown;
         cap = next;
      }
      fm_rollback_item_t *it = &items[n++];
      memset(it, 0, sizeof(*it));
      it->id = aimee_pg_column_int64(q, 0);
      fm_copy(it->kind, sizeof(it->kind), aimee_pg_column_text(q, 1));
      fm_copy(it->key, sizeof(it->key), aimee_pg_column_text(q, 2));
      fm_copy(it->action, sizeof(it->action), aimee_pg_column_text(q, 3));
      it->existed = aimee_pg_column_int(q, 4);
      it->before.found = it->existed;
      it->before.id = it->id;
      fm_copy(it->before.lifecycle, sizeof(it->before.lifecycle), aimee_pg_column_text(q, 5));
      fm_copy(it->before.superseded_at, sizeof(it->before.superseded_at),
              aimee_pg_column_text(q, 6));
      fm_copy(it->before.invalidated_at, sizeof(it->before.invalidated_at),
              aimee_pg_column_text(q, 7));
      it->before.suppressed = aimee_pg_column_int(q, 8);
      it->before.confidence = aimee_pg_column_double(q, 9);
      it->before.authority_rank = aimee_pg_column_int(q, 10);
      it->before.version = aimee_pg_column_int(q, 11);
   }
   aimee_pg_finalize(q);
   if (!load_ok || n <= 0)
   {
      free(items);
      return fm_end(conn, 0);
   }

   char now[32];
   fm_now(now);
   for (int i = 0; i < n; i++)
   {
      fm_rollback_item_t *it = &items[i];
      if (it->id == 0)
      {
         if (fm_rollback_external(conn, it->kind, it->key, it->action) != 0 ||
             fm_external_change(conn, rid, it->kind, it->key, "rollback", "applied", "restored",
                                "atomic ingest-run rollback") != 0)
         {
            free(items);
            return fm_end(conn, 0);
         }
         continue;
      }
      fm_state_t current;
      if (fm_load_id(conn, it->id, &current) != 0 || !current.found)
         continue;
      fm_state_t after = it->before;
      if (!it->existed)
      {
         after = current;
         fm_copy(after.lifecycle, sizeof(after.lifecycle), FACT_LIFECYCLE_INVALIDATED);
         fm_copy(after.invalidated_at, sizeof(after.invalidated_at), now);
         /* See db2_fact_commit_rollback: the operator's authority must land on
          * the row so re-extraction cannot re-establish the rolled-back triple. */
         if ((int)actor->rank > after.authority_rank)
            after.authority_rank = (int)actor->rank;
         after.version++;
      }
      after.found = 1;
      aimee_pg_stmt_t *u = aimee_pg_prepare(
          conn,
          "UPDATE entity_edges SET lifecycle_state=?2,superseded_at=?3,invalidated_at=?4,"
          " suppressed=?5,confidence=?6,authority_rank=?7,version=?8,commit_id=?9 WHERE id=?1",
          err, sizeof(err));
      if (!u)
      {
         free(items);
         return fm_end(conn, 0);
      }
      aimee_pg_bind_int64(u, "?1", it->id);
      aimee_pg_bind_text(u, "?2", after.lifecycle);
      aimee_pg_bind_text(u, "?3", after.superseded_at);
      aimee_pg_bind_text(u, "?4", after.invalidated_at);
      aimee_pg_bind_int(u, "?5", after.suppressed);
      aimee_pg_bind_double(u, "?6", after.confidence);
      aimee_pg_bind_int(u, "?7", after.authority_rank);
      aimee_pg_bind_int(u, "?8", after.version);
      aimee_pg_bind_text(u, "?9", rid);
      int ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(u);
      if (!ok || fm_change(conn, rid, it->id, "rollback", &current, &after,
                           "atomic ingest-run rollback") != 0)
      {
         free(items);
         return fm_end(conn, 0);
      }
   }
   free(items);

   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE fact_evidence SET invalidated_at=?2 WHERE ingest_run_id=?1 AND invalidated_at=''",
       err, sizeof(err));
   if (!st)
      return fm_end(conn, 0);
   aimee_pg_bind_text(st, "?1", ingest_run_id);
   aimee_pg_bind_text(st, "?2", now);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   st = aimee_pg_prepare(
       conn,
       "UPDATE fact_graph_commits SET status='rolled_back',rolled_back_at=?2,rolled_back_by=?3"
       " WHERE parent_commit_id=?1 AND status='applied'",
       err, sizeof(err));
   if (!st)
      return fm_end(conn, 0);
   aimee_pg_bind_text(st, "?1", ingest_run_id);
   aimee_pg_bind_text(st, "?2", now);
   aimee_pg_bind_text(st, "?3", actor->principal);
   ok = ok && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE &&
        aimee_pg_stmt_changes(st) == total;
   aimee_pg_finalize(st);
   if (!ok ||
       fm_commit_finish(conn, actor, rid, "fact.ingest.rollback", ingest_run_id, "applied") != 0 ||
       fm_end(conn, 1) != 0)
      return -1;
   if (rollback_id)
      fm_copy(rollback_id, FACT_COMMIT_ID_MAX, rid);
   return n;
}

int db2_fact_erasure_preview(const char *source, const char *relation, const char *target,
                             fact_erasure_impact_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!source || !source[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char sql[768], err[FM_ERRBUF] = "";
   if (fm_selector_sql(sql, sizeof(sql), "SELECT COUNT(*) FROM entity_edges", "", relation,
                       target) != 0)
      return -1;
   aimee_pg_stmt_t *q = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!q)
      return -1;
   fm_bind_selector(q, source, relation, target);
   if (aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
      out->assertion_count = aimee_pg_column_int(q, 0);
   aimee_pg_finalize(q);

   /* Evidence impact uses the same selector through its assertion join. */
   snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM fact_evidence fe JOIN entity_edges e ON e.id=fe.assertion_id"
            " WHERE e.source=?1 AND e.edge_class='semantic'%s%s",
            relation && relation[0] ? " AND e.relation=?2" : "",
            target && target[0]
                ? (relation && relation[0] ? " AND e.target=?3" : " AND e.target=?2")
                : "");
   q = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!q)
      return -1;
   fm_bind_selector(q, source, relation, target);
   if (aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
      out->evidence_count = aimee_pg_column_int(q, 0);
   aimee_pg_finalize(q);
   snprintf(out->residual_data, sizeof(out->residual_data),
            "removes assertion and evidence rows; retains content-free graph commit ids and WORM "
            "audit; entity aliases, replicas, backups, exports, and external vector stores are not "
            "automatically erased; derived profile views update immediately");
   return 0;
}

int db2_fact_erasure_execute(const fact_actor_t *actor, const char *source, const char *relation,
                             const char *target, fact_erasure_impact_t *out,
                             char commit_id[FACT_COMMIT_ID_MAX])
{
   if (commit_id)
      commit_id[0] = '\0';
   if (!fm_actor_ok(actor) || actor->rank != FACT_ACTOR_OPERATOR || !source || !source[0] || !out)
      return -1;
   void *conn = db2_conn();
   if (fm_begin(conn) != 0)
      return -1;
   /* Recompute the impact inside the erasure transaction.  The public preview
    * remains advisory; this report is the cascade actually authorized. */
   if (db2_fact_erasure_preview(source, relation, target, out) != 0)
      return fm_end(conn, 0);
   char cid[FACT_COMMIT_ID_MAX];
   if (fm_commit_open(conn, actor, "fact.erase", 0, cid) != 0)
      return fm_end(conn, 0);
   char sql[768], err[FM_ERRBUF] = "", now[32];
   fm_now(now);
   if (fm_selector_sql(sql, sizeof(sql), "DELETE FROM entity_edges", "", relation, target) != 0)
      return fm_end(conn, 0);
   aimee_pg_stmt_t *del = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!del)
      return fm_end(conn, 0);
   fm_bind_selector(del, source, relation, target);
   int ok = aimee_pg_step(del, err, sizeof(err)) == AIMEE_PG_DONE;
   int removed = aimee_pg_stmt_changes(del);
   aimee_pg_finalize(del);
   if (!ok || removed != out->assertion_count)
      return fm_end(conn, 0);
   aimee_pg_stmt_t *ri = aimee_pg_prepare(
       conn,
       "INSERT INTO fact_erasure_reports(commit_id,selector,assertion_count,evidence_count,"
       " residual_data,completed_at) VALUES(?1,?2,?3,?4,?5,?6)",
       err, sizeof(err));
   if (!ri)
      return fm_end(conn, 0);
   /* Selector contains only the subject label; relation/target content is not
    * retained after an erasure. */
   aimee_pg_bind_text(ri, "?1", cid);
   aimee_pg_bind_text(ri, "?2", "semantic assertion selector (content erased)");
   aimee_pg_bind_int(ri, "?3", out->assertion_count);
   aimee_pg_bind_int(ri, "?4", out->evidence_count);
   aimee_pg_bind_text(ri, "?5", out->residual_data);
   aimee_pg_bind_text(ri, "?6", now);
   ok = aimee_pg_step(ri, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(ri);
   if (!ok || fm_commit_finish(conn, actor, cid, "fact.erase", "erased-selector", "erased") != 0 ||
       fm_end(conn, 1) != 0)
      return -1;
   if (commit_id)
      fm_copy(commit_id, FACT_COMMIT_ID_MAX, cid);
   return removed;
}

int db2_fact_candidates(fact_candidate_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[FM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT e.id,e.source,e.relation,e.target,e.assertion_kind,e.lifecycle_state,"
       " e.authority_rank,(SELECT COUNT(*) FROM fact_evidence fe WHERE fe.assertion_id=e.id"
       " AND fe.stance='supports' AND fe.invalidated_at=''),e.commit_id FROM entity_edges e"
       " WHERE e.edge_class='semantic' AND e.lifecycle_state='candidate'"
       " AND e.superseded_at='' AND e.invalidated_at='' AND e.suppressed=0"
       " ORDER BY e.id LIMIT ?1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].id = aimee_pg_column_int64(st, 0);
      fm_copy(out[n].source, sizeof(out[n].source), aimee_pg_column_text(st, 1));
      fm_copy(out[n].relation, sizeof(out[n].relation), aimee_pg_column_text(st, 2));
      fm_copy(out[n].target, sizeof(out[n].target), aimee_pg_column_text(st, 3));
      fm_copy(out[n].assertion_kind, sizeof(out[n].assertion_kind), aimee_pg_column_text(st, 4));
      fm_copy(out[n].lifecycle, sizeof(out[n].lifecycle), aimee_pg_column_text(st, 5));
      out[n].authority_rank = aimee_pg_column_int(st, 6);
      out[n].evidence_count = aimee_pg_column_int(st, 7);
      fm_copy(out[n].commit_id, sizeof(out[n].commit_id), aimee_pg_column_text(st, 8));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

static int fm_maintenance_transition(const fact_actor_t *actor, int promote, int threshold,
                                     const char *cutoff)
{
   if (!fm_actor_ok(actor) || actor->rank != FACT_ACTOR_SYSTEM ||
       (promote ? threshold <= 0 : (!cutoff || !cutoff[0])))
      return -1;
   void *conn = db2_conn();
   if (fm_begin(conn) != 0)
      return -1;
   const char *operation = promote ? "fact.maintenance.promote" : "fact.maintenance.expire";
   char cid[FACT_COMMIT_ID_MAX];
   if (fm_commit_open(conn, actor, operation, 1, cid) != 0)
      return fm_end(conn, 0);
   char err[FM_ERRBUF] = "";
   const char *sql =
       promote
           ? "SELECT e.id,e.lifecycle_state,e.superseded_at,e.invalidated_at,e.suppressed,"
             " e.confidence,e.authority_rank,e.version FROM entity_edges e"
             " WHERE e.edge_class='semantic' AND e.lifecycle_state='candidate'"
             " AND e.authority_rank<=20"
             " AND e.confidence_class='B' AND e.superseded_at='' AND e.invalidated_at=''"
             " AND e.suppressed=0 AND (SELECT COUNT(*) FROM fact_evidence fe"
             " WHERE fe.assertion_id=e.id AND fe.stance='supports' AND fe.invalidated_at='')>=?1"
           : "SELECT e.id,e.lifecycle_state,e.superseded_at,e.invalidated_at,e.suppressed,"
             " e.confidence,e.authority_rank,e.version FROM entity_edges e"
             " WHERE e.edge_class='semantic' AND e.lifecycle_state='candidate'"
             " AND e.authority_rank<=20"
             " AND e.confidence_class='C' AND e.superseded_at='' AND e.invalidated_at=''"
             " AND e.suppressed=0 AND e.asserted_at<>'' AND e.asserted_at<?1"
             " AND (SELECT COUNT(*) FROM fact_evidence fe WHERE fe.assertion_id=e.id"
             " AND fe.stance='supports' AND fe.invalidated_at='')<=1";
   aimee_pg_stmt_t *q = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!q)
      return fm_end(conn, 0);
   if (promote)
      aimee_pg_bind_int(q, "?1", threshold);
   else
      aimee_pg_bind_text(q, "?1", cutoff);
   fm_state_t rows[FM_STATE_MAX];
   int nr = 0;
   while (nr < FM_STATE_MAX && aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
      fm_state_row(q, &rows[nr++]);
   aimee_pg_finalize(q);
   char now[32];
   fm_now(now);
   for (int i = 0; i < nr; i++)
   {
      fm_state_t after = rows[i];
      if (promote)
      {
         fm_copy(after.lifecycle, sizeof(after.lifecycle), FACT_LIFECYCLE_PERSISTENT);
         if (after.confidence < 0.8)
            after.confidence = 0.8;
      }
      else
      {
         fm_copy(after.lifecycle, sizeof(after.lifecycle), FACT_LIFECYCLE_INVALIDATED);
         fm_copy(after.invalidated_at, sizeof(after.invalidated_at), now);
      }
      after.version++;
      aimee_pg_stmt_t *u = aimee_pg_prepare(
          conn,
          "UPDATE entity_edges SET lifecycle_state=?2,invalidated_at=?3,confidence=?4,"
          " version=version+1,commit_id=?5 WHERE id=?1",
          err, sizeof(err));
      if (!u)
         return fm_end(conn, 0);
      aimee_pg_bind_int64(u, "?1", rows[i].id);
      aimee_pg_bind_text(u, "?2", after.lifecycle);
      aimee_pg_bind_text(u, "?3", after.invalidated_at);
      aimee_pg_bind_double(u, "?4", after.confidence);
      aimee_pg_bind_text(u, "?5", cid);
      int ok = aimee_pg_step(u, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(u);
      if (!ok || fm_change(conn, cid, rows[i].id, promote ? "promote" : "expire", &rows[i], &after,
                           promote ? "evidence threshold reached" : "candidate TTL elapsed") != 0)
         return fm_end(conn, 0);
   }
   if (fm_commit_finish(conn, actor, cid, operation, "semantic-candidate-batch", "applied") != 0 ||
       fm_end(conn, 1) != 0)
      return -1;
   return nr;
}

int db2_fact_mutation_promote_supported(const fact_actor_t *actor, int threshold)
{
   return fm_maintenance_transition(actor, 1, threshold, NULL);
}

int db2_fact_mutation_expire_candidates(const fact_actor_t *actor, const char *cutoff_iso)
{
   return fm_maintenance_transition(actor, 0, 0, cutoff_iso);
}
