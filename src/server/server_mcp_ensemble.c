/* server_mcp_ensemble.c: MCP ensemble (multi-agent session) tool handlers */
#include "server_mcp_ensemble.h"
#include "aimee.h"
#include "db1.h"
#include "headers/primary_session_adapter.h"
#include "server_http.h"
#include "agent_config.h"
#include "persona.h"
#include "role_templates.h"
#include "util.h" /* safe_strdup */
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static cJSON *ensemble_text_content(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

static cJSON *ensemble_summary_content(const ensemble_info_t *info, const char *prompt,
                                       const char *context)
{
   char buf[4096];
   int pos = 0;
   pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "ensemble #%d\n", info->id);
   pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "template: %s\n", info->template_name);
   pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "channel: %s\n", info->channel);
   pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "status: %s\n", info->status);
   if (info->phase_count > 0)
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "phase: %s (%d/%d)\n", info->phase_name,
                      info->current_phase + 1, info->phase_count);
   if (strcmp(info->status, "complete") != 0 && info->turns_in_phase > 0)
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "turn: %d/%d\n", info->current_turn + 1,
                      info->turns_in_phase);
   if (info->expected_agent[0])
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "expected: %s [%s]\n",
                      info->expected_agent, info->expected_role);
   if (info->paused_reason[0])
      pos +=
          snprintf(buf + pos, sizeof(buf) - (size_t)pos, "pause_reason: %s\n", info->paused_reason);
   if (context && context[0] && pos < (int)sizeof(buf) - 64)
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\nrecent context:\n%s", context);
   if (prompt && prompt[0] && pos < (int)sizeof(buf) - 64)
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\nnext prompt:\n%s\n", prompt);
   return ensemble_text_content(buf);
}

static cJSON *ensemble_assignments_from_args(cJSON *args, char *err, size_t errlen)
{
   cJSON *src = cJSON_GetObjectItemCaseSensitive(args, "assignments");
   if (!cJSON_IsObject(src))
   {
      snprintf(err, errlen, "missing 'assignments' object");
      return NULL;
   }

   cJSON *root = cJSON_CreateObject();
   if (!root)
   {
      snprintf(err, errlen, "out of memory");
      return NULL;
   }

   for (cJSON *child = src->child; child; child = child->next)
   {
      cJSON *arr = cJSON_CreateArray();
      if (!arr)
      {
         cJSON_Delete(root);
         snprintf(err, errlen, "out of memory");
         return NULL;
      }

      if (cJSON_IsString(child) && child->valuestring && child->valuestring[0])
      {
         cJSON_AddItemToArray(arr, cJSON_CreateString(child->valuestring));
      }
      else if (cJSON_IsArray(child))
      {
         for (cJSON *item = child->child; item; item = item->next)
         {
            if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0])
            {
               cJSON_Delete(arr);
               cJSON_Delete(root);
               snprintf(err, errlen, "assignments.%s must be a string or array of strings",
                        child->string ? child->string : "(unknown)");
               return NULL;
            }
            cJSON_AddItemToArray(arr, cJSON_CreateString(item->valuestring));
         }
      }
      else
      {
         cJSON_Delete(arr);
         cJSON_Delete(root);
         snprintf(err, errlen, "assignments.%s must be a string or array of strings",
                  child->string ? child->string : "(unknown)");
         return NULL;
      }

      if (cJSON_GetArraySize(arr) == 0)
      {
         cJSON_Delete(arr);
         cJSON_Delete(root);
         snprintf(err, errlen, "assignments.%s must not be empty",
                  child->string ? child->string : "(unknown)");
         return NULL;
      }

      cJSON_AddItemToObject(root, child->string, arr);
   }

   return root;
}

/* Canonical ensemble tool names are ensemble_*; the legacy session_* spellings
 * remain as back-compat aliases. Map either spelling to the canonical name so
 * the dispatch below only tests ensemble_*. Returns a pointer valid for the
 * duration of the call (thread-local scratch for the alias rewrite). */
static const char *ensemble_tool_canonical(const char *tool)
{
   if (tool && strncmp(tool, "session_", 8) == 0)
   {
      static _Thread_local char buf[64];
      snprintf(buf, sizeof buf, "ensemble_%s", tool + 8);
      return buf;
   }
   return tool ? tool : "";
}

int server_mcp_is_ensemble_tool(const char *tool)
{
   const char *t = ensemble_tool_canonical(tool);
   return strcmp(t, "ensemble_start") == 0 || strcmp(t, "ensemble_status") == 0 ||
          strcmp(t, "ensemble_pause") == 0 || strcmp(t, "ensemble_advance") == 0 ||
          strcmp(t, "ensemble_list") == 0;
}

int server_mcp_handle_ensemble_tool(server_conn_t *conn, const char *tool, cJSON *args,
                                    cJSON **content_out, cJSON **structured_out)
{
   const char *t = ensemble_tool_canonical(tool);
   cJSON *content = NULL;
   cJSON *structured = NULL;

   if (strcmp(t, "ensemble_start") == 0 || strcmp(t, "ensemble_status") == 0 ||
       strcmp(t, "ensemble_pause") == 0 || strcmp(t, "ensemble_advance") == 0)
   {
      ensemble_info_t info;
      char *prompt = NULL;
      char *context = NULL;
      char errbuf[256] = "";
      int rc = -1;

      if (strcmp(t, "ensemble_start") == 0)
      {
         cJSON *jt = cJSON_GetObjectItemCaseSensitive(args, "template");
         cJSON *jc = cJSON_GetObjectItemCaseSensitive(args, "channel");
         if (!cJSON_IsString(jt) || !jt->valuestring[0])
         {
            snprintf(errbuf, sizeof(errbuf), "missing 'template' parameter");
         }
         else
         {
            cJSON *assignments = ensemble_assignments_from_args(args, errbuf, sizeof(errbuf));
            if (assignments)
            {
               char cwd[MAX_PATH_LEN];
               int id = 0;
               if (!getcwd(cwd, sizeof(cwd)))
                  cwd[0] = '\0';
               rc = db1_ensemble_create(cwd, jt->valuestring,
                                        (cJSON_IsString(jc) && jc->valuestring[0]) ? jc->valuestring
                                                                                   : "general",
                                        assignments, &id, errbuf, sizeof(errbuf));
               cJSON_Delete(assignments);
               if (rc == 0)
                  rc = db1_ensemble_get(id, &info, &prompt, &context, errbuf, sizeof(errbuf));
            }
         }
      }
      else if (strcmp(t, "ensemble_status") == 0)
      {
         cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "id");
         if (!cJSON_IsNumber(jid))
            snprintf(errbuf, sizeof(errbuf), "missing 'id' parameter");
         else
            rc = db1_ensemble_get(jid->valueint, &info, &prompt, &context, errbuf, sizeof(errbuf));
      }
      else if (strcmp(t, "ensemble_pause") == 0)
      {
         cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "id");
         cJSON *jr = cJSON_GetObjectItemCaseSensitive(args, "reason");
         if (!cJSON_IsNumber(jid))
            snprintf(errbuf, sizeof(errbuf), "missing 'id' parameter");
         else if (db1_ensemble_pause(jid->valueint,
                                     (cJSON_IsString(jr) && jr->valuestring[0]) ? jr->valuestring
                                                                                : "manual",
                                     errbuf, sizeof(errbuf)) == 0)
            rc = db1_ensemble_get(jid->valueint, &info, &prompt, &context, errbuf, sizeof(errbuf));
      }
      else
      {
         cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "id");
         cJSON *js = cJSON_GetObjectItemCaseSensitive(args, "speaker");
         cJSON *jm = cJSON_GetObjectItemCaseSensitive(args, "message");
         if (!cJSON_IsNumber(jid))
            snprintf(errbuf, sizeof(errbuf), "missing 'id' parameter");
         else if (!cJSON_IsString(js) || !js->valuestring[0])
            snprintf(errbuf, sizeof(errbuf), "missing 'speaker' parameter");
         else if (db1_ensemble_advance(jid->valueint, js->valuestring,
                                       cJSON_IsString(jm) ? jm->valuestring : "", &info, &prompt,
                                       errbuf, sizeof(errbuf)) == 0)
         {
            free(context);
            context = NULL;
            rc = db1_ensemble_get(jid->valueint, &info, NULL, &context, errbuf, sizeof(errbuf));
         }
      }

      if (rc != 0)
      {
         free(prompt);
         free(context);
         return server_send_error(conn, errbuf[0] ? errbuf : "ensemble operation failed", NULL);
      }

      content = ensemble_summary_content(&info, prompt, context);
      structured = db1_ensemble_info_to_json(&info, prompt, context);
      free(prompt);
      free(context);
      if (!structured)
         return server_send_error(conn, "ensemble serialization failed", NULL);
   }
   else if (strcmp(t, "ensemble_list") == 0)
   {
      cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
      int limit = cJSON_IsNumber(jl) ? jl->valueint : 20;
      char errbuf[256] = "";
      ensemble_info_t *rows = NULL;
      int n = 0;
      if (db1_ensemble_list(&rows, &n, errbuf, sizeof(errbuf)) != 0)
         return server_send_error(conn, errbuf[0] ? errbuf : "ensemble list failed", NULL);
      if (limit < 0)
         limit = 0;
      if (limit > 100)
         limit = 100;
      if (n < limit)
         limit = n;
      structured = cJSON_CreateObject();
      if (!structured)
      {
         free(rows);
         return server_send_error(conn, "ensemble serialization failed", NULL);
      }
      cJSON *sessions = cJSON_AddArrayToObject(structured, "sessions");
      if (!sessions)
      {
         free(rows);
         cJSON_Delete(structured);
         return server_send_error(conn, "ensemble serialization failed", NULL);
      }
      for (int i = 0; i < limit; i++)
      {
         cJSON *item = db1_ensemble_info_to_json(&rows[i], NULL, NULL);
         if (!item)
         {
            free(rows);
            cJSON_Delete(structured);
            return server_send_error(conn, "ensemble serialization failed", NULL);
         }
         cJSON_AddItemToArray(sessions, item);
      }
      free(rows);
      cJSON_AddNumberToObject(structured, "count", limit);
      char summary[128];
      snprintf(summary, sizeof(summary), "ensembles: %d", limit);
      content = ensemble_text_content(summary);
   }

   if (!content)
      return server_send_error(conn, "ensemble operation failed", NULL);

   *content_out = content;
   *structured_out = structured;
   return 0;
}

cJSON *server_mcp_compact_context(const char *session_id)
{
   if (!session_id || !session_id[0])
      return ensemble_text_content("error: compact_context requires an active primary session");

   session_compact_result_t sc_result;
   char errbuf[256] = {0};
   int rc = primary_session_adapter_compact_by_session_id(session_id, &sc_result, errbuf,
                                                          sizeof(errbuf));
   if (rc != 0)
   {
      char out[512];
      snprintf(out, sizeof(out), "compact_context failed: %s",
               errbuf[0] ? errbuf : "unknown error");
      return ensemble_text_content(out);
   }
   if (!sc_result.compacted)
      return ensemble_text_content("compact_context: context is not near the limit");

   char out[SESSION_COMPACT_SUMMARY_MAX + 512];
   snprintf(out, sizeof(out), "compact_context: %d→%d messages (%d removed)\n\n%s",
            sc_result.messages_before, sc_result.messages_after, sc_result.messages_removed,
            sc_result.summary);
   return ensemble_text_content(out);
}

cJSON *server_mcp_set_primary_agent(const char *session_id, cJSON *args)
{
   if (!session_id || !session_id[0])
      return ensemble_text_content("error: set_primary_agent requires an active session");

   cJSON *jclear = args ? cJSON_GetObjectItemCaseSensitive(args, "clear") : NULL;
   if (cJSON_IsTrue(jclear))
   {
      session_primary_clear(session_id);
      return ensemble_text_content(
          "primary agent cleared; this session reverts to the default provider");
   }

   cJSON *jagent = args ? cJSON_GetObjectItemCaseSensitive(args, "agent") : NULL;
   if (!cJSON_IsString(jagent) || !jagent->valuestring[0])
      return ensemble_text_content(
          "error: set_primary_agent requires an 'agent' name (or clear:true)");

   const char *name = jagent->valuestring;
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0 || !agent_find(&acfg, name))
   {
      char out[256];
      snprintf(out, sizeof(out), "error: no such agent '%s' (see 'aimee agent list')", name);
      return ensemble_text_content(out);
   }

   session_primary_set(session_id, name);
   char out[256];
   snprintf(out, sizeof(out), "primary agent set to '%s' for this session", name);
   return ensemble_text_content(out);
}

cJSON *server_mcp_upsert_persona(cJSON *args)
{
   cJSON *jname = args ? cJSON_GetObjectItemCaseSensitive(args, "name") : NULL;
   if (!cJSON_IsString(jname) || !persona_name_valid(jname->valuestring))
      return ensemble_text_content("error: upsert_persona requires a valid 'name' "
                                   "(letters, digits, '.', '_', '-')");

   persona_t p;
   memset(&p, 0, sizeof(p));
   snprintf(p.name, sizeof(p.name), "%s", jname->valuestring);

   cJSON *d = cJSON_GetObjectItemCaseSensitive(args, "description");
   if (cJSON_IsString(d))
      snprintf(p.description, sizeof(p.description), "%s", d->valuestring);
   cJSON *dg = cJSON_GetObjectItemCaseSensitive(args, "delegates");
   if (cJSON_IsString(dg) && dg->valuestring[0])
      snprintf(p.delegates, sizeof(p.delegates), "%s", dg->valuestring);
   else
      snprintf(p.delegates, sizeof(p.delegates), "%s", PERSONA_DELEGATES_FULL);
   cJSON *cr = cJSON_GetObjectItemCaseSensitive(args, "check_role");
   if (cJSON_IsString(cr))
      snprintf(p.check_role, sizeof(p.check_role), "%s", cr->valuestring);
   cJSON *cm = cJSON_GetObjectItemCaseSensitive(args, "check_marker");
   if (cJSON_IsString(cm))
      snprintf(p.check_marker, sizeof(p.check_marker), "%s", cm->valuestring);
   cJSON *roles = cJSON_GetObjectItemCaseSensitive(args, "roles");
   if (cJSON_IsArray(roles))
   {
      cJSON *r = NULL;
      cJSON_ArrayForEach(r, roles)
      {
         if (cJSON_IsString(r) && p.roles_count < PERSONA_MAX_ROLES)
            snprintf(p.roles[p.roles_count++], PERSONA_NAME_MAX, "%s", r->valuestring);
      }
   }
   cJSON *pt = cJSON_GetObjectItemCaseSensitive(args, "persona");
   if (cJSON_IsString(pt) && pt->valuestring[0])
      p.persona_text = safe_strdup(pt->valuestring);
   cJSON *pr = cJSON_GetObjectItemCaseSensitive(args, "principles");
   if (cJSON_IsString(pr) && pr->valuestring[0])
      p.principles_text = safe_strdup(pr->valuestring);
   cJSON *br = cJSON_GetObjectItemCaseSensitive(args, "brief");
   if (cJSON_IsString(br) && br->valuestring[0])
      p.brief_text = safe_strdup(br->valuestring);

   int rc = persona_write(&p);
   char saved_name[PERSONA_NAME_MAX];
   snprintf(saved_name, sizeof(saved_name), "%s", p.name);
   persona_free(&p);
   if (rc != 0)
      return ensemble_text_content("error: failed to write persona file");

   char out[256];
   snprintf(out, sizeof(out),
            "persona '%s' written to user config; it is the source of truth on the next load",
            saved_name);
   return ensemble_text_content(out);
}

cJSON *server_mcp_upsert_role_template(cJSON *args)
{
   cJSON *jrole = args ? cJSON_GetObjectItemCaseSensitive(args, "role") : NULL;
   if (!cJSON_IsString(jrole) || !role_template_name_valid(jrole->valuestring))
      return ensemble_text_content("error: upsert_role_template requires a valid 'role' name "
                                   "(letters, digits, '.', '_', '-')");
   cJSON *jc = args ? cJSON_GetObjectItemCaseSensitive(args, "content") : NULL;
   if (!cJSON_IsString(jc))
      return ensemble_text_content(
          "error: upsert_role_template requires 'content' (the template body, with optional "
          "{{TASK}} and {{CONTEXT}} placeholders)");

   if (role_template_write(jrole->valuestring, jc->valuestring) != 0)
      return ensemble_text_content("error: failed to write role template (bad name or too large)");

   char out[256];
   snprintf(out, sizeof(out),
            "role template '%s' written; delegates of that role use it on the next run",
            jrole->valuestring);
   return ensemble_text_content(out);
}
