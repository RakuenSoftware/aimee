#include "json_int64.h"
/* kb_service_memory.c: aimee-kb dispatch handlers for the memory.*
 * RPC family (find_facts, list, get, briefing, context_block,
 * entity_profile, entity_edges, search_graph, get_episode, ask,
 * store).  Split out of kb_service.c so the file stays under the
 * per-file line cap. */

#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "module_commands.h"
#include "config.h"
#include "modules/db2/c/kb_service_backend.h"
#include "modules/db2/c/demotion.h" /* db2_demotion_retrieval_event_write_turn (auditable-correctness P1) */
#include "modules/db2/c/evidence_lifecycle.h" /* P5 outcome history on provenance export */
#include "modules/db2/c/memory_query.h"
#include "modules/db2/c/fidelity.h" /* db2_fidelity_report_by_turn (auditable-correctness P3) */
#include "modules/db2/c/fact_mutation.h"
#include "modules/db2/c/code_index_ops.h" /* db2_code_file_hash (auditable-correctness P1.5 code provenance) */
#include "kb_service_memory.h"
#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Defined in kb_service.c; non-static so this file can call them. */
int kb_send_response(int fd, cJSON *resp);
int kb_send_error(int fd, const char *message);
int kb_reply_or_error(int fd, cJSON *resp, const char *err_msg);

/* Auditable-correctness P1: record one per-turn retrieval_event keyed by the
 * caller-visible turn_id, listing the int64 memory rows surfaced into the turn.
 * The emission decision (the kb_evidence_emit_enabled flag) is made server-side
 * by the ingress before this action is called; this handler is the dumb writer.
 * Uses the already-merged turn-keyed writer (first-wins on a duplicate turn_id).
 * Returns {status:ok, retrieval_event_id} on success. */
int kb_handle_evidence_emit_retrieval_event(int fd, cJSON *req)
{
   cJSON *turn_j = cJSON_GetObjectItemCaseSensitive(req, "turn_id");
   cJSON *role_j = cJSON_GetObjectItemCaseSensitive(req, "role");
   cJSON *fp_j = cJSON_GetObjectItemCaseSensitive(req, "query_fingerprint");
   cJSON *ids_j = cJSON_GetObjectItemCaseSensitive(req, "surfaced_ids");
   if (!cJSON_IsString(turn_j) || !turn_j->valuestring[0])
      return kb_send_error(fd, "evidence.emit_retrieval_event requires turn_id");
   const char *role =
       cJSON_IsString(role_j) && role_j->valuestring[0] ? role_j->valuestring : "Recall";
   const char *fp = cJSON_IsString(fp_j) ? fp_j->valuestring : "";

   int n = cJSON_IsArray(ids_j) ? cJSON_GetArraySize(ids_j) : 0;
   int64_t *ids = NULL;
   int n_ids = 0;
   if (n > 0)
   {
      ids = (int64_t *)calloc((size_t)n, sizeof(int64_t));
      if (!ids)
         return kb_send_error(fd, "out of memory");
      for (int i = 0; i < n; i++)
      {
         cJSON *e = cJSON_GetArrayItem(ids_j, i);
         int64_t id;
         if (!jo_read_i64_exact(e, &id))
         {
            free(ids);
            return kb_send_error(
                fd,
                "source ids require exact integers; use decimal strings above the JSON safe range");
         }
         if (id > 0)
            ids[n_ids++] = id;
      }
   }

   char ev_id[64] = "";
   int rc = db2_demotion_retrieval_event_write_turn(turn_j->valuestring, fp, role, ids, n_ids,
                                                    ev_id, sizeof(ev_id));
   free(ids);
   if (rc != 0)
      return kb_send_error(fd, "failed to write retrieval_event");

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "retrieval_event_id", ev_id);
   return kb_reply_or_error(fd, resp, "failed to write retrieval_event");
}

/* Auditable-correctness P1.5 (D3/D14): the two-writer code-ref merge. A second
 * surface (the ingress code search) contributes its typed code refs into the SAME
 * turn's retrieval_event, idempotently merged with whatever the memory surface
 * already wrote. The emission decision (kb_evidence_emit_enabled) is made
 * server-side; this is the dumb writer over db2_demotion_retrieval_event_merge_refs_turn. */
int kb_handle_evidence_merge_retrieval_event(int fd, cJSON *req)
{
   cJSON *turn_j = cJSON_GetObjectItemCaseSensitive(req, "turn_id");
   cJSON *role_j = cJSON_GetObjectItemCaseSensitive(req, "role");
   cJSON *fp_j = cJSON_GetObjectItemCaseSensitive(req, "query_fingerprint");
   cJSON *refs_j = cJSON_GetObjectItemCaseSensitive(req, "surfaced_refs");
   if (!cJSON_IsString(turn_j) || !turn_j->valuestring[0])
      return kb_send_error(fd, "evidence.merge_retrieval_event requires turn_id");
   const char *role =
       cJSON_IsString(role_j) && role_j->valuestring[0] ? role_j->valuestring : "Recall";
   const char *fp = cJSON_IsString(fp_j) ? fp_j->valuestring : "";

   int n = cJSON_IsArray(refs_j) ? cJSON_GetArraySize(refs_j) : 0;
   const char **types = NULL, **refs = NULL, **versions = NULL;
   int m = 0;
   if (n > 0)
   {
      types = (const char **)calloc((size_t)n, sizeof(char *));
      refs = (const char **)calloc((size_t)n, sizeof(char *));
      versions = (const char **)calloc((size_t)n, sizeof(char *));
      if (!types || !refs || !versions)
      {
         free(types);
         free(refs);
         free(versions);
         return kb_send_error(fd, "out of memory");
      }
      for (int i = 0; i < n; i++)
      {
         cJSON *e = cJSON_GetArrayItem(refs_j, i);
         if (!e)
            continue;
         cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "type");
         cJSON *r = cJSON_GetObjectItemCaseSensitive(e, "ref");
         cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "v");
         /* require a non-empty typed identity (defence-in-depth: the merge skips
          * empties too, but don't forward junk from an adversarial caller). */
         if (!cJSON_IsString(t) || !t->valuestring[0] || !cJSON_IsString(r) || !r->valuestring[0])
            continue;
         types[m] = t->valuestring;
         refs[m] = r->valuestring;
         versions[m] = (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
         m++;
      }
   }

   char ev_id[64] = "";
   /* m==0 (no valid refs) is a no-op: don't call the merge, which would otherwise
    * create a bare turn event for an empty request. */
   int rc = (m > 0) ? db2_demotion_retrieval_event_merge_refs_turn(turn_j->valuestring, fp, role,
                                                                   types, refs, versions, m, ev_id,
                                                                   sizeof(ev_id))
                    : 0;
   free(types);
   free(refs);
   free(versions);
   if (rc != 0)
      return kb_send_error(fd, "failed to merge retrieval_event refs");

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "retrieval_event_id", ev_id);
   return kb_reply_or_error(fd, resp, "failed to merge retrieval_event refs");
}

/* Auditable-correctness P1: the /v1/audit/trace read. Look up the per-turn
 * retrieval_event by its caller-visible turn_id and return a four-state status.
 * Only two states are reachable in P1's single-writer foundation:
 *   - "ok"                   : a retrieval_event durably exists for the turn.
 *   - "evidence_unavailable" : the turn id resolves to no durable event (never
 *                              landed — e.g. DB2 was down at emit) or the read
 *                              itself errored.
 * The other two states are defined but produced by later phases:
 *   - "not_instrumented"     : a path that emits no evidence (e.g. the Anthropic
 *                              relay) — needs the per-path awareness of P1b.
 *   - "not_evaluated"        : fidelity skipped on a tool-loop turn — P3.
 * Never a falsely-empty success: a missing event is an explicit status, and the
 * action always returns status:ok (the lookup ran) with trace_status set. */
int kb_handle_evidence_trace_retrieval_event(int fd, cJSON *req)
{
   cJSON *turn_j = cJSON_GetObjectItemCaseSensitive(req, "turn_id");
   if (!cJSON_IsString(turn_j) || !turn_j->valuestring[0])
      return kb_send_error(fd, "audit.trace requires turn_id");

   char ev_id[64] = "";
   char payload[8192] = "";
   int rc = db2_demotion_retrieval_event_by_turn(turn_j->valuestring, ev_id, sizeof(ev_id), payload,
                                                 sizeof(payload));

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "turn_id", turn_j->valuestring);
   if (rc == 1)
   {
      cJSON_AddStringToObject(resp, "trace_status", "ok");
      cJSON_AddStringToObject(resp, "retrieval_event_id", ev_id);
      /* Embed the event payload (surfaced ids etc.) as a structured object when
       * it parses, else as the raw string — never silently dropped. */
      cJSON *ev = payload[0] ? cJSON_Parse(payload) : NULL;
      if (ev)
         cJSON_AddItemToObject(resp, "event", ev);
      else if (payload[0])
         cJSON_AddStringToObject(resp, "event_raw", payload);
   }
   else
   {
      cJSON_AddStringToObject(resp, "trace_status", "evidence_unavailable");
      cJSON_AddStringToObject(resp, "detail",
                              rc < 0 ? "lookup error" : "no durable retrieval_event for this turn");
   }
   return kb_reply_or_error(fd, resp, "failed to trace retrieval_event");
}

/* Auditable-correctness P2: the /v1/audit/provenance read. Look up the turn's
 * retrieval_event (same four-state status as the trace read) and resolve each
 * surfaced source id to {id, kind, source, version} — version is the memory row's
 * updated_at, read live, so a caller can compare it to what trace recorded and a
 * source no longer present (deleted/superseded since the turn) is flagged
 * present:false rather than silently dropped. This is a pure read (D8). */
int kb_handle_evidence_provenance(int fd, cJSON *req)
{
   cJSON *turn_j = cJSON_GetObjectItemCaseSensitive(req, "turn_id");
   if (!cJSON_IsString(turn_j) || !turn_j->valuestring[0])
      return kb_send_error(fd, "audit.provenance requires turn_id");
   /* turn_ids are short UUIDs; reject absurd input up front (defense in depth —
    * downstream binds it as a SQL parameter, not into a fixed buffer). */
   if (strlen(turn_j->valuestring) > 128)
      return kb_send_error(fd, "audit.provenance turn_id too long");

   char ev_id[64] = "";
   char payload[8192] = "";
   int rc = db2_demotion_retrieval_event_by_turn(turn_j->valuestring, ev_id, sizeof(ev_id), payload,
                                                 sizeof(payload));

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "turn_id", turn_j->valuestring);
   if (rc == 1)
   {
      cJSON_AddStringToObject(resp, "provenance_status", "ok");
      cJSON_AddStringToObject(resp, "retrieval_event_id", ev_id);
      /* P5 extends the established audit/CLI export rather than creating a
       * parallel reporting path.  An empty array means computed-and-empty;
       * lookup failure is explicit and never masquerades as no outcomes. */
      char outcomes_json[16384] = "";
      if (db2_work_outcomes_for_retrieval_json(ev_id, outcomes_json, (int)sizeof(outcomes_json)) ==
          0)
      {
         cJSON *outcomes = cJSON_Parse(outcomes_json);
         if (cJSON_IsArray(outcomes))
            cJSON_AddItemToObject(resp, "outcomes", outcomes);
         else
         {
            cJSON_Delete(outcomes);
            cJSON_AddStringToObject(resp, "outcomes", "not-computed");
         }
      }
      else
         cJSON_AddStringToObject(resp, "outcomes", "not-computed");
      cJSON *sources = cJSON_AddArrayToObject(resp, "sources");
      cJSON *ev = payload[0] ? cJSON_Parse(payload) : NULL;
      cJSON *ids = ev ? cJSON_GetObjectItemCaseSensitive(ev, "surfaced_ids") : NULL;
      /* surfaced_items = [{id, v}] carries each source's point-in-time version
       * (memories.updated_at at emit). Absent on legacy events (pre emit-capture);
       * then turn_version is "" and drift is simply not reported. */
      cJSON *items = ev ? cJSON_GetObjectItemCaseSensitive(ev, "surfaced_items") : NULL;
      int n = cJSON_IsArray(ids) ? cJSON_GetArraySize(ids) : 0;
      for (int i = 0; sources && i < n; i++)
      {
         cJSON *e = cJSON_GetArrayItem(ids, i);
         int64_t id;
         if (!jo_read_i64_exact(e, &id))
         {
            cJSON *unknown = cJSON_CreateObject();
            cJSON_AddStringToObject(unknown, "identity_state", "unavailable");
            cJSON_AddStringToObject(unknown, "reason",
                                    "stored source identity is not an exact integer");
            cJSON_AddBoolToObject(unknown, "present", 0);
            cJSON_AddBoolToObject(unknown, "error", 1);
            cJSON_AddItemToArray(sources, unknown);
            continue;
         }
         if (id <= 0)
            continue;
         cJSON *record_args = cJSON_CreateObject(), *record_reply = NULL;
         cJSON_AddStringToObject(record_args, "operation", "record");
         cJSON_AddItemToObject(record_args, "id", jo_i64_value_exact(id));
         int fetched =
             aimee_module_commands_dispatch_internal("memory.runtime", record_args, &record_reply);
         cJSON_Delete(record_args);
         const cJSON *record = cJSON_GetObjectItemCaseSensitive(record_reply, "memory");
         int found = fetched == 1 && strcmp(jo_cstr(record_reply, "status"), "ok") == 0 &&
                             cJSON_IsObject(record)
                         ? 1
                     : fetched == 1 && strcmp(jo_cstr(record_reply, "kind"), "not_found") == 0 ? 0
                                                                                               : -1;
         const char *kind = jo_cstr(record, "kind"), *source = jo_cstr(record, "source_session"),
                    *version = jo_cstr(record, "updated_at");
         cJSON *src = cJSON_CreateObject();
         cJSON_AddItemToObject(src, "id", jo_i64_value_exact(id));
         cJSON_AddStringToObject(src, "kind", kind);
         cJSON_AddStringToObject(src, "source", source);
         cJSON_AddStringToObject(src, "version", version); /* live/current version */
         cJSON_AddBoolToObject(src, "present", found == 1);
         /* Distinguish a source that is genuinely gone (found==0, deleted/
          * superseded since the turn) from one whose lookup errored (found<0):
          * both leave present:false, but only the latter is an unreliable read. */
         if (found < 0)
            cJSON_AddBoolToObject(src, "error", 1);
         /* Point-in-time version captured at emit (surfaced_items[].v): report it
          * and flag drift when the source has since changed (turn_version differs
          * from the current version of a still-present source). */
         const char *turn_version = "";
         if (cJSON_IsArray(items))
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
                     turn_version = v->valuestring;
                  break;
               }
            }
         }
         cJSON_AddStringToObject(src, "turn_version", turn_version);
         cJSON_AddBoolToObject(src, "drifted",
                               turn_version[0] && found == 1 && strcmp(turn_version, version) != 0);
         cJSON_AddItemToArray(sources, src);
         cJSON_Delete(record_reply);
      }
      /* auditable-correctness P1.5 (D3): resolve typed CODE refs from the unified
       * surfaced_refs. Each {type:"code", ref:"code:<project>:<file_path>", v} is
       * reported in a separate code_sources[] with present + drift (live files.hash
       * != the version captured on the turn). Memory refs already came through
       * sources[] above (the legacy projection), so only code is resolved here. */
      /* Always present (possibly empty) for a stable API contract, mirroring how
       * `sources` is always present — even for a legacy event with no surfaced_refs. */
      cJSON *code_sources = cJSON_AddArrayToObject(resp, "code_sources");
      cJSON *refs = ev ? cJSON_GetObjectItemCaseSensitive(ev, "surfaced_refs") : NULL;
      if (cJSON_IsArray(refs))
      {
         int rn = cJSON_GetArraySize(refs);
         for (int i = 0; i < rn; i++)
         {
            cJSON *r = cJSON_GetArrayItem(refs, i);
            cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "type");
            cJSON *rfj = cJSON_GetObjectItemCaseSensitive(r, "ref");
            if (!cJSON_IsString(t) || strcmp(t->valuestring, "code") != 0 || !cJSON_IsString(rfj))
               continue;
            const char *ref = rfj->valuestring;
            if (strncmp(ref, "code:", 5) != 0)
               continue;
            /* parse code:<project>:<file_path> — split on the FIRST colon after the
             * "code:" prefix (project has no colon; file_path keeps any remaining). */
            const char *rest = ref + 5;
            const char *colon = strchr(rest, ':');
            if (!colon || colon == rest || !colon[1])
               continue;
            char project[128] = "";
            size_t plen = (size_t)(colon - rest);
            if (plen >= sizeof(project))
               plen = sizeof(project) - 1;
            memcpy(project, rest, plen);
            project[plen] = '\0';
            const char *file_path = colon + 1;

            cJSON *vj = cJSON_GetObjectItemCaseSensitive(r, "v");
            const char *turn_v = (cJSON_IsString(vj) && vj->valuestring) ? vj->valuestring : "";
            /* >= content_hash[80] (the source width) with margin, so a live hash is
             * never truncated into a spurious mismatch. */
            char live[128] = "";
            int found = db2_code_file_hash(project, file_path, live, sizeof(live));

            if (!code_sources)
               continue; /* array alloc failed — skip rather than leak */
            cJSON *cs = cJSON_CreateObject();
            if (!cs)
               continue;
            cJSON_AddStringToObject(cs, "ref", ref);
            cJSON_AddStringToObject(cs, "version", turn_v); /* captured on the turn */
            cJSON_AddStringToObject(cs, "live_hash", live);
            cJSON_AddBoolToObject(cs, "present", found == 1);
            if (found < 0) /* lookup error — distinct from a genuinely absent file */
               cJSON_AddBoolToObject(cs, "error", 1);
            /* drift only when BOTH versions are known and differ; an empty live or
             * turn version means "unknown", not drift (matches memory provenance). */
            cJSON_AddBoolToObject(cs, "drifted",
                                  found == 1 && turn_v[0] && live[0] && strcmp(turn_v, live) != 0);
            if (!cJSON_AddItemToArray(code_sources, cs))
               cJSON_Delete(cs);
         }
      }
      if (ev)
         cJSON_Delete(ev);
   }
   else
   {
      cJSON_AddStringToObject(resp, "provenance_status", "evidence_unavailable");
      cJSON_AddStringToObject(resp, "detail",
                              rc < 0 ? "lookup error" : "no durable retrieval_event for this turn");
   }
   return kb_reply_or_error(fd, resp, "failed to read provenance");
}

/* Auditable-correctness P3: the /v1/audit/fidelity read. Return the turn's
 * answer-level fidelity_report (supported/unsupported/abstained buckets) plus the
 * per-chunk attribution_count. A pure read of the non-scored fidelity artifacts.
 * When no report exists the status is not_evaluated (the default-off judge has not
 * run for the turn) — distinct from a lookup error (evidence_unavailable). */
int kb_handle_evidence_fidelity(int fd, cJSON *req)
{
   cJSON *turn_j = cJSON_GetObjectItemCaseSensitive(req, "turn_id");
   if (!cJSON_IsString(turn_j) || !turn_j->valuestring[0])
      return kb_send_error(fd, "audit.fidelity requires turn_id");
   if (strlen(turn_j->valuestring) > 128)
      return kb_send_error(fd, "audit.fidelity turn_id too long");

   char fstatus[32] = "";
   int sup = 0, uns = 0, abst = 0;
   int rc = db2_fidelity_report_by_turn(turn_j->valuestring, fstatus, sizeof(fstatus), &sup, &uns,
                                        &abst);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "turn_id", turn_j->valuestring);
   if (rc == 1)
   {
      cJSON_AddStringToObject(resp, "fidelity_status", fstatus[0] ? fstatus : "ok");
      cJSON *rep = cJSON_AddObjectToObject(resp, "report");
      cJSON_AddNumberToObject(rep, "supported", sup);
      cJSON_AddNumberToObject(rep, "unsupported", uns);
      cJSON_AddNumberToObject(rep, "abstained", abst);
      /* Emit the count only when it read cleanly; on a count error omit it (rather
       * than clamp to 0, which would conflate "no attributions" with "count
       * failed") and flag the error — honesty over a silent zero on an audit read. */
      int ac = db2_fidelity_attribution_count_by_turn(turn_j->valuestring);
      if (ac >= 0)
         cJSON_AddNumberToObject(resp, "attribution_count", ac);
      else
         cJSON_AddBoolToObject(resp, "attribution_count_error", 1);
   }
   else
   {
      cJSON_AddStringToObject(resp, "fidelity_status",
                              rc < 0 ? "evidence_unavailable" : "not_evaluated");
      cJSON_AddStringToObject(resp, "detail",
                              rc < 0 ? "lookup error" : "no fidelity report for this turn");
   }
   return kb_reply_or_error(fd, resp, "failed to read fidelity report");
}

/* Only transport remains: Go owns validation, authority, mutation and replies. */
static int kb_entities_mutation(int fd, cJSON *req, const char *action)
{
   cJSON *args = cJSON_CreateObject(), *reply = NULL;
   cJSON_AddStringToObject(args, "operation", "entity-mutate");
   cJSON_AddStringToObject(args, "action", action);
   const char *fields[] = {"from_id", "into_id", "merge_id"};
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
   {
      cJSON *value = cJSON_GetObjectItemCaseSensitive(req, fields[i]);
      if (value)
         cJSON_AddItemToObject(args, fields[i], cJSON_Duplicate(value, 1));
   }
   int rc = args ? aimee_module_commands_dispatch_internal("memory.runtime", args, &reply) : -1;
   cJSON_Delete(args);
   const char *json = jo_cstr(reply, "json");
   cJSON *result = NULL;
   if (rc == 1 && !strcmp(jo_cstr(reply, "status"), "ok") && strlen(json) <= 1048576)
   {
      cJSON *parsed = cJSON_ParseWithOpts(json, NULL, 1);
      const char *status = jo_cstr(parsed, "status");
      cJSON *id = cJSON_GetObjectItemCaseSensitive(parsed, "merge_id");
      int valid_id = cJSON_IsNumber(id) && isfinite(id->valuedouble) && id->valuedouble > 0 &&
                     trunc(id->valuedouble) == id->valuedouble;
      if (cJSON_IsObject(parsed) &&
          ((!strcmp(status, "ok") && valid_id && jo_cstr(parsed, "commit_id")[0]) ||
           !strcmp(status, "error")))
         result = cJSON_CreateRaw(json);
      cJSON_Delete(parsed);
   }
   cJSON_Delete(reply);
   return kb_reply_or_error(fd, result, "entity mutation unavailable");
}

int kb_handle_entities_merge(int fd, cJSON *req)
{
   return kb_entities_mutation(fd, req, "merge");
}

int kb_handle_entities_unmerge(int fd, cJSON *req)
{
   return kb_entities_mutation(fd, req, "unmerge");
}

int kb_handle_task_create(int fd, cJSON *req)
{
   cJSON *title_j = cJSON_GetObjectItemCaseSensitive(req, "title");
   cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *parent_j = cJSON_GetObjectItemCaseSensitive(req, "parent_id");
   if (!cJSON_IsString(title_j))
      return kb_send_error(fd, "task.create requires title");
   const char *sid = cJSON_IsString(sid_j) ? sid_j->valuestring : "";
   int64_t parent = cJSON_IsNumber(parent_j) ? (int64_t)parent_j->valuedouble : 0;

   cJSON *resp = db2_kb_service_task_create_json(title_j->valuestring, sid, parent);
   return kb_reply_or_error(fd, resp, "failed to create task");
}

int kb_handle_task_update_state(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   if (!cJSON_IsNumber(id_j) || !cJSON_IsString(state_j))
      return kb_send_error(fd, "task.update_state requires id and state");

   cJSON *resp =
       db2_kb_service_task_update_state_json((int64_t)id_j->valuedouble, state_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to update task state");
}

int kb_handle_task_delete(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "task.delete requires id");

   cJSON *resp = db2_kb_service_task_delete_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to delete task");
}

int kb_handle_task_add_edge(int fd, cJSON *req)
{
   cJSON *src_j = cJSON_GetObjectItemCaseSensitive(req, "source");
   cJSON *dst_j = cJSON_GetObjectItemCaseSensitive(req, "target");
   cJSON *rel_j = cJSON_GetObjectItemCaseSensitive(req, "relation");
   if (!cJSON_IsNumber(src_j) || !cJSON_IsNumber(dst_j))
      return kb_send_error(fd, "task.add_edge requires source and target");
   const char *relation = cJSON_IsString(rel_j) ? rel_j->valuestring : "depends_on";

   cJSON *resp = db2_kb_service_task_add_edge_json((int64_t)src_j->valuedouble,
                                                   (int64_t)dst_j->valuedouble, relation);
   return kb_reply_or_error(fd, resp, "failed to add task edge");
}

int kb_handle_task_get_edges(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "task_id");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "task.get_edges requires task_id");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 16;

   cJSON *resp = db2_kb_service_task_get_edges_json((int64_t)id_j->valuedouble, max);
   return kb_reply_or_error(fd, resp, "failed to fetch task edges");
}

int kb_handle_task_list(int fd, cJSON *req)
{
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *state = cJSON_IsString(state_j) ? state_j->valuestring : NULL;
   const char *sid = cJSON_IsString(sid_j) ? sid_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 16;

   cJSON *resp = db2_kb_service_task_list_json(state, sid, limit);
   return kb_reply_or_error(fd, resp, "failed to list tasks");
}
