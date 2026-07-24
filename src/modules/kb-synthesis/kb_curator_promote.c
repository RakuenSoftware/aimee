/* kb_curator_promote.c: deep-curator promote_entity pass. See header.
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_promote.h"
#include "aimee.h"
#include "config.h"
#include "cJSON.h"
#include "log.h"
#include "db2/artifacts.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"

#include <stdlib.h>
#include <string.h>

#define CURATOR_PROMOTE_DEFAULT_MIN_SOURCES 3
/* Max distinct citing sources whose `mentions` we carry onto the promoted
 * entity in a single pass. A hard cap keeps the per-job work bounded. */
#define CURATOR_PROMOTE_MAX_CARRY 256

const char *kb_curator_scope_next_kind(const char *kind)
{
   if (!kind)
      return NULL;
   if (strcmp(kind, "project") == 0)
      return "workspace";
   if (strcmp(kind, "workspace") == 0)
      return "global";
   return NULL; /* global / unknown: already at (or off) the top */
}

/* Resolve the broader scope for (kind, id). project -> its owning workspace
 * (from the projects table) when known, else straight to global; workspace ->
 * global. Writes out_kind/out_id; returns 1 if a broader scope exists, 0 if
 * already global / unknown. */
int kb_curator_broaden_scope(void *conn, const char *kind, const char *id, char *out_kind,
                             size_t kn, char *out_id, size_t in)
{
   const char *next = kb_curator_scope_next_kind(kind);
   if (!next)
      return 0;

   if (strcmp(next, "global") == 0)
   {
      snprintf(out_kind, kn, "global");
      snprintf(out_id, in, "%s", "");
      return 1;
   }

   /* next == "workspace": look up the project's owning workspace by name. */
   snprintf(out_kind, kn, "%s", next);
   snprintf(out_id, in, "%s", ""); /* default: workspace unknown */
   static const char *sql = "SELECT workspace FROM projects WHERE name = :name LIMIT 1";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (st)
   {
      aimee_pg_bind_text(st, "name", id ? id : "");
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *ws = aimee_pg_column_text(st, 0);
         if (ws && ws[0])
            snprintf(out_id, in, "%s", ws);
      }
      aimee_pg_finalize(st);
   }
   /* If the project has no recorded workspace, promote straight to global so
    * the entity is not stranded at a workspace scope with no id. */
   if (!out_id[0])
   {
      snprintf(out_kind, kn, "global");
   }
   return 1;
}

/* Pick the highest-evidence promotion candidate: a committed, non-global,
 * not-yet-superseded entity cited by >= `min_sources` distinct sources. */
int kb_curator_promote_pick(void *conn, int min_sources, char *id, size_t id_len,
                            char **payload_out, char *scope_kind, size_t sk_len, char *scope_id,
                            size_t si_len)
{
   static const char *sql =
       "SELECT a.id, a.payload, a.scope_kind, a.scope_id, COUNT(DISTINCT l.from_id) AS srcs"
       " FROM artifacts a"
       " JOIN artifact_links l ON l.to_id = a.id AND l.kind = 'mentions'"
       " WHERE a.kind = 'entity' AND a.state = 'committed' AND a.scope_kind <> 'global'"
       "   AND NOT EXISTS ("
       "     SELECT 1 FROM artifact_links sp WHERE sp.from_id = a.id AND sp.kind = 'supersedes')"
       " GROUP BY a.id, a.payload, a.scope_kind, a.scope_id"
       " HAVING COUNT(DISTINCT l.from_id) >= :minsrc"
       " ORDER BY srcs DESC, a.id ASC"
       " LIMIT 1";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "minsrc", min_sources);
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c_id = aimee_pg_column_text(st, 0);
      const char *c_pl = aimee_pg_column_text(st, 1);
      const char *c_sk = aimee_pg_column_text(st, 2);
      const char *c_si = aimee_pg_column_text(st, 3);
      snprintf(id, id_len, "%s", c_id ? c_id : "");
      *payload_out = strdup(c_pl ? c_pl : "{}");
      snprintf(scope_kind, sk_len, "%s", c_sk ? c_sk : "");
      snprintf(scope_id, si_len, "%s", c_si ? c_si : "");
      found = id[0] ? 1 : 0;
   }
   aimee_pg_finalize(st);
   return found;
}

/* Carry the old entity's distinct `mentions` sources onto the new entity, so
 * the promoted entity inherits the evidence (and centrality). Returns the count
 * carried. */
static int promote_carry_mentions(void *conn, const char *old_id, const char *new_id)
{
   static const char *sql = "SELECT DISTINCT from_id FROM artifact_links"
                            " WHERE to_id = :old AND kind = 'mentions'"
                            " ORDER BY from_id LIMIT :cap";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "old", old_id);
   aimee_pg_bind_int(st, "cap", CURATOR_PROMOTE_MAX_CARRY);

   char(*froms)[64] = calloc(CURATOR_PROMOTE_MAX_CARRY, sizeof(*froms));
   int n = 0;
   if (froms)
   {
      while (n < CURATOR_PROMOTE_MAX_CARRY && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *f = aimee_pg_column_text(st, 0);
         if (f && f[0])
            snprintf(froms[n++], sizeof(froms[0]), "%s", f);
      }
   }
   aimee_pg_finalize(st);

   for (int i = 0; i < n; i++)
      db2_artifact_link(froms[i], new_id, "mentions");
   free(froms);
   return n;
}

int kb_curator_promote_entity_one(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.kb_curator_promote_entity_enabled)
      return 0;
   int min_sources = cfg.kb_curator_promote_min_sources > 0 ? cfg.kb_curator_promote_min_sources
                                                            : CURATOR_PROMOTE_DEFAULT_MIN_SOURCES;

   char old_id[64] = "", scope_kind[64] = "", scope_id[128] = "";
   char *payload = NULL;
   if (!kb_curator_promote_pick(conn, min_sources, old_id, sizeof(old_id), &payload, scope_kind,
                                sizeof(scope_kind), scope_id, sizeof(scope_id)))
   {
      free(payload);
      return 0;
   }

   char new_kind[64], new_id_scope[128];
   if (!kb_curator_broaden_scope(conn, scope_kind, scope_id, new_kind, sizeof(new_kind),
                                 new_id_scope, sizeof(new_id_scope)))
   {
      /* Already global / unknown — should not happen (we filter non-global),
       * but guard so a stray row is not retried forever: nothing to do. */
      free(payload);
      return 0;
   }

   /* Build the promoted entity payload: the old payload + promotion provenance. */
   cJSON *pj = payload ? cJSON_Parse(payload) : NULL;
   if (!pj)
      pj = cJSON_CreateObject();
   cJSON_DeleteItemFromObjectCaseSensitive(pj, "promoted_from");
   cJSON_AddStringToObject(pj, "promoted_from", old_id);
   cJSON_AddStringToObject(pj, "promoted_from_scope_kind", scope_kind);
   cJSON_AddStringToObject(pj, "promoted_from_scope_id", scope_id);
   char *new_payload = cJSON_PrintUnformatted(pj);
   cJSON_Delete(pj);

   char new_id[64];
   db2_artifact_gen_id(new_id, sizeof(new_id));
   int rc = db2_artifact_write(new_id, "entity", "committed", new_kind, new_id_scope, "curator",
                               0.0, new_payload ? new_payload : "{}");
   free(new_payload);
   free(payload);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "kb.curator.promote", "failed to write promoted entity for %s", old_id);
      return 0;
   }

   /* Wire old -> new, carry evidence, audit, and retire the old entity. */
   db2_artifact_link(old_id, new_id, "supersedes");
   int carried = promote_carry_mentions(conn, old_id, new_id);

   char before[256], after[256];
   snprintf(before, sizeof(before), "{\"scope_kind\":\"%s\",\"scope_id\":\"%s\"}", scope_kind,
            scope_id);
   snprintf(after, sizeof(after),
            "{\"scope_kind\":\"%s\",\"scope_id\":\"%s\",\"entity_id\":\"%s\"}", new_kind,
            new_id_scope, new_id);
   char audit_id[64];
   db2_artifact_gen_id(audit_id, sizeof(audit_id));
   db2_audit_event_write(audit_id, old_id, "kb.curator.promote_entity", new_id, "curator", new_kind,
                         new_id_scope, 0.0, 0, before, after);

   db2_artifact_set_state(old_id, "superseded");

   aimee_log(LOG_INFO, "kb.curator.promote",
             "promoted entity %s (%s:%s) -> %s (%s:%s), carried %d source(s)", old_id, scope_kind,
             scope_id, new_id, new_kind, new_id_scope, carried);
   return 1;
}
