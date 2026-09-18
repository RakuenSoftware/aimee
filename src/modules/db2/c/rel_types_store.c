/* rel_types_store.c: DB2 persistence + commit path for the typed-fact ontology
 * (typed-fact §1 / P1b). See rel_types_store.h. */
#include "../headers/aimee.h" /* edge_t (used by entity_edges.h) */
#include "rel_types_store.h"
#include "../headers/rel_types.h"
#include "entity_edges.h"
#include "fact_mutation.h"
#include "entity_registry.h"    /* db2_entity_register_named (§3 endpoint resolution) */
#include "ontology_evolution.h" /* db2_ontology_eval_observe (§2 / P4) */
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define RTS_ERRBUF 256
/* Entity endpoint / alias name width — matches entity_aliases.name and the
 * edge source/target columns (db2_entity_aliases_for writes into char[128]). */
#define FACT_ENDPOINT_MAX 128

static db2_fact_gate_fn g_fact_gate_provider;

void aimee_db2_register_fact_gate_provider(db2_fact_gate_fn provider)
{
   g_fact_gate_provider = provider;
}

int db2_rel_types_resolve(const char *rel_type, long *out_id)
{
   if (!rel_type || !rel_type[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   if (!norm[0])
      return 0;
   static const char *sql = "SELECT id FROM rel_types WHERE rel_type = ?1 LIMIT 1";
   char err[RTS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (out_id)
         *out_id = (long)aimee_pg_column_int64(st, 0);
      found = 1;
   }
   aimee_pg_finalize(st);
   return found;
}

/* §1: is `rel_type` an ACTIVE relation in the live ontology table? A promoted
 * (§2) or operator-added type is known even though it is not in the in-code seed;
 * the gate (seed-only) returns NOVEL for it, so the commit path consults this to
 * avoid re-staging an already-active type as provisional. Returns 1 if active,
 * 0 if not (or only provisional), -1 on DB error. */
int db2_rel_types_active(const char *rel_type)
{
   if (!rel_type || !rel_type[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   if (!norm[0])
      return 0;
   static const char *sql =
       "SELECT 1 FROM rel_types WHERE rel_type = ?1 AND status = 'active' LIMIT 1";
   char err[RTS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   int active = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return active;
}

long db2_rel_types_stage_provisional(const char *rel_type)
{
   if (!rel_type || !rel_type[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   if (!norm[0])
      return -1;
   /* A novel type defaults to the restrictive sensitivity (fail closed, §7). */
   static const char *sql = "INSERT INTO rel_types (rel_type, status, sensitivity)"
                            " VALUES (?1, 'provisional', 'pii') ON CONFLICT (rel_type) DO NOTHING";
   char err[RTS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   long id = 0;
   return db2_rel_types_resolve(norm, &id) == 1 ? id : -1;
}

/* Entity-kind endpoints (people/places/devices/orgs/ips) are canonicalized
 * through the registry (§3) so aliased names ("DevBox" / "the workstation")
 * collapse to one node's preferred name; scalar/other values are stored verbatim
 * (an age or IP literal is not an entity to canonicalize). */
static int fact_kind_is_entity(memory_node_kind_t k)
{
   switch (k)
   {
   case NODE_PERSON:
   case NODE_PLACE:
   case NODE_DEVICE:
   case NODE_ORG:
   case NODE_IP:
      return 1;
   default:
      return 0;
   }
}

static void fact_canonical_endpoint(const char *in, memory_node_kind_t kind, char *out, size_t cap)
{
   if (fact_kind_is_entity(kind))
   {
      int64_t cid = db2_entity_register_named(in, (int)kind); /* get-or-create */
      char names[1][FACT_ENDPOINT_MAX];
      if (cid > 0 && db2_entity_aliases_for(cid, names, 1) == 1 && names[0][0])
      {
         snprintf(out, cap, "%s", names[0]); /* canonical (preferred) name */
         return;
      }
   }
   snprintf(out, cap, "%s", in); /* scalar/other, or registry unavailable */
}

fact_gate_verdict_t db2_fact_commit_with_actor(const char *source, memory_node_kind_t head_kind,
                                               const char *rel_type, const char *target,
                                               memory_node_kind_t tail_kind,
                                               const fact_actor_t *actor, int enabled,
                                               const fact_evidence_input_t *evidence,
                                               const char *assertion_kind, const char *valid_from,
                                               const char *valid_until)
{
   int verdict = -1;
   int commit_allowed = -1;
   if (!g_fact_gate_provider ||
       g_fact_gate_provider((int)head_kind, rel_type, (int)tail_kind, &verdict, &commit_allowed) !=
           0 ||
       (commit_allowed != 0 && commit_allowed != 1) || verdict < DB2_FACT_GATE_ACCEPT ||
       verdict > DB2_FACT_GATE_BADARG)
      verdict = -1;
   fact_gate_verdict_t v = FACT_GATE_DEFER;
   switch (verdict)
   {
   case DB2_FACT_GATE_ACCEPT:
      v = FACT_GATE_ACCEPT;
      break;
   case DB2_FACT_GATE_REJECT_KIND:
      v = FACT_GATE_REJECT_KIND;
      break;
   case DB2_FACT_GATE_NOVEL:
      v = FACT_GATE_NOVEL;
      break;
   case DB2_FACT_GATE_BADARG:
      v = FACT_GATE_BADARG;
      break;
   }
   if (!enabled)
      return v; /* observe-only when the feature flag is off */
   if (!source || !source[0] || !target || !target[0])
      return v;

   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));

   if (v != FACT_GATE_ACCEPT && v != FACT_GATE_NOVEL)
      return v; /* REJECT_KIND / BADARG: never write an unvalidated semantic edge */

   /* The Go write decision includes credential withholding. A missing or
    * malformed decision already deferred above; no native PII callback remains. */
   if (!commit_allowed)
      return FACT_GATE_REJECT_SENSITIVE;

   /* §5: provenance-keyed class. user -> A, model+ACCEPT -> B, model NOVEL -> C. */
   fact_authority_t authority =
       actor && actor->rank >= FACT_ACTOR_USER ? FACT_AUTHORITY_USER : FACT_AUTHORITY_MODEL;
   const char *cls = fact_class_for(authority, v);
   double conf = fact_class_confidence(cls);

   /* §3: canonicalize entity-kind endpoints so aliased names share one node. */
   char csrc[FACT_ENDPOINT_MAX], ctgt[FACT_ENDPOINT_MAX];
   fact_canonical_endpoint(source, head_kind, csrc, sizeof(csrc));
   fact_canonical_endpoint(target, tail_kind, ctgt, sizeof(ctgt));

   /* §1: a type the seed-only gate calls NOVEL may already be ACTIVE in the live
    * ontology (promoted §2 or operator-added). Treat that as known — commit it
    * normally with an ACCEPT-derived class — instead of re-staging it provisional
    * (which would wrongly demote a promoted type's facts to Class C every write). */
   int live_known = (v == FACT_GATE_NOVEL) && db2_rel_types_active(norm) == 1;
   if (live_known)
   {
      cls = fact_class_for(authority, FACT_GATE_ACCEPT);
      conf = fact_class_confidence(cls);
   }

   if (v == FACT_GATE_ACCEPT || live_known)
   {
      long id = 0;
      if (db2_rel_types_resolve(norm, &id) != 1)
         /* validated, but the relation_id couldn't be resolved (ontology not
          * seeded / DB issue): DEFER — never write unresolved, and never report
          * success or (for a live-known NOVEL) leave the caller thinking it should
          * still stage a provisional. */
         return FACT_GATE_DEFER;
      /* §1: a failed write must NOT be reported as success — return DEFER so the
       * caller retries rather than believing the fact was committed. */
      if (!actor)
         return FACT_GATE_DEFER;
      fact_assertion_input_t input = {.source = csrc,
                                      .relation = norm,
                                      .target = ctgt,
                                      .relation_id = (int)id,
                                      .subject_kind = (int)head_kind,
                                      .object_kind = (int)tail_kind,
                                      .confidence_class = cls,
                                      .confidence = conf,
                                      .assertion_kind = assertion_kind,
                                      .valid_from = valid_from,
                                      .valid_until = valid_until,
                                      .evidence = evidence};
      if (db2_fact_mutation_assert(actor, &input, NULL) != 0)
         return FACT_GATE_DEFER;
      return FACT_GATE_ACCEPT;
   }
   /* FACT_GATE_NOVEL (and not active in the live table): stage as provisional so
    * the edge's relation_id resolves; no kind validation for a novel type — that
    * is promotion's job (§2). cls is already Class C here (fact_class_for maps
    * NOVEL -> C regardless of authority). */
   long id = db2_rel_types_stage_provisional(norm);
   if (id <= 0)
      return FACT_GATE_DEFER; /* could not stage (DB issue) — defer, do not drop */
   if (!actor)
      return FACT_GATE_DEFER;
   fact_assertion_input_t input = {.source = csrc,
                                   .relation = norm,
                                   .target = ctgt,
                                   .relation_id = (int)id,
                                   .subject_kind = (int)head_kind,
                                   .object_kind = (int)tail_kind,
                                   .confidence_class = cls,
                                   .confidence = conf,
                                   .assertion_kind = assertion_kind,
                                   .valid_from = valid_from,
                                   .valid_until = valid_until,
                                   .evidence = evidence};
   if (db2_fact_mutation_assert(actor, &input, NULL) != 0)
      return FACT_GATE_DEFER;
   /* §2: count the sighting only after the edge actually committed, so a failed
    * write doesn't inflate the promotion occurrence count. */
   (void)db2_ontology_eval_observe(norm);
   return v;
}

fact_gate_verdict_t db2_fact_commit_with_evidence(const char *source, memory_node_kind_t head_kind,
                                                  const char *rel_type, const char *target,
                                                  memory_node_kind_t tail_kind,
                                                  fact_authority_t authority, int enabled,
                                                  const fact_evidence_input_t *evidence,
                                                  const char *assertion_kind,
                                                  const char *valid_from, const char *valid_until)
{
   fact_actor_t actor;
   if (db2_fact_actor_internal(
           authority == FACT_AUTHORITY_USER ? FACT_ACTOR_USER : FACT_ACTOR_MODEL, &actor) != 0)
      return FACT_GATE_DEFER;
   return db2_fact_commit_with_actor(source, head_kind, rel_type, target, tail_kind, &actor,
                                     enabled, evidence, assertion_kind, valid_from, valid_until);
}

fact_gate_verdict_t db2_fact_commit(const char *source, memory_node_kind_t head_kind,
                                    const char *rel_type, const char *target,
                                    memory_node_kind_t tail_kind, fact_authority_t authority,
                                    int enabled)
{
   return db2_fact_commit_with_evidence(source, head_kind, rel_type, target, tail_kind, authority,
                                        enabled, NULL, FACT_KIND_WORLD_FACT, NULL, NULL);
}
