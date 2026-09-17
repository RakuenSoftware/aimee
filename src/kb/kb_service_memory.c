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
#include "modules/db2/c/memory_scope_query.h"
#include "modules/db2/c/fidelity.h" /* db2_fidelity_report_by_turn (auditable-correctness P3) */
#include "modules/db2/c/fact_mutation.h"
#include "modules/db2/c/code_index_ops.h" /* db2_code_file_hash (auditable-correctness P1.5 code provenance) */
#include "kb/kb_login_throttle.h" /* kb_login_throttle_peer_is_loopback — the canonical local test */
#include "kb_reqctx.h"            /* kb_reqctx_actor — authenticated caller for write authority */
#include "kb_service_memory.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Defined in kb_service.c; non-static so this file can call them. */
int kb_send_response(int fd, cJSON *resp);
int kb_send_error(int fd, const char *message);
int kb_reply_or_error(int fd, cJSON *resp, const char *err_msg);

/* The typed-fact write authority of the request being served (typed-fact §5).
 *
 * Derived from the request's AUTHENTICATED actor and nothing else — never from a
 * field in the request (verify-then-trust, kb_verifier.h). Class A is reserved
 * for a direct assertion by the user, so only a named human identity qualifies:
 * an OIDC subject, or a host account the calling service asserted over the mTLS
 * listener after its certificate, bearer and service identity all verified
 * (kb_tls_serve.c -> kb_reqctx_apply_asserted). Everything else is an agent-side
 * or service origin and gets MODEL authority — a bearer/owner credential, an
 * mTLS machine identity, and an unauthenticated caller alike.
 *
 * The actor answers WHO is calling. It does not answer whether the payload is
 * that person's own words: an agent's tool call, made mid-turn, inherits the
 * human's request context. So a caller that relays model-composed text must not
 * use this — see kb_handle_memory_context_block.
 *
 * The owner bearer from a LOOPBACK PEER is the third case, and it is a
 * deployment fact rather than a weaker rule. Only the plain HTTP listener
 * records a peer address (kb_http_listener.c), so this condition means
 * specifically "arrived over plain loopback HTTP" — never mTLS, never a remote
 * peer, and never a direct in-process call, all of which leave the peer empty
 * and are not local. That listener serves exactly one client, aimee-server on
 * this host, because kb_client refuses to put the bearer on a cleartext link to
 * anywhere else; and it has no peer certificate to bind a caller assertion to,
 * so kb_http_conn.c rejects the caller header outright (B5). aimee-server has
 * already derived the authority from the kernel-attested peer of ITS OWN socket
 * and only asks for "user" when it attested a person. Nothing the model can
 * reach bypasses that: its tool calls go through the server, which resolves
 * authority from attestation and never from the payload. Over mTLS the human
 * arrives as an asserted host actor instead, handled above. */
/* Who is this request, in the only terms that decide whether it may speak as the
 * user: is there an authenticated account?
 *
 * Authentication happens once, at message receipt -- the channel, the session
 * and the account are each verified there, and a request that fails any of them
 * never reaches an action. `actor` IS that result, so this reads it rather than
 * re-deriving anything: only the constructors in kb_identity.h set
 * authenticated = 1, and a zero-initialized principal is unauthenticated.
 *
 * An account NAMES SOMEONE, and that is what separates the four kinds:
 *
 *   OIDC   an issuer-scoped subject -- the same account whatever carried it
 *   HOST   a local host account PAM accepted
 *   CERT   a verified mTLS peer, which names one enrolled machine
 *   OWNER  a SHARED install credential, which names nobody in particular
 *
 * The first three identify a principal, so no transport qualifier applies to
 * them: an OIDC subject is the same person over any socket, and a client cert
 * names one enrolled machine.
 *
 * OWNER is different in kind, not merely weaker. It is one bearer for the whole
 * install, so it cannot say WHICH person is acting; loopback is what makes it
 * stand for "the operator at this machine" rather than "whoever holds the
 * token". Dropping that qualifier was measured, from a genuinely non-loopback
 * peer, and it let a remote holder of the bearer destroy a user-stated Class-A
 * fact:
 *
 *     alice before: A current
 *     remote peer, authority=user -> {"status":"ok","retracted":1}
 *     alice after:  A gone
 *
 * That is a real widening of what a leaked bearer can do, and it is not what
 * the account model argues for -- the model says the ACCOUNT decides, and a
 * shared credential is precisely the case where there is no account to decide
 * with. The qualifier is kept for OWNER alone.
 *
 * CERT was previously excluded, mirroring a matching exclusion of
 * ATTEST_MTLS_CLIENT on the server, and the two together were the bug: a client
 * presenting a verified certificate was an authenticated principal everywhere
 * else in the tree (vault_capability.c puts it with UDS/webchat precisely
 * because it "makes the grant expressible per client") yet an anonymous agent
 * here. A caller could be a person to one daemon and not to the other. */
static fact_authority_t kb_memory_request_authority(void)
{
   const kb_principal_t *actor = kb_reqctx_actor(); /* NULL unless authenticated */
   if (!actor || !actor->authenticated || actor->kind == KB_PRIN_NONE)
      return FACT_AUTHORITY_MODEL;
   /* A shared install bearer only stands for a person at the machine it is
    * installed on. Every other kind names one. */
   if (actor->kind == KB_PRIN_OWNER)
      return kb_login_throttle_peer_is_loopback() ? FACT_AUTHORITY_USER : FACT_AUTHORITY_MODEL;
   return FACT_AUTHORITY_USER;
}

static int kb_memory_scope_begin(cJSON *req, int force, int *missing_out)
{
   cJSON *enabled_j = cJSON_GetObjectItemCaseSensitive(req, "scope_context");
   if (!force && !(cJSON_IsBool(enabled_j) && cJSON_IsTrue(enabled_j)))
      return 0;
   cJSON *workspace_j = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   cJSON *project_j = cJSON_GetObjectItemCaseSensitive(req, "project");
   cJSON *all_j = cJSON_GetObjectItemCaseSensitive(req, "include_all");
   const char *workspace = cJSON_IsString(workspace_j) ? workspace_j->valuestring : "";
   const char *project = cJSON_IsString(project_j) ? project_j->valuestring : "";
   int include_all = cJSON_IsBool(all_j) && cJSON_IsTrue(all_j);
   db2_memory_scope_context_set(workspace, project, include_all);
   if (missing_out)
      *missing_out = (!workspace[0] && !project[0]) ? 1 : 0;
   return 1;
}

static void kb_memory_scope_end(cJSON *resp, int active, int missing)
{
   if (active && resp)
      cJSON_AddBoolToObject(resp, "active_context_missing", missing ? 1 : 0);
   if (active)
      db2_memory_scope_context_clear();
}

static int kb_handle_session_briefing_section(int fd, cJSON *req, cJSON *(*fn)(int limit),
                                              const char *err_msg)
{
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 0;
   cJSON *resp = fn(limit);
   if (!resp)
      return kb_send_error(fd, err_msg);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_memory_assemble_typed_context(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   if (!cJSON_IsString(query_j) || !query_j->valuestring[0])
      return kb_send_error(fd, "memory.assemble_typed_context requires a non-empty query");
   int missing = 0;
   int scope_active = kb_memory_scope_begin(req, 1, &missing);
   cJSON *resp = db2_kb_service_memory_assemble_typed_context_json(req);
   kb_memory_scope_end(resp, scope_active, missing);
   return kb_reply_or_error(fd, resp, "failed to assemble typed context");
}

int kb_handle_session_briefing_commitments(int fd, cJSON *req)
{
   return kb_handle_session_briefing_section(fd, req,
                                             db2_kb_service_session_briefing_commitments_json,
                                             "failed to render session-briefing commitments");
}

int kb_handle_session_briefing_directives(int fd, cJSON *req)
{
   return kb_handle_session_briefing_section(fd, req,
                                             db2_kb_service_session_briefing_directives_json,
                                             "failed to render session-briefing directives");
}

int kb_handle_memory_context_block(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *block_j = cJSON_GetObjectItemCaseSensitive(req, "block_type");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(query_j))
      return kb_send_error(fd, "memory.context_block requires query");
   const char *block_type = cJSON_IsString(block_j) ? block_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 5;

   int missing = 0;
   int scope_active = kb_memory_scope_begin(req, 0, &missing);
   /* MODEL authority, structurally — not kb_memory_request_authority(). This
    * action's only caller is the get_context_block MCP tool, so `query` is a
    * string the MODEL composed, even though the request carries the human's
    * authenticated identity (a tool call runs inside the user's turn and inherits
    * its context). Authenticating the caller therefore proves nothing about who
    * wrote the text, and the §4 retraction this query can trigger deletes facts.
    * A future caller that really does relay the user's own turn should pass
    * FACT_AUTHORITY_USER here — and nothing else should. */
   cJSON *resp = db2_kb_service_memory_context_block_json(query_j->valuestring, block_type, limit,
                                                          FACT_AUTHORITY_MODEL);
   kb_memory_scope_end(resp, scope_active, missing);
   return kb_reply_or_error(fd, resp, "failed to build context block");
}

int kb_handle_memory_facts(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   if (!cJSON_IsString(query_j))
      return kb_send_error(fd, "memory.facts requires query");

   int missing = 0;
   int scope_active = kb_memory_scope_begin(req, 0, &missing);
   cJSON *resp = db2_kb_service_memory_facts_json(query_j->valuestring);
   kb_memory_scope_end(resp, scope_active, missing);
   return kb_reply_or_error(fd, resp, "failed to recall facts");
}

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
         if (cJSON_IsNumber(e) && e->valuedouble > 0)
            ids[n_ids++] = (int64_t)e->valuedouble;
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
         if (!cJSON_IsNumber(e) || e->valuedouble <= 0)
            continue;
         /* Same cast the emit writer (kb_handle_evidence_emit_retrieval_event)
          * used to store the id, so the round-trip is lossless for the values
          * actually persisted. */
         int64_t id = (int64_t)e->valuedouble;
         cJSON *record_args = cJSON_CreateObject(), *record_reply = NULL;
         cJSON_AddStringToObject(record_args, "operation", "record");
         cJSON_AddNumberToObject(record_args, "id", (double)id);
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
         cJSON_AddNumberToObject(src, "id", (double)id);
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
               if (cJSON_IsNumber(iid) && (int64_t)iid->valuedouble == id)
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

int kb_handle_memory_search_assertions(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *valid_j = cJSON_GetObjectItemCaseSensitive(req, "valid_at");
   cJSON *believed_j = cJSON_GetObjectItemCaseSensitive(req, "believed_at");
   cJSON *historical_j = cJSON_GetObjectItemCaseSensitive(req, "include_historical");
   cJSON *hops_j = cJSON_GetObjectItemCaseSensitive(req, "max_hops");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(query_j) || !query_j->valuestring[0])
      return kb_send_error(fd, "memory.search_assertions requires a non-empty query");
   if (valid_j && !cJSON_IsString(valid_j))
      return kb_send_error(fd, "memory.search_assertions valid_at must be a timestamp string");
   if (believed_j && !cJSON_IsString(believed_j))
      return kb_send_error(fd, "memory.search_assertions believed_at must be a timestamp string");
   if (historical_j && !cJSON_IsBool(historical_j))
      return kb_send_error(fd, "memory.search_assertions include_historical must be boolean");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 10;
   int include_historical = cJSON_IsBool(historical_j) && cJSON_IsTrue(historical_j);
   int max_hops = cJSON_IsNumber(hops_j) ? (int)hops_j->valuedouble : 0;
   if (max_hops < 0 || max_hops > 2)
      return kb_send_error(fd, "memory.search_assertions max_hops must be between 0 and 2");

   int missing = 0;
   int scope_active = kb_memory_scope_begin(req, 1, &missing);
   cJSON *resp = db2_kb_service_memory_search_assertions_json(
       query_j->valuestring, cJSON_IsString(valid_j) ? valid_j->valuestring : "",
       cJSON_IsString(believed_j) ? believed_j->valuestring : "", include_historical, max_hops,
       limit);
   kb_memory_scope_end(resp, scope_active, missing);
   return kb_reply_or_error(fd, resp, "failed to search semantic assertions");
}

/* §4 retraction: withdraw a typed fact the layer got wrong. `target` is optional
 * and scopes the retraction to one value; omitting it retracts every current
 * value of (source, relation). */
int kb_handle_facts_retract(int fd, cJSON *req)
{
   cJSON *src_j = cJSON_GetObjectItemCaseSensitive(req, "source");
   cJSON *rel_j = cJSON_GetObjectItemCaseSensitive(req, "relation");
   cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(req, "target");
   cJSON *auth_j = cJSON_GetObjectItemCaseSensitive(req, "authority");
   if (!cJSON_IsString(src_j) || !src_j->valuestring[0] || !cJSON_IsString(rel_j) ||
       !rel_j->valuestring[0])
      return kb_send_error(fd, "facts.retract requires a non-empty source and relation");
   if (tgt_j && !cJSON_IsString(tgt_j))
      return kb_send_error(fd, "facts.retract target must be a string");
   if (auth_j && !cJSON_IsString(auth_j))
      return kb_send_error(fd, "facts.retract authority must be a string");

   /* This action is reachable from outside (POST /v1/actions/facts.retract), so
    * the body's `authority` is a request, not a grant: it may only lower what the
    * caller authenticated as. Without an authenticated human actor a "user"
    * retraction is served at model authority, and db2_fact_retract then leaves
    * Class-A facts and immutable relations alone.
    *
    * aimee-server carries the human across this hop as X-Aimee-Caller-Subject,
    * which the mTLS listener turns into a KB_PRIN_HOST actor after the peer
    * certificate, bearer and service identity have verified. The PLAIN listener
    * has no peer to attest and rejects that header by design (B5), so on a
    * plain-loopback kb a user retraction lands at model authority and reports
    * retracted: 0. That is a deployment property, not a silent failure — say so,
    * because "nothing happened and nothing was logged" is the hard version. */
   const char *requested = cJSON_IsString(auth_j) ? auth_j->valuestring : NULL;
   int wants_user = requested && strcmp(requested, "user") == 0;
   int granted_user = wants_user && kb_memory_request_authority() == FACT_AUTHORITY_USER;
   const char *authority = granted_user ? "user" : "model";
   if (wants_user && !granted_user)
      LOG_WARN("kb.facts",
               "user retraction of %s/%s served at model authority: the request "
               "carries no authenticated human actor",
               src_j->valuestring, rel_j->valuestring);

   cJSON *resp = db2_kb_service_facts_retract_json(
       src_j->valuestring, rel_j->valuestring, cJSON_IsString(tgt_j) ? tgt_j->valuestring : NULL,
       authority);
   return kb_reply_or_error(fd, resp, "failed to retract fact");
}

/* §3 entity merge: collapse two records of the same real entity into one. */
int kb_handle_entities_merge(int fd, cJSON *req)
{
   cJSON *from_j = cJSON_GetObjectItemCaseSensitive(req, "from_id");
   cJSON *into_j = cJSON_GetObjectItemCaseSensitive(req, "into_id");
   if (!cJSON_IsNumber(from_j) || !cJSON_IsNumber(into_j))
      return kb_send_error(fd, "entities.merge requires numeric from_id and into_id");
   if (from_j->valuedouble <= 0 || into_j->valuedouble <= 0)
      return kb_send_error(fd, "entities.merge ids must be positive");

   cJSON *resp = db2_kb_service_entities_merge_json((int64_t)from_j->valuedouble,
                                                    (int64_t)into_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to merge entities");
}

/* §3 unmerge: reverse a recorded merge. Without this the merge audit row was
 * reversible only in principle — nothing outside a test could undo one. */
int kb_handle_entities_unmerge(int fd, cJSON *req)
{
   cJSON *mid_j = cJSON_GetObjectItemCaseSensitive(req, "merge_id");
   if (!cJSON_IsNumber(mid_j) || mid_j->valuedouble <= 0)
      return kb_send_error(fd, "entities.unmerge requires a positive merge_id");

   cJSON *resp = db2_kb_service_entities_unmerge_json((int64_t)mid_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to unmerge entities");
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
