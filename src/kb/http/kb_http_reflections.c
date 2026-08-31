/* kb_http_reflections.c: aimee-kb reflection and in-session feedback HTTP handlers.
 *
 * Implements Phase 6 of docs/proposals/accepted/aimee-kb-service-and-public-api.md:
 *   POST /v1/reflections            — batch reflection proposals from aimee-server
 *   GET  /v1/reflections            — list proposed artifacts
 *   POST /v1/reflections/{id}/accept — thumbs-up
 *   POST /v1/reflections/{id}/reject — thumbs-down
 *   POST /v1/feedback/in-session    — immediate in-session feedback
 *
 * Candidate generation and per-surface review CLIs are owned by
 * cross-source-learning-substrate; these handlers are the storage transport only.
 */

#include "kb_http_reflections.h"
#include "modules/db2/c/artifacts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── JSON helpers ─────────────────────────────────────────────────────────── */

static int json_str(const char *json, const char *key, char *out, int cap)
{
   if (!json || !key)
      return 0;
   char needle[256];
   snprintf(needle, sizeof(needle), "\"%s\":\"", key);
   const char *p = strstr(json, needle);
   if (!p)
      return 0;
   p += strlen(needle);
   int i = 0;
   while (*p && *p != '"' && i < cap - 1)
   {
      if (*p == '\\' && *(p + 1))
         p++;
      out[i++] = *p++;
   }
   out[i] = '\0';
   return i > 0;
}

static double json_double(const char *json, const char *key, double def)
{
   if (!json)
      return def;
   char needle[256];
   snprintf(needle, sizeof(needle), "\"%s\":", key);
   const char *p = strstr(json, needle);
   if (!p)
      return def;
   p += strlen(needle);
   while (*p == ' ')
      p++;
   return atof(p);
}

static int json_object_field(const char *json, const char *key, char *out, int cap)
{
   if (!json || !key)
      return 0;
   char needle[256];
   snprintf(needle, sizeof(needle), "\"%s\":{", key);
   const char *p = strstr(json, needle);
   if (!p)
      return 0;
   p += strlen(needle) - 1;
   int depth = 0;
   int i = 0;
   while (*p && i < cap - 1)
   {
      if (*p == '{')
         depth++;
      else if (*p == '}')
      {
         depth--;
         if (depth == 0)
         {
            out[i++] = '}';
            break;
         }
      }
      out[i++] = *p++;
   }
   out[i] = '\0';
   return i > 0;
}

static int query_param(const char *qs, const char *key, char *out, int cap)
{
   if (!qs || !key)
      return 0;
   char needle[256];
   snprintf(needle, sizeof(needle), "%s=", key);
   const char *p = strstr(qs, needle);
   if (!p)
      return 0;
   p += strlen(needle);
   int i = 0;
   while (*p && *p != '&' && i < cap - 1)
      out[i++] = *p++;
   out[i] = '\0';
   return i > 0;
}

/* ── POST /v1/reflections ─────────────────────────────────────────────────── */

int handle_post_reflections(const char *body, int body_len, char *out_buf, int out_cap)
{
   (void)body_len;
   if (!body)
      return 400;

   /* Walk the "entries" array.  Simple state-machine: find each '{' after
    * "entries":[  and parse until matching '}'. */
   const char *entries_start = strstr(body, "\"entries\":[");
   if (!entries_start)
      return 400;
   entries_start += strlen("\"entries\":[");

   int created = 0;
   const char *p = entries_start;

   while (*p)
   {
      while (*p && *p != '{' && *p != ']')
         p++;
      if (!*p || *p == ']')
         break;

      /* Find matching '}' with depth tracking */
      const char *entry_start = p;
      int depth = 0;
      while (*p)
      {
         if (*p == '{')
            depth++;
         else if (*p == '}')
         {
            depth--;
            if (depth == 0)
            {
               p++;
               break;
            }
         }
         p++;
      }
      int entry_len = (int)(p - entry_start);
      if (entry_len <= 2)
         continue;

      char entry[8192];
      if (entry_len >= (int)sizeof(entry))
         continue;
      memcpy(entry, entry_start, (size_t)entry_len);
      entry[entry_len] = '\0';

      char kind[64] = {0};
      char scope_user[128] = {0};
      char scope_project[128] = {0};
      char payload[4096] = {0};

      json_str(entry, "kind", kind, sizeof(kind));
      json_str(entry, "scope_user", scope_user, sizeof(scope_user));
      json_str(entry, "scope_project", scope_project, sizeof(scope_project));
      double confidence = json_double(entry, "confidence", 0.5);
      json_object_field(entry, "payload", payload, sizeof(payload));
      if (!payload[0])
         json_str(entry, "payload", payload, sizeof(payload));

      if (!kind[0])
         continue;

      char id[37];
      db2_artifact_gen_id(id, sizeof(id));

      const char *scope_kind = scope_user[0] ? "user" : (scope_project[0] ? "project" : "global");
      const char *scope_id = scope_user[0] ? scope_user : scope_project;
      const char *payload_json = payload[0] ? payload : "{}";

      if (db2_artifact_write(id, kind, "proposed", scope_kind, scope_id, NULL, confidence,
                             payload_json) == 0)
         created++;
   }

   if (created == 0)
      return 400;

   snprintf(out_buf, out_cap, "{\"created\":%d}", created);
   return 201;
}

/* ── GET /v1/reflections ──────────────────────────────────────────────────── */

int handle_get_reflections(const char *query_string, char *out_buf, int out_cap)
{
   char kind_filter[64] = {0};
   char limit_str[16] = {0};
   int limit = 20;

   if (query_string)
   {
      query_param(query_string, "kind", kind_filter, sizeof(kind_filter));
      if (query_param(query_string, "limit", limit_str, sizeof(limit_str)))
         limit = atoi(limit_str);
   }
   if (limit <= 0 || limit > 100)
      limit = 20;

   db2_artifact_proposed_t rows[100];
   int n = db2_artifact_list_proposed(NULL, limit, rows, limit < 100 ? limit : 100);
   if (n < 0)
      n = 0;

   /* Filter by kind if requested */
   int pos = 0;
   pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "{\"items\":[");
   int first = 1;
   for (int i = 0; i < n && pos < out_cap - 2; i++)
   {
      if (kind_filter[0] && strcmp(rows[i].kind, kind_filter) != 0)
         continue;
      if (!first)
         pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), ",");
      first = 0;
      pos += snprintf(out_buf + pos, (size_t)(out_cap - pos),
                      "{\"id\":\"%s\",\"kind\":\"%s\",\"confidence\":%.3f,"
                      "\"created_at\":\"%s\",\"payload\":%s}",
                      rows[i].id, rows[i].kind, rows[i].confidence, rows[i].created_at,
                      rows[i].payload_json[0] ? rows[i].payload_json : "{}");
   }
   pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "]}");
   return 200;
}

/* ── POST /v1/reflections/{id}/accept ────────────────────────────────────── */

int handle_post_reflection_accept(const char *artifact_id, const char *body, int body_len,
                                  char *out_buf, int out_cap)
{
   (void)body_len;
   if (!artifact_id || !artifact_id[0])
      return 400;

   if (db2_artifact_set_state(artifact_id, "committed") != 0)
      return 404;

   char notes[256] = {0};
   if (body)
      json_str(body, "notes", notes, sizeof(notes));

   char audit_id[37];
   db2_artifact_gen_id(audit_id, sizeof(audit_id));
   db2_audit_event_write(audit_id, artifact_id, NULL, NULL, NULL, NULL, NULL, 1.0, 0, NULL, NULL);

   snprintf(out_buf, out_cap, "{\"id\":\"%s\",\"state\":\"committed\"}", artifact_id);
   return 200;
}

/* ── POST /v1/reflections/{id}/reject ────────────────────────────────────── */

int handle_post_reflection_reject(const char *artifact_id, const char *body, int body_len,
                                  char *out_buf, int out_cap)
{
   (void)body_len;
   if (!artifact_id || !artifact_id[0])
      return 400;

   char verdict_tag[64] = {0};
   char verdict_scope[64] = {0};
   char counter_example[512] = {0};

   if (body)
   {
      json_str(body, "verdict_tag", verdict_tag, sizeof(verdict_tag));
      json_str(body, "verdict_scope", verdict_scope, sizeof(verdict_scope));
      json_str(body, "counter_example", counter_example, sizeof(counter_example));
   }

   if (db2_artifact_reject(artifact_id, verdict_tag[0] ? verdict_tag : NULL,
                           verdict_scope[0] ? verdict_scope : NULL,
                           counter_example[0] ? counter_example : NULL, NULL) != 0)
      return 404;

   snprintf(out_buf, out_cap, "{\"id\":\"%s\",\"state\":\"rejected\"}", artifact_id);
   return 200;
}

/* ── POST /v1/feedback/in-session ────────────────────────────────────────── */

int handle_post_feedback_in_session(const char *body, int body_len, char *out_buf, int out_cap)
{
   (void)body_len;
   if (!body)
      return 400;

   char session_id[128] = {0};
   char turn_id[128] = {0};
   char kind[64] = {0};
   char scope_user[128] = {0};
   char content[2048] = {0};

   json_str(body, "session_id", session_id, sizeof(session_id));
   json_str(body, "turn_id", turn_id, sizeof(turn_id));
   json_str(body, "kind", kind, sizeof(kind));
   json_str(body, "scope_user", scope_user, sizeof(scope_user));
   json_str(body, "content", content, sizeof(content));

   if (!kind[0] ||
       (strcmp(kind, "feedback_negative") != 0 && strcmp(kind, "feedback_positive") != 0))
      return 400;

   char payload[2560];
   snprintf(payload, sizeof(payload),
            "{\"session_id\":\"%s\",\"turn_id\":\"%s\",\"content\":\"%s\"}", session_id, turn_id,
            content);

   char id[37];
   db2_artifact_gen_id(id, sizeof(id));

   if (db2_artifact_write(id, kind, "committed", scope_user[0] ? "user" : "global",
                          scope_user[0] ? scope_user : "anon", NULL, 1.0, payload) != 0)
      return 500;

   if (session_id[0])
      db2_artifact_cite(id, "session", session_id);

   snprintf(out_buf, out_cap, "{\"id\":\"%s\",\"kind\":\"%s\",\"state\":\"committed\"}", id, kind);
   return 201;
}
