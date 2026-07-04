/* kb_http_governance.c: console governance routes — decision records + the
 * policy-verdict action audit. See kb_http_governance.h. All routes are
 * console-admin-ACL'd upstream. */
#include "kb_http_governance.h"

#include "cJSON.h"
#include "db2/artifacts.h"    /* db2_audit_event_* */
#include "db2/decision_log.h" /* db2_decision_log_* */
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DECISION_LIST_MAX 50
#define AUDIT_LIST_MAX    100

static const char *jstr(const cJSON *o, const char *k)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}
static int64_t jint(const cJSON *o, const char *k)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}

/* Read one query param (no percent-decoding needed for our ids/timestamps). */
static int qparam(const char *qs, const char *key, char *out, size_t cap)
{
   if (cap == 0)
      return 0;
   out[0] = '\0';
   if (!qs)
      return 0;
   size_t klen = strlen(key);
   const char *p = qs;
   while (p && *p)
   {
      if (strncmp(p, key, klen) == 0 && p[klen] == '=')
      {
         const char *v = p + klen + 1;
         const char *amp = strchr(v, '&');
         size_t n = amp ? (size_t)(amp - v) : strlen(v);
         if (n >= cap)
            n = cap - 1;
         memcpy(out, v, n);
         out[n] = '\0';
         return 1;
      }
      p = strchr(p, '&');
      if (p)
         p++;
   }
   return 0;
}

static int emit(cJSON *root, char *out_buf, int out_cap, int status)
{
   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s || strlen(s) >= (size_t)out_cap)
   {
      free(s);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"response too large\"}");
      return 500;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   return status;
}

static cJSON *decision_to_json(const db2_decision_log_row_t *d)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddNumberToObject(o, "id", (double)d->id);
   cJSON_AddStringToObject(o, "subject", d->subject);
   cJSON_AddStringToObject(o, "options", d->options);
   cJSON_AddStringToObject(o, "chosen", d->chosen);
   cJSON_AddStringToObject(o, "rationale", d->rationale);
   cJSON_AddStringToObject(o, "outcome", d->outcome);
   cJSON_AddStringToObject(o, "status", d->status);
   cJSON_AddStringToObject(o, "revisit_when", d->revisit_when);
   cJSON_AddNumberToObject(o, "supersedes_id", (double)d->supersedes_id);
   cJSON_AddStringToObject(o, "author", d->author);
   cJSON_AddNumberToObject(o, "linked_policy_id", (double)d->linked_policy_id);
   cJSON_AddStringToObject(o, "created_at", d->created_at);
   return o;
}

/* GET /v1/decisions[?subject=&status=&limit=] */
static int list_decisions(const char *qs, char *out_buf, int out_cap)
{
   char subject[256], status[32], limit_s[16];
   qparam(qs, "subject", subject, sizeof(subject));
   qparam(qs, "status", status, sizeof(status));
   qparam(qs, "limit", limit_s, sizeof(limit_s));
   int limit = limit_s[0] ? atoi(limit_s) : 50;
   db2_decision_log_row_t rows[DECISION_LIST_MAX];
   int n = db2_decision_log_list_scoped(subject[0] ? subject : NULL, status[0] ? status : NULL,
                                        limit, rows, DECISION_LIST_MAX);
   if (n < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"decisions unavailable\"}");
      return 503;
   }
   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "decisions");
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, decision_to_json(&rows[i]));
   cJSON_AddNumberToObject(root, "count", n);
   return emit(root, out_buf, out_cap, 200);
}

/* GET /v1/decisions/{id} — the decision plus its supersede chain (older first). */
static int get_decision(int64_t id, char *out_buf, int out_cap)
{
   db2_decision_log_row_t d;
   if (db2_decision_log_get(id, &d) != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"decision not found\"}");
      return 404;
   }
   cJSON *root = decision_to_json(&d);
   cJSON *chain = cJSON_AddArrayToObject(root, "supersede_chain");
   int64_t prev = d.supersedes_id;
   int guard = 0;
   while (prev > 0 && guard++ < 64)
   {
      db2_decision_log_row_t p;
      if (db2_decision_log_get(prev, &p) != 0)
         break;
      cJSON_AddItemToArray(chain, decision_to_json(&p));
      prev = p.supersedes_id;
   }
   return emit(root, out_buf, out_cap, 200);
}

/* POST /v1/decisions — create (or supersede via supersedes_id). One active
 * decision per (subject, linked_policy_id): a conflicting create is a typed 409. */
static int create_decision(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   const char *subject = jstr(req, "subject");
   const char *options = jstr(req, "options");
   const char *chosen = jstr(req, "chosen");
   /* jstr returns "" as non-NULL — require genuinely non-empty scope + choice. */
   if (!subject || !subject[0] || !options || !options[0] || !chosen || !chosen[0])
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"bad request: non-empty subject, options, chosen required\"}");
      return 400;
   }
   const char *rationale = jstr(req, "rationale");
   const char *author = jstr(req, "author");
   const char *revisit = jstr(req, "revisit_when");
   int64_t linked_policy_id = jint(req, "linked_policy_id");
   int64_t supersedes_id = jint(req, "supersedes_id");

   /* Pre-check the one-active-per-scope invariant on the EXACT index key
    * (subject, linked_policy_id) so a conflict is a clear 409 and a different
    * linked policy is not falsely rejected. The DB index remains authoritative. */
   if (supersedes_id == 0)
   {
      int64_t active = db2_decision_log_active_id(subject, linked_policy_id);
      if (active > 0)
      {
         cJSON_Delete(req);
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"conflict: an active decision already exists for this scope; "
                  "supersede it instead\",\"active_id\":%lld}",
                  (long long)active);
         return 409;
      }
   }

   db2_decision_log_row_t out;
   int rc = db2_decision_log_record(subject, options, chosen, rationale ? rationale : "",
                                    author ? author : "", linked_policy_id, revisit ? revisit : "",
                                    supersedes_id, &out);
   cJSON_Delete(req);
   if (rc != 0)
   {
      /* record() can fail from: a race that filled the scope after the pre-check
       * (=> genuine 409), a stale/wrong-scope supersedes_id (=> 400), or a store
       * error (=> 503). Re-check the scope to pick the accurate code. */
      if (supersedes_id == 0 && db2_decision_log_active_id(subject, linked_policy_id) > 0)
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"conflict: an active decision already exists for this scope\"}");
         return 409;
      }
      if (supersedes_id > 0)
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"bad request: supersedes_id is not an active decision in this "
                  "scope\"}");
         return 400;
      }
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"decision store unavailable\"}");
      return 503;
   }
   /* Sanitize the client subject before interpolating into the audit line
    * (strip CR/LF/TAB) to prevent audit-log forgery. */
   char safe_subj[256];
   size_t si = 0;
   for (const char *p = out.subject; *p && si + 1 < sizeof(safe_subj); p++)
      safe_subj[si++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
   safe_subj[si] = '\0';
   audit_log("console_decision_create", "id=%lld subject=%s supersedes=%lld", (long long)out.id,
             safe_subj, (long long)supersedes_id);
   return emit(decision_to_json(&out), out_buf, out_cap, 201);
}

/* POST /v1/decisions/{id}/{outcome|status|revisit} — single-field updates. */
static int update_decision(int64_t id, const char *action, const char *body, char *out_buf,
                           int out_cap)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   const char *val = jstr(req, strcmp(action, "revisit") == 0 ? "revisit_when" : action);
   if (!val)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"bad request: %s (string) required\"}",
               strcmp(action, "revisit") == 0 ? "revisit_when" : action);
      return 400;
   }
   /* status is a closed vocabulary — an arbitrary value would corrupt the
    * active/superseded state model the one-active-per-scope index depends on. */
   if (strcmp(action, "status") == 0 && strcmp(val, "active") != 0 &&
       strcmp(val, "superseded") != 0 && strcmp(val, "revisit_due") != 0)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"bad request: status must be active|superseded|revisit_due\"}");
      return 400;
   }
   int rc;
   if (strcmp(action, "outcome") == 0)
      rc = db2_decision_log_set_outcome(id, val);
   else if (strcmp(action, "status") == 0)
      rc = db2_decision_log_set_status(id, val);
   else
      rc = db2_decision_log_set_revisit(id, val);
   cJSON_Delete(req);
   if (rc != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"decision not found\"}");
      return 404;
   }
   audit_log("console_decision_update", "id=%lld field=%s", (long long)id, action);
   db2_decision_log_row_t d;
   if (db2_decision_log_get(id, &d) == 0)
      return emit(decision_to_json(&d), out_buf, out_cap, 200);
   snprintf(out_buf, (size_t)out_cap, "{\"ok\":true}");
   return 200;
}

/* POST /v1/decisions/{id}/supersede — the body is the NEW decision that replaces
 * {id}; delegates to create with supersedes_id pinned to {id}. */
static int supersede_decision(int64_t id, const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   if (!req)
      req = cJSON_CreateObject();
   cJSON_DeleteItemFromObjectCaseSensitive(req, "supersedes_id");
   cJSON_AddNumberToObject(req, "supersedes_id", (double)id);
   char *merged = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   int rc = create_decision(merged ? merged : "{}", out_buf, out_cap);
   free(merged);
   return rc;
}

/* GET /v1/audit/actions?since=&until=&scope_kind=&limit= — since is REQUIRED. */
static int list_audit(const char *qs, char *out_buf, int out_cap)
{
   char since[32], until[32], scope[32], limit_s[16];
   qparam(qs, "since", since, sizeof(since));
   qparam(qs, "until", until, sizeof(until));
   qparam(qs, "scope_kind", scope, sizeof(scope));
   qparam(qs, "limit", limit_s, sizeof(limit_s));
   if (!since[0])
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"bad request: a 'since' time-window bound is required\"}");
      return 400;
   }
   int limit = limit_s[0] ? atoi(limit_s) : 100;
   db2_audit_event_row_t rows[AUDIT_LIST_MAX];
   int n = db2_audit_event_list(since, until[0] ? until : NULL, scope[0] ? scope : NULL, limit,
                                rows, AUDIT_LIST_MAX);
   if (n < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"audit unavailable\"}");
      return 503;
   }
   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "actions");
   for (int i = 0; i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "id", rows[i].id);
      cJSON_AddStringToObject(o, "target_surface", rows[i].target_surface);
      cJSON_AddStringToObject(o, "target_id", rows[i].target_id);
      cJSON_AddStringToObject(o, "operator_id", rows[i].operator_id);
      cJSON_AddStringToObject(o, "scope_kind", rows[i].scope_kind);
      cJSON_AddStringToObject(o, "scope_id", rows[i].scope_id);
      cJSON_AddStringToObject(o, "applied_at", rows[i].applied_at);
      cJSON_AddNumberToObject(o, "applied_confidence", rows[i].applied_confidence);
      cJSON_AddBoolToObject(o, "flagged_for_review", rows[i].flagged_for_review != 0);
      cJSON_AddItemToArray(arr, o);
   }
   cJSON_AddNumberToObject(root, "count", n);
   return emit(root, out_buf, out_cap, 200);
}

int kb_http_governance_route(const char *method, const char *path, const char *query_string,
                             const char *body, char *out_buf, int out_cap)
{
   if (strcmp(path, "/v1/audit/actions") == 0)
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      return list_audit(query_string, out_buf, out_cap);
   }
   if (strcmp(path, "/v1/decisions") == 0)
   {
      if (strcmp(method, "GET") == 0)
         return list_decisions(query_string, out_buf, out_cap);
      if (strcmp(method, "POST") == 0)
         return create_decision(body, out_buf, out_cap);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }
   if (strncmp(path, "/v1/decisions/", 14) == 0)
   {
      long long id = 0;
      char action[24] = "";
      char extra = 0;
      /* The trailing %c must NOT match — anchors the path so
       * /v1/decisions/7/supersede/extra (matched==3) is rejected, not dispatched. */
      int matched = sscanf(path, "/v1/decisions/%lld/%23[a-z]%c", &id, action, &extra);
      if (matched == 1 && id > 0)
      {
         /* GET /v1/decisions/{id} */
         if (strcmp(method, "GET") != 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
            return 405;
         }
         return get_decision((int64_t)id, out_buf, out_cap);
      }
      if (matched == 2 && id > 0 && strcmp(method, "POST") == 0)
      {
         if (strcmp(action, "supersede") == 0)
            return supersede_decision((int64_t)id, body, out_buf, out_cap);
         if (strcmp(action, "outcome") == 0 || strcmp(action, "status") == 0 ||
             strcmp(action, "revisit") == 0)
            return update_decision((int64_t)id, action, body, out_buf, out_cap);
      }
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"bad decision route\"}");
      return 400;
   }
   return -1; /* not a governance route */
}
