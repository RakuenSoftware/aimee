#include "json_fluent.h"
/* db2/demotion.c: retrieval attribution evidence transport (scoring lives in Go).
 * See docs/proposals/done/outcome-driven-demotion-and-poison-resilience.md */

#include "demotion.h"
#include "artifacts.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "aimee.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Optional external host transport. The standalone storage module has no
 * command registry; unresolved memory versions retain the documented unknown
 * value. Memory policy is never supplied by DB2. */
extern int aimee_module_commands_dispatch_internal(const char *, const cJSON *, cJSON **)
    __attribute__((weak));

/* auditable-correctness P1.5 (D3): the retrieval_event carries a UNIFIED typed
 * `surfaced_refs` list — [{type,...,v}] — as the source of truth. Memory rows are
 * {type:"memory", id:<int64>, v:<updated_at>}; code refs are {type:"code",
 * ref:"code:<project>:<file_path>", v:<content_hash>}. The legacy `surfaced_ids`
 * and `surfaced_items` arrays are kept as DERIVED projections of the memory-typed
 * entries, so every existing reader (trace, provenance, demotion) is byte-identical.
 * make_memory_ref / project_memory_refs / ensure_surfaced_refs keep that invariant;
 * the writer and both merges go through them. */

/* Event-payload read buffer for the merges (~900 refs). On overflow by_turn
 * truncates → the CAS old-value can't match → -1 (fail-safe). */
#define MERGE_EVENT_PAYLOAD_CAP 65536

/* {type:"memory", id, v} — v captured now (omitted when unresolved, mirroring the
 * legacy contract: a missing version means "unknown", not drift). */
static cJSON *make_memory_ref(int64_t id)
{
   cJSON *r = cJSON_CreateObject();
   if (!r)
      return NULL;
   cJSON_AddStringToObject(r, "type", "memory");
   cJSON_AddItemToObject(r, "id", jo_i64_value_exact(id));
   cJSON *args = cJSON_CreateObject(), *reply = NULL;
   cJSON_AddStringToObject(args, "operation", "record");
   cJSON_AddItemToObject(args, "id", jo_i64_value_exact(id));
   if (aimee_module_commands_dispatch_internal &&
       aimee_module_commands_dispatch_internal("memory.runtime", args, &reply) == 1)
   {
      const cJSON *record = cJSON_GetObjectItemCaseSensitive(reply, "memory");
      const char *version =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "updated_at"));
      const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "status"));
      if (status && strcmp(status, "ok") == 0 && version && version[0])
         cJSON_AddStringToObject(r, "v", version);
   }
   cJSON_Delete(reply);
   cJSON_Delete(args);
   return r;
}

/* Regenerate surfaced_ids + surfaced_items from the memory-typed entries of
 * surfaced_refs (the unified source of truth), so the back-compat projections
 * always match after any mutation. */
static void project_memory_refs(cJSON *p)
{
   cJSON *refs = cJSON_GetObjectItemCaseSensitive(p, "surfaced_refs");
   if (!cJSON_IsArray(refs))
      return; /* nothing to project from — leave any existing projections intact */
   /* Build the replacements as DETACHED arrays first, then swap them in only once
    * both exist — so an allocation failure never leaves the event with the old
    * projections deleted and no replacements (no destruction window). */
   cJSON *ids = cJSON_CreateArray();
   cJSON *items = cJSON_CreateArray();
   if (!ids || !items)
   {
      cJSON_Delete(ids);
      cJSON_Delete(items);
      return;
   }
   int n = cJSON_GetArraySize(refs);
   for (int i = 0; i < n; i++)
   {
      cJSON *r = cJSON_GetArrayItem(refs, i);
      cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "type");
      cJSON *idj = cJSON_GetObjectItemCaseSensitive(r, "id");
      int64_t id;
      if (!cJSON_IsString(t) || strcmp(t->valuestring, "memory") != 0 ||
          !jo_read_i64_exact(idj, &id))
         continue;
      cJSON_AddItemToArray(ids, jo_i64_value_exact(id));
      cJSON *it = cJSON_CreateObject();
      cJSON_AddItemToObject(it, "id", jo_i64_value_exact(id));
      cJSON *v = cJSON_GetObjectItemCaseSensitive(r, "v");
      if (cJSON_IsString(v) && v->valuestring)
         cJSON_AddStringToObject(it, "v", v->valuestring);
      cJSON_AddItemToArray(items, it);
   }
   cJSON_DeleteItemFromObjectCaseSensitive(p, "surfaced_ids");
   cJSON_DeleteItemFromObjectCaseSensitive(p, "surfaced_items");
   cJSON_AddItemToObject(p, "surfaced_ids", ids);
   cJSON_AddItemToObject(p, "surfaced_items", items);
}

/* Return p's surfaced_refs array, back-filling it from the legacy surfaced_ids/
 * surfaced_items of an event written before the unified model (migration-on-read).
 * Returns NULL only on allocation failure. */
static cJSON *ensure_surfaced_refs(cJSON *p)
{
   cJSON *refs = cJSON_GetObjectItemCaseSensitive(p, "surfaced_refs");
   if (cJSON_IsArray(refs))
   {
      const cJSON *row;
      cJSON_ArrayForEach(row, refs)
      {
         int64_t id;
         const cJSON *type = cJSON_GetObjectItemCaseSensitive(row, "type");
         if (cJSON_IsString(type) && !strcmp(type->valuestring, "memory") &&
             !jo_read_i64_exact(cJSON_GetObjectItemCaseSensitive(row, "id"), &id))
            return NULL; /* Cannot recover a previously rounded source identity. */
      }
      return refs;
   }
   refs = cJSON_AddArrayToObject(p, "surfaced_refs");
   if (!refs)
      return NULL;
   cJSON *ids = cJSON_GetObjectItemCaseSensitive(p, "surfaced_ids");
   cJSON *items = cJSON_GetObjectItemCaseSensitive(p, "surfaced_items");
   int n = cJSON_IsArray(ids) ? cJSON_GetArraySize(ids) : 0;
   for (int i = 0; i < n; i++)
   {
      cJSON *e = cJSON_GetArrayItem(ids, i);
      int64_t id;
      if (!jo_read_i64_exact(e, &id))
         return NULL;
      cJSON *r = cJSON_CreateObject();
      if (!r)
         continue;
      cJSON_AddStringToObject(r, "type", "memory");
      cJSON_AddItemToObject(r, "id", jo_i64_value_exact(id));
      if (cJSON_IsArray(items)) /* preserve the legacy point-in-time v */
      {
         int m = cJSON_GetArraySize(items);
         for (int j = 0; j < m; j++)
         {
            cJSON *it = cJSON_GetArrayItem(items, j);
            cJSON *iid = it ? cJSON_GetObjectItemCaseSensitive(it, "id") : NULL;
            int64_t item_id;
            if (jo_read_i64_exact(iid, &item_id) && item_id == id)
            {
               cJSON *v = cJSON_GetObjectItemCaseSensitive(it, "v");
               if (cJSON_IsString(v) && v->valuestring)
                  cJSON_AddStringToObject(r, "v", v->valuestring);
               break;
            }
         }
      }
      cJSON_AddItemToArray(refs, r);
   }
   return refs;
}

/* Compare-and-swap the event payload: land newpayload only if the row's payload
 * still equals oldpayload (no concurrent change). Returns 1 if updated, 0 if no row
 * matched (a concurrent merge changed it, or the event was deleted), -1 on error.
 * Shared by both merges so the CAS retry contract is identical. */
static int cas_update_event_payload(void *conn, const char *ev_id, const char *oldpayload,
                                    const char *newpayload)
{
   if (!conn || !ev_id || !oldpayload || !newpayload)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE artifacts SET payload = ?1 WHERE id = ?2 AND payload = ?3", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", newpayload);
   aimee_pg_bind_text(st, "?2", ev_id);
   aimee_pg_bind_text(st, "?3", oldpayload);
   int step = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_DONE)
      return -1;
   return changes > 0 ? 1 : 0;
}

int db2_demotion_retrieval_event_write(const char *query_fingerprint, const char *role,
                                       const int64_t *surfaced_ids, int n_surfaced, char *id_out,
                                       int id_out_len)
{
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   /* Build the unified surfaced_refs (all memory at create), then derive the
    * back-compat surfaced_ids/surfaced_items projections from it (D3/P1.5). */
   cJSON *p = cJSON_CreateObject();
   if (!p)
      return -1;
   cJSON_AddStringToObject(p, "query_fingerprint", query_fingerprint ? query_fingerprint : "");
   cJSON_AddStringToObject(p, "role", role ? role : "");
   cJSON *refs = cJSON_AddArrayToObject(p, "surfaced_refs");
   /* surfaced_ids may be NULL (proactive/no-surface events); guard so n is 0. */
   for (int i = 0; surfaced_ids && refs && i < n_surfaced; i++)
   {
      cJSON *r = make_memory_ref(surfaced_ids[i]);
      if (r)
         cJSON_AddItemToArray(refs, r);
   }
   project_memory_refs(p);
   char *payload = cJSON_PrintUnformatted(p);
   cJSON_Delete(p);
   if (!payload)
      return -1;

   int rc = db2_artifact_write(id, "retrieval_event", "proposed", "system", "", "", 1.0, payload);
   free(payload);
   if (rc != 0)
      return -1;

   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);
   return 0;
}

int db2_demotion_retrieval_event_write_turn(const char *turn_id, const char *query_fingerprint,
                                            const char *role, const int64_t *surfaced_ids,
                                            int n_surfaced, char *id_out, int id_out_len)
{
   char id[64];
   if (db2_demotion_retrieval_event_write(query_fingerprint, role, surfaced_ids, n_surfaced, id,
                                          sizeof(id)) != 0)
      return -1;

   /* Stamp the caller-visible turn_id (single follow-up UPDATE, like the
    * attribution writer stamps model_version). NOTE: the INSERT + this UPDATE are
    * not one transaction — a crash between them leaves a NULL-stamped event,
    * recoverable on the turn's retry (first-wins preserved). On a duplicate
    * turn_id the partial unique index makes this UPDATE fail; we then return the
    * AUTHORITATIVE event's id (so callers attribute to the reachable event, not
    * this orphan) — the closest P1 gets to the P1.5 idempotent merge. */
   if (turn_id && turn_id[0])
   {
      void *conn = db2_conn();
      if (conn)
      {
         char err[256] = "";
         aimee_pg_stmt_t *st = aimee_pg_prepare(
             conn, "UPDATE artifacts SET turn_id = ?1 WHERE id = ?2", err, sizeof(err));
         if (st)
         {
            aimee_pg_bind_text(st, "?1", turn_id);
            aimee_pg_bind_text(st, "?2", id);
            int rc = aimee_pg_step(st, err, sizeof(err));
            aimee_pg_finalize(st);
            if (rc != AIMEE_PG_DONE) /* duplicate turn_id (unique conflict) */
            {
               char auth[64];
               if (db2_demotion_retrieval_event_by_turn(turn_id, auth, sizeof(auth), NULL, 0) == 1)
               {
                  if (id_out && id_out_len > 0)
                     snprintf(id_out, (size_t)id_out_len, "%s", auth);
                  return 0;
               }
            }
         }
      }
   }

   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);
   return 0;
}

int db2_demotion_retrieval_event_by_turn(const char *turn_id, char *id_out, int id_out_len,
                                         char *payload_out, int payload_out_len)
{
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';
   if (payload_out && payload_out_len > 0)
      payload_out[0] = '\0';
   if (!turn_id || !turn_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, payload FROM artifacts"
                        " WHERE kind = 'retrieval_event' AND turn_id = ?1 LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", turn_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int found = 0;
   if (rc == AIMEE_PG_ROW)
   {
      const char *id = aimee_pg_column_text(st, 0);
      const char *payload = aimee_pg_column_text(st, 1);
      id = id ? id : "";
      payload = payload ? payload : "";
      size_t id_bytes = strlen(id), payload_bytes = strlen(payload);
      if ((id_out && (id_out_len <= 0 || id_bytes >= (size_t)id_out_len)) ||
          (payload_out && (payload_out_len <= 0 || payload_bytes >= (size_t)payload_out_len)))
         found = -1; /* Never label a truncated event or identity as a complete trace. */
      else
      {
         found = 1;
         if (id_out)
            memcpy(id_out, id, id_bytes + 1);
         if (payload_out)
            memcpy(payload_out, payload, payload_bytes + 1);
      }
   }
   aimee_pg_finalize(st);
   /* Distinguish a DB error (-1) from a genuine no-event (0): /v1/audit/trace
    * must report evidence_unavailable on failure, never a falsely-empty trace. */
   if (rc == AIMEE_PG_ERR)
      return -1;
   return found;
}

int db2_demotion_retrieval_event_merge_turn(const char *turn_id, const char *query_fingerprint,
                                            const char *role, const int64_t *surfaced_ids,
                                            int n_surfaced, char *id_out, int id_out_len)
{
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';
   if (!turn_id || !turn_id[0])
      return -1;

   /* Optional external host transport. The standalone storage module has no
    * command registry; unresolved memory versions retain the documented unknown
    * value. Memory policy is never supplied by DB2. */
   extern int aimee_module_commands_dispatch_internal(const char *, const cJSON *, cJSON **)
       __attribute__((weak));

   /* auditable-correctness P1.5 (D14): the two-writer idempotent merge. A turn's
    * retrieval_event may be contributed to by more than one surface (the memory
    * recall AND the code-search surface). The first writer creates the event; any
    * later writer MERGES its surfaced refs into that same event rather than being
    * dropped (the plain write_turn dup path only returns the existing id). Dedup by
    * id makes it idempotent — re-merging the same refs is a no-op.
    *
    * Concurrency: a compare-and-swap retry loop (budget of 5 attempts; on exhaustion
    * returns -1). The UPDATE lands only if the row's payload still equals what we
    * read; a concurrent merge (or the event being deleted) yields 0 rows-affected,
    * so we re-read and retry. The CREATE path also loops: after write_turn we
    * `continue` and re-merge, so even if we lost a create race (write_turn wrote an
    * un-stamped orphan and returned the winner's id) our refs still land in the
    * canonical event on the next pass. Portable across Postgres and the sqlite shim
    * (no FOR UPDATE / jsonb needed). */
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* One heap buffer reused across retries (~900 refs); avoids a large per-iteration
    * stack frame. If a payload ever exceeds it, by_turn truncates → the CAS old-value
    * can't match → retries exhaust → -1 (fail-safe, never a silent partial write). */
   char *payload = malloc(MERGE_EVENT_PAYLOAD_CAP);
   if (!payload)
      return -1;

   int result = -1;
   for (int attempt = 0; attempt < 5; attempt++)
   {
      char ev_id[64] = "";
      payload[0] = '\0';
      int rc = db2_demotion_retrieval_event_by_turn(turn_id, ev_id, sizeof(ev_id), payload,
                                                    MERGE_EVENT_PAYLOAD_CAP);
      if (rc < 0)
      {
         result = -1;
         break;
      }
      if (rc == 0)
      {
         /* No event yet — create it, then re-read IN THIS iteration so the merge
          * below still runs (even on the last retry) and our refs land even if a
          * concurrent writer won the create race. */
         if (db2_demotion_retrieval_event_write_turn(turn_id, query_fingerprint, role, surfaced_ids,
                                                     n_surfaced, NULL, 0) != 0)
         {
            result = -1;
            break;
         }
         payload[0] = '\0';
         rc = db2_demotion_retrieval_event_by_turn(turn_id, ev_id, sizeof(ev_id), payload,
                                                   MERGE_EVENT_PAYLOAD_CAP);
         if (rc != 1) /* created but not readable (raced away) — fail this call */
         {
            result = -1;
            break;
         }
      }

      cJSON *p = payload[0] ? cJSON_Parse(payload) : NULL;
      if (!p)
      {
         result = -1; /* present row, unparseable payload — surface the error */
         break;
      }
      /* Merge into the unified surfaced_refs (migrating a legacy event on read). */
      cJSON *refs = ensure_surfaced_refs(p);
      if (!refs)
      {
         cJSON_Delete(p);
         result = -1;
         break;
      }

      int added = 0;
      int oom = 0;
      for (int i = 0; surfaced_ids && i < n_surfaced; i++)
      {
         int64_t id = surfaced_ids[i];
         if (id <= 0) /* non-positive ids are ignored (documented in the header) */
            continue;
         /* dedup: skip memory refs already on the event (idempotency). Only memory-
          * typed entries are compared, so a code ref never falsely matches. */
         int present = 0, m = cJSON_GetArraySize(refs);
         for (int j = 0; j < m; j++)
         {
            cJSON *r = cJSON_GetArrayItem(refs, j);
            cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "type");
            cJSON *idj = cJSON_GetObjectItemCaseSensitive(r, "id");
            int64_t existing_id;
            if (cJSON_IsString(t) && strcmp(t->valuestring, "memory") == 0 &&
                jo_read_i64_exact(idj, &existing_id) && existing_id == id)
            {
               present = 1;
               break;
            }
         }
         if (present)
            continue;
         cJSON *r = make_memory_ref(id);
         if (!r) /* allocation failure — abort rather than silently drop a ref */
         {
            oom = 1;
            break;
         }
         cJSON_AddItemToArray(refs, r);
         added++;
      }
      if (oom)
      {
         cJSON_Delete(p);
         result = -1;
         break;
      }

      if (added > 0)
         project_memory_refs(p); /* keep the back-compat projections in sync */

      if (added == 0)
      {
         /* All refs already present — idempotent no-op, no write needed. */
         cJSON_Delete(p);
         if (id_out && id_out_len > 0)
            snprintf(id_out, (size_t)id_out_len, "%s", ev_id);
         result = 0;
         break;
      }

      char *newpayload = cJSON_PrintUnformatted(p);
      cJSON_Delete(p);
      if (!newpayload)
      {
         result = -1;
         break;
      }

      /* CAS: land the update only if the payload is still the value we merged from. */
      int cas = cas_update_event_payload(conn, ev_id, payload, newpayload);
      free(newpayload);
      if (cas < 0)
      {
         result = -1;
         break;
      }
      if (cas > 0)
      {
         if (id_out && id_out_len > 0)
            snprintf(id_out, (size_t)id_out_len, "%s", ev_id);
         result = 0;
         break;
      }
      /* 0 rows: concurrent merge or the event vanished — re-read and retry. */
   }
   free(payload);
   return result;
}

int db2_demotion_retrieval_event_merge_refs_turn(const char *turn_id, const char *query_fingerprint,
                                                 const char *role, const char *const *types,
                                                 const char *const *refs_in,
                                                 const char *const *versions, int n_refs,
                                                 char *id_out, int id_out_len)
{
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';
   if (!turn_id || !turn_id[0])
      return -1;

   /* Optional external host transport. The standalone storage module has no
    * command registry; unresolved memory versions retain the documented unknown
    * value. Memory policy is never supplied by DB2. */
   extern int aimee_module_commands_dispatch_internal(const char *, const cJSON *, cJSON **)
       __attribute__((weak));

   /* auditable-correctness P1.5 (D3/D14): merge TYPED refs ({type, ref, v}, e.g.
    * code:<project>:<file_path> with v=content_hash) into the turn's unified
    * surfaced_refs, deduped by (type, ref). Same CAS retry + create-then-merge as
    * the int64 merge; the create path reuses write_turn to make a bare turn event
    * (with dup-race handling), after which the typed refs merge on the next pass. */
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char *payload = malloc(MERGE_EVENT_PAYLOAD_CAP);
   if (!payload)
      return -1;

   int result = -1;
   for (int attempt = 0; attempt < 5; attempt++)
   {
      char ev_id[64] = "";
      payload[0] = '\0';
      int rc = db2_demotion_retrieval_event_by_turn(turn_id, ev_id, sizeof(ev_id), payload,
                                                    MERGE_EVENT_PAYLOAD_CAP);
      if (rc < 0)
      {
         result = -1;
         break;
      }
      if (rc == 0)
      {
         /* No event yet — create a bare turn event (reusing write_turn's dup-race
          * handling), then re-read IN THIS iteration so the typed merge below runs
          * even on the last retry. */
         if (db2_demotion_retrieval_event_write_turn(turn_id, query_fingerprint, role, NULL, 0,
                                                     NULL, 0) != 0)
         {
            result = -1;
            break;
         }
         payload[0] = '\0';
         rc = db2_demotion_retrieval_event_by_turn(turn_id, ev_id, sizeof(ev_id), payload,
                                                   MERGE_EVENT_PAYLOAD_CAP);
         if (rc != 1) /* created but not readable (raced away) — fail this call */
         {
            result = -1;
            break;
         }
      }

      cJSON *p = payload[0] ? cJSON_Parse(payload) : NULL;
      if (!p)
      {
         result = -1;
         break;
      }
      cJSON *refs = ensure_surfaced_refs(p);
      if (!refs)
      {
         cJSON_Delete(p);
         result = -1;
         break;
      }

      int added = 0, oom = 0;
      for (int i = 0; i < n_refs; i++)
      {
         const char *type = types ? types[i] : NULL;
         const char *ref = refs_in ? refs_in[i] : NULL;
         if (!type || !type[0] || !ref || !ref[0]) /* require a typed identity */
            continue;
         /* memory refs are id-keyed and belong to merge_turn (this path dedups by
          * ref string, which a memory entry lacks) — skip them defensively. */
         if (strcmp(type, "memory") == 0)
            continue;
         /* dedup by (type, ref) — a typed ref never collides with a memory entry
          * (which has no "ref" field). Idempotent. */
         int present = 0, m = cJSON_GetArraySize(refs);
         for (int j = 0; j < m; j++)
         {
            cJSON *r = cJSON_GetArrayItem(refs, j);
            cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "type");
            cJSON *rf = cJSON_GetObjectItemCaseSensitive(r, "ref");
            if (cJSON_IsString(t) && strcmp(t->valuestring, type) == 0 && cJSON_IsString(rf) &&
                strcmp(rf->valuestring, ref) == 0)
            {
               present = 1;
               break;
            }
         }
         if (present)
            continue;
         cJSON *r = cJSON_CreateObject();
         if (!r)
         {
            oom = 1;
            break;
         }
         cJSON_AddStringToObject(r, "type", type);
         cJSON_AddStringToObject(r, "ref", ref);
         const char *v = versions ? versions[i] : NULL;
         if (v && v[0])
            cJSON_AddStringToObject(r, "v", v);
         cJSON_AddItemToArray(refs, r);
         added++;
      }
      if (oom)
      {
         cJSON_Delete(p);
         result = -1;
         break;
      }

      /* typed refs don't change the memory-only legacy projections, but a migrated
       * legacy event still needs them rebuilt so surfaced_ids/items exist. */
      if (added > 0)
         project_memory_refs(p);

      if (added == 0)
      {
         cJSON_Delete(p);
         if (id_out && id_out_len > 0)
            snprintf(id_out, (size_t)id_out_len, "%s", ev_id);
         result = 0;
         break;
      }

      char *newpayload = cJSON_PrintUnformatted(p);
      cJSON_Delete(p);
      if (!newpayload)
      {
         result = -1;
         break;
      }
      int cas = cas_update_event_payload(conn, ev_id, payload, newpayload);
      free(newpayload);
      if (cas < 0)
      {
         result = -1;
         break;
      }
      if (cas > 0)
      {
         if (id_out && id_out_len > 0)
            snprintf(id_out, (size_t)id_out_len, "%s", ev_id);
         result = 0;
         break;
      }
      /* 0 rows: concurrent change — re-read and retry. */
   }
   free(payload);
   return result;
}

int db2_demotion_retrieval_attribution_write(const char *retrieval_event_id,
                                             int64_t surfaced_row_id, const char *verdict,
                                             double weight)
{
   if (!retrieval_event_id || !verdict)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   /* scope_id = string(surfaced_row_id) for fast lookup by row. */
   char scope_id_buf[32];
   snprintf(scope_id_buf, sizeof(scope_id_buf), "%lld", (long long)surfaced_row_id);

   cJSON *record = cJSON_CreateObject();
   if (!record)
      return -1;
   cJSON_AddStringToObject(record, "retrieval_event_id", retrieval_event_id);
   cJSON_AddItemToObject(record, "surfaced_row_id", jo_i64_value_exact(surfaced_row_id));
   cJSON_AddStringToObject(record, "verdict", verdict);
   cJSON_AddNumberToObject(record, "weight", weight);
   char *payload = cJSON_PrintUnformatted(record);
   cJSON_Delete(record);
   if (!payload)
      return -1;
   int rc = db2_artifact_write(id, "retrieval_attribution", "proposed", "memory", scope_id_buf, "",
                               1.0, payload);
   free(payload);
   if (rc != 0)
      return -1;

   /* Stamp model_version = retrieval_event_id for FK-style linking queries. */
   void *conn = db2_conn();
   if (!conn)
      return 0; /* written, but can't stamp event link — tolerable */

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE artifacts SET model_version = ?1 WHERE id = ?2", err, sizeof(err));
   if (st)
   {
      aimee_pg_bind_text(st, "?1", retrieval_event_id);
      aimee_pg_bind_text(st, "?2", id);
      aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);
   }
   return 0;
}
