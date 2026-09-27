/* evidence_lifecycle.c: authenticated JSON operations for the P1-P9 layer. */
#include "evidence_lifecycle.h"

#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define EL_ERR_MAX 512

typedef struct
{
   const char *sql;
   int nargs;
   int operator_only;
} el_spec_t;

static const el_spec_t *el_spec(evidence_lifecycle_op_t op)
{
   static const el_spec_t specs[] = {
       [EL_CHANGESET_SHOW] = {"SELECT knowledge_changeset_show(?1)::text", 1, 1},
       [EL_CHANGESET_DIFF] = {"SELECT knowledge_changeset_diff(?1)::text", 1, 1},
       [EL_CHANGESET_PREVIEW_REVERT] = {"SELECT knowledge_changeset_preview_revert(?1)::text", 1,
                                        1},
       [EL_CHANGESET_REVERT] = {"SELECT knowledge_changeset_revert(?1,CAST(?2 AS boolean))::text",
                                2, 1},
       [EL_DOCUMENT_PREVIEW] = {"SELECT document_lifecycle_preview(CAST(?1 AS bigint),?2)::text", 2,
                                1},
       [EL_DOCUMENT_APPLY] = {"SELECT document_lifecycle_apply(CAST(?1 AS bigint),?2,?3,?4)::text",
                              4, 1},
       [EL_DERIVED_STATUS] =
           {"SELECT jsonb_build_object('items',COALESCE((SELECT jsonb_agg(to_jsonb(x)) FROM ("
            " SELECT r.*,f.status AS computed_status,f.cause_kind,f.cause_id FROM"
            " derived_memory_registry r JOIN derived_memory_freshness f USING"
            " (derived_kind,derived_memory_id) WHERE (?1='' OR r.derived_kind=?1)"
            " AND (?2='' OR r.derived_memory_id=?2)) x),'[]'::jsonb),'reverse',COALESCE(("
            " SELECT jsonb_agg(to_jsonb(d)) FROM derived_memory_dependencies d WHERE"
            " (?3='' OR d.input_kind=?3) AND (?4='' OR d.input_id=?4)),'[]'::jsonb))::text",
            4, 1},
       [EL_OUTCOME_RECORD] =
           {"SELECT jsonb_build_object('outcome_id',work_outcome_record(?1,?2,?3,?4,?5,?6,"
            " ?7,?8,?9,?10,?11,?12,CAST(?13 AS bigint),?14,?15))::text",
            15, 0},
       [EL_REVIEW_LIST] =
           {"SELECT jsonb_build_object('items',COALESCE(jsonb_agg(to_jsonb(x) ORDER BY priority"
            " DESC,item_id),'[]'::jsonb),'changeset_head',evidence_current_head())::text FROM"
            " (SELECT * FROM operator_review_surface ORDER BY priority DESC,item_id"
            " LIMIT CAST(?1 AS bigint)) x",
            1, 1},
       [EL_REVIEW_DECIDE] = {"SELECT operator_review_decide(?1,?2,?3,?4,?5)::text", 5, 1},
       [EL_ONTOLOGY_EXPORT] = {"SELECT ontology_package_export()::text", 0, 1},
       [EL_ONTOLOGY_IMPORT] =
           {"SELECT jsonb_build_object('package_id',ontology_package_import(CAST(?1 AS jsonb),"
            " ?2,?3,?4))::text",
            4, 1},
       [EL_ONTOLOGY_DRY_RUN] = {"SELECT ontology_package_dry_run(?1,CAST(?2 AS boolean))::text", 2,
                                1},
       [EL_ONTOLOGY_MIGRATE] = {"SELECT ontology_package_migrate(?1,?2)::text", 2, 1},
       [EL_ONTOLOGY_REPORT] = {"SELECT ontology_package_report(?1)::text", 1, 1},
       [EL_ONTOLOGY_ROLLBACK] = {"SELECT ontology_package_rollback()::text", 0, 1},
       [EL_RECALL_TRACE_RECORD] = {"SELECT recall_trace_record(?1,?2,?3,?4,?5,?6,CAST(?7 AS jsonb),"
                                   " CAST(?8 AS boolean))::text",
                                   8, 0},
       [EL_RECALL_TRACE_GET] = {"SELECT recall_trace_get(?1,?2,?3)::text", 3, 0},
   };
   if (op <= 0 || op >= (int)(sizeof(specs) / sizeof(specs[0])) || !specs[op].sql)
      return NULL;
   return &specs[op];
}

static int el_set_context(void *conn, const fact_actor_t *actor, char *err, size_t err_cap)
{
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT set_config('aimee.principal',?1,true),set_config('aimee.authority',?2,true),"
       " set_config('aimee.transport_identity',?3,true)",
       err, err_cap);
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor->principal);
   aimee_pg_bind_text(st, "?2", actor->role);
   aimee_pg_bind_text(st, "?3",
                      actor->transport_identity[0] ? actor->transport_identity : "internal");
   int ok = aimee_pg_step(st, err, err_cap) == AIMEE_PG_ROW;
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

int db2_evidence_lifecycle_json(const fact_actor_t *actor, evidence_lifecycle_op_t op,
                                const char *const *args, int nargs, char *out, int out_cap)
{
   if (!actor || !actor->principal[0] || !actor->role[0] || !out || out_cap < 3)
      return -1;
   out[0] = '\0';
   const el_spec_t *spec = el_spec(op);
   if (!spec || nargs != spec->nargs || nargs > EL_MAX_ARGS || (nargs && !args) ||
       (spec->operator_only && actor->rank != FACT_ACTOR_OPERATOR))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[EL_ERR_MAX] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0 ||
       el_set_context(conn, actor, err, sizeof(err)) != 0)
      goto rollback;
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, spec->sql, err, sizeof(err));
   if (!st)
      goto rollback;
   for (int i = 0; i < nargs; i++)
   {
      char name[8];
      snprintf(name, sizeof(name), "?%d", i + 1);
      aimee_pg_bind_text(st, name, args[i] ? args[i] : "");
   }
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW;
   const char *json = ok ? aimee_pg_column_text(st, 0) : NULL;
   if (!json || strlen(json) >= (size_t)out_cap)
      ok = 0;
   else
      snprintf(out, (size_t)out_cap, "%s", json);
   aimee_pg_finalize(st);
   if (!ok || aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      goto rollback;
   return 0;

rollback:
   (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   out[0] = '\0';
   return -1;
}

int db2_work_outcomes_for_retrieval_json(const char *retrieval_event_id, char *out, int out_cap)
{
   if (!retrieval_event_id || !retrieval_event_id[0] || !out || out_cap < 3)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[EL_ERR_MAX] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT COALESCE(jsonb_agg(to_jsonb(o) ORDER BY o.occurred_at,o.outcome_id),"
       " '[]'::jsonb)::text FROM work_outcomes o WHERE o.retrieval_event_id=?1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", retrieval_event_id);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW;
   const char *json = ok ? aimee_pg_column_text(st, 0) : NULL;
   if (!json || strlen(json) >= (size_t)out_cap)
      ok = 0;
   else
      snprintf(out, (size_t)out_cap, "%s", json);
   aimee_pg_finalize(st);
   if (!ok)
      out[0] = '\0';
   return ok ? 0 : -1;
}
