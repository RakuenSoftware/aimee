/* server_mcp_call_table.c: split from server_mcp.c into a real translation unit
 * (was server_mcp_call_table.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "server_mcp_internal.h"
#include "server.h"
#include "agent_tools.h" /* the native surface this table registers into */
#include "toolset.h"     /* toolset_register_native_tool */
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "dstr.h"
#include "commands.h"
#include "db2/curiosity.h"
#include "memory.h"
#include "index.h"
#include "code_span.h"
#include "db1.h"
#include "kb_client.h"
#include "config.h"
#include "dashboard.h"
#include "work_queue.h"
#include "mcp_tools.h"
#include "mcp_git.h"
#include "git_verify.h"
#include "workspace_turn.h"
#include "notes.h"
#include "agent_coord.h"
#include "agent_tasks.h"
#include "agent_pipeline.h"
#include "delegate_economics.h"
#include "delegate_patch_coordinator.h"
#include "platform_path.h"
#include "lsp.h"
#include "server_mcp_learning.h"
#include "server_mcp_process.h"
#include "server_mcp_skill.h"
#include "server_mcp_delegate.h"
#include "server_mcp_ensemble.h"
#include "wfe_advance_exec.h"  /* advance_request interactive-driver executor (S2) */
#include "wfe_block_resolve.h" /* per-block externalization guard (S2 sub-slice 4) */
#include "server_mcp_gateway.h"
#include "server_http.h"
#include "server_pipeline.h" /* handle_pipeline_* for the pipeline.* MCP tools */
#include "headers/conversation_context.h"
#include "headers/payload_rewrite.h"
#include "headers/session_search_tool.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <stdarg.h>
#include "agent_help_data.h"

/* Per-call bundle passed to every handler: the request context plus the
 * out-param for tools that emit an MCP `structured` payload alongside text. */

/* ── thin wrappers: uniform signature over the existing tool_* helpers ─────── */

static cJSON *mcph_get_help(struct mcp_call *c)
{
   return tool_get_help(c->jargs);
}
static cJSON *mcph_ast_grep_search(struct mcp_call *c)
{
   return tool_ast_grep_search(c->jargs);
}
static cJSON *mcph_search_memory(struct mcp_call *c)
{
   return tool_search_memory(c->jargs);
}
static cJSON *mcph_mutate(struct mcp_call *c)
{
   return tool_memory_mutate(c->jargs);
}
static cJSON *mcph_memory_ask(struct mcp_call *c)
{
   return tool_memory_ask(c->jargs, c->structured);
}
static cJSON *mcph_search_graph(struct mcp_call *c)
{
   return tool_search_graph(c->jargs);
}
static cJSON *mcph_get_episode(struct mcp_call *c)
{
   return tool_get_episode(c->jargs);
}
static cJSON *mcph_get_entity(struct mcp_call *c)
{
   return tool_get_entity(c->jargs);
}
static cJSON *mcph_get_entity_edges(struct mcp_call *c)
{
   return tool_get_entity_edges(c->jargs);
}
static cJSON *mcph_get_context_block(struct mcp_call *c)
{
   return tool_get_context_block(c->jargs);
}
static cJSON *mcph_memory_get(struct mcp_call *c)
{
   return tool_memory_get(c->jargs);
}
static cJSON *mcph_list_facts(struct mcp_call *c)
{
   (void)c;
   return tool_list_facts();
}
static cJSON *mcph_memory_briefing(struct mcp_call *c)
{
   return tool_memory_briefing(c->jargs);
}
static cJSON *mcph_get_identity(struct mcp_call *c)
{
   (void)c;
   return tool_get_identity();
}
static cJSON *mcph_list_curiosity_items(struct mcp_call *c)
{
   return tool_list_curiosity_items(c->jargs);
}
static cJSON *mcph_create_prospective_memory(struct mcp_call *c)
{
   return tool_create_prospective_memory(c->jargs);
}
static cJSON *mcph_list_prospective_memories(struct mcp_call *c)
{
   return tool_list_prospective_memories(c->jargs);
}
static cJSON *mcph_complete_prospective_memory(struct mcp_call *c)
{
   return tool_complete_prospective_memory(c->jargs);
}
static cJSON *mcph_get_host(struct mcp_call *c)
{
   return tool_get_host(c->jargs);
}
static cJSON *mcph_list_hosts(struct mcp_call *c)
{
   (void)c;
   return tool_list_hosts();
}
static cJSON *mcph_find_symbol(struct mcp_call *c)
{
   return smcp_tool_find_symbol(c->jargs);
}
static cJSON *mcph_preview_blast_radius(struct mcp_call *c)
{
   return tool_preview_blast_radius(c->jargs);
}
static cJSON *mcph_record_attempt(struct mcp_call *c)
{
   return tool_record_attempt(c->jargs);
}
static cJSON *mcph_list_attempts(struct mcp_call *c)
{
   return tool_list_attempts(c->jargs);
}
static cJSON *mcph_rules_propose(struct mcp_call *c)
{
   return server_mcp_tool_rules_propose(c->jargs);
}
static cJSON *mcph_rules_list(struct mcp_call *c)
{
   (void)c;
   return server_mcp_tool_rules_list();
}
static cJSON *mcph_store_workflow(struct mcp_call *c)
{
   return tool_store_workflow(c->jargs);
}
static cJSON *mcph_learning_propose(struct mcp_call *c)
{
   return server_mcp_tool_learning_propose(c->jargs);
}
static cJSON *mcph_learning_review(struct mcp_call *c)
{
   return server_mcp_tool_learning_review(c->jargs);
}
static cJSON *mcph_skill_manage(struct mcp_call *c)
{
   return server_mcp_tool_skill_manage(c->jargs);
}
static cJSON *mcph_create_note(struct mcp_call *c)
{
   return smcp_tool_create_note(c->jargs);
}
static cJSON *mcph_list_notes(struct mcp_call *c)
{
   return smcp_tool_list_notes(c->jargs);
}
static cJSON *mcph_search_notes(struct mcp_call *c)
{
   return smcp_tool_search_notes(c->jargs);
}
static cJSON *mcph_job_start(struct mcp_call *c)
{
   return tool_job_start(c->jargs);
}
static cJSON *mcph_job_status(struct mcp_call *c)
{
   return tool_job_status(c->jargs);
}
static cJSON *mcph_session_context_search(struct mcp_call *c)
{
   return json_result_content(tool_session_context_search(c->jargs));
}
static cJSON *mcph_session_context_expand(struct mcp_call *c)
{
   return json_result_content(tool_session_context_expand(c->jargs));
}
static cJSON *mcph_session_context_status(struct mcp_call *c)
{
   return json_result_content(tool_session_context_status(c->jargs));
}
static cJSON *mcph_compact_context(struct mcp_call *c)
{
   return server_mcp_compact_context(c->sid);
}
static cJSON *mcph_set_primary_agent(struct mcp_call *c)
{
   return server_mcp_set_primary_agent(c->sid, c->jargs);
}
static cJSON *mcph_upsert_persona(struct mcp_call *c)
{
   return server_mcp_upsert_persona(c->jargs);
}
static cJSON *mcph_upsert_role_template(struct mcp_call *c)
{
   return server_mcp_upsert_role_template(c->jargs);
}

/* ── extracted inline-block handlers (bodies moved verbatim) ───────────────── */

static cJSON *mcph_memory_alerts(struct mcp_call *c)
{
   cJSON *jargs = c->jargs;
   const char *since = NULL;
   cJSON *js = cJSON_GetObjectItemCaseSensitive(jargs, "since");
   if (cJSON_IsString(js) && js->valuestring[0])
      since = js->valuestring;
   char *envelope = kb_client_memory_alerts_json(since);
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *alerts = resp ? cJSON_GetObjectItemCaseSensitive(resp, "alerts") : NULL;
   char *rendered = NULL;
   if (alerts)
   {
      cJSON *detached = cJSON_DetachItemViaPointer(resp, alerts);
      rendered = detached ? cJSON_PrintUnformatted(detached) : NULL;
      cJSON_Delete(detached);
   }
   cJSON_Delete(resp);
   cJSON *content = rendered ? text_content(rendered) : text_content("error: memory_alerts failed");
   free(rendered);
   return content;
}

static cJSON *mcph_memory_recall(struct mcp_call *c)
{
   cJSON *jargs = c->jargs;
   const char *task_hint = NULL;
   cJSON *jt = cJSON_GetObjectItemCaseSensitive(jargs, "task_hint");
   if (cJSON_IsString(jt) && jt->valuestring[0])
      task_hint = jt->valuestring;
   int session_start = 0;
   cJSON *jss = cJSON_GetObjectItemCaseSensitive(jargs, "session_start");
   if (cJSON_IsBool(jss))
      session_start = cJSON_IsTrue(jss) ? 1 : 0;
   int limit_tokens = 0;
   cJSON *jl = cJSON_GetObjectItemCaseSensitive(jargs, "limit_tokens");
   if (cJSON_IsNumber(jl))
      limit_tokens = (int)jl->valuedouble;
   /* Graph-code fusion is always on for recall. */
   char *envelope = kb_client_memory_recall_json_ex(task_hint, limit_tokens, session_start, "on");
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *recall = resp ? cJSON_GetObjectItemCaseSensitive(resp, "recall") : NULL;
   char *rendered = NULL;
   if (recall)
   {
      cJSON *detached = cJSON_DetachItemViaPointer(resp, recall);
      rendered = detached ? cJSON_PrintUnformatted(detached) : NULL;
      cJSON_Delete(detached);
   }
   cJSON_Delete(resp);
   cJSON *content;
   if (!rendered)
      content = text_content("error: memory_recall failed");
   else
   {
      content = text_content(rendered);
      free(rendered);
   }
   return content;
}

static cJSON *mcph_list_epistemic_directives(struct mcp_call *c)
{
   cJSON *jargs = c->jargs;
   const char *state = NULL;
   cJSON *js = cJSON_GetObjectItemCaseSensitive(jargs, "state");
   if (cJSON_IsString(js) && js->valuestring[0])
      state = js->valuestring;
   const char *cause = NULL;
   cJSON *jc = cJSON_GetObjectItemCaseSensitive(jargs, "cause");
   if (cJSON_IsString(jc) && jc->valuestring[0])
      cause = jc->valuestring;
   int limit = 50;
   cJSON *jl = cJSON_GetObjectItemCaseSensitive(jargs, "limit");
   if (cJSON_IsNumber(jl))
      limit = (int)jl->valuedouble;
   if (limit < 1)
      limit = 1;
   if (limit > 256)
      limit = 256;
   char *envelope = kb_client_memory_directive_list_json(state, cause, limit);
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *directives = resp ? cJSON_GetObjectItemCaseSensitive(resp, "directives") : NULL;
   char *rendered = NULL;
   if (cJSON_IsArray(directives))
   {
      cJSON *detached = cJSON_DetachItemViaPointer(resp, directives);
      rendered = detached ? cJSON_PrintUnformatted(detached) : NULL;
      cJSON_Delete(detached);
   }
   cJSON_Delete(resp);
   cJSON *content = rendered ? text_content(rendered) : text_content("[]");
   free(rendered);
   return content;
}

static cJSON *mcph_create_epistemic_directive(struct mcp_call *c)
{
   cJSON *jargs = c->jargs;
   const char *question = NULL;
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(jargs, "question");
   if (cJSON_IsString(jq))
      question = jq->valuestring;
   if (!question || !question[0])
      return text_content("error: question is required");

   const char *topic = NULL, *entity = NULL, *file = NULL, *cause = NULL, *valid_until = NULL;
   cJSON *j;
   if ((j = cJSON_GetObjectItemCaseSensitive(jargs, "topic")) && cJSON_IsString(j))
      topic = j->valuestring;
   if ((j = cJSON_GetObjectItemCaseSensitive(jargs, "anchor_entity")) && cJSON_IsString(j))
      entity = j->valuestring;
   if ((j = cJSON_GetObjectItemCaseSensitive(jargs, "anchor_file")) && cJSON_IsString(j))
      file = j->valuestring;
   if ((j = cJSON_GetObjectItemCaseSensitive(jargs, "cause")) && cJSON_IsString(j) &&
       j->valuestring[0])
      cause = j->valuestring;
   if ((j = cJSON_GetObjectItemCaseSensitive(jargs, "valid_until")) && cJSON_IsString(j))
      valid_until = j->valuestring;
   int priority = 50;
   if ((j = cJSON_GetObjectItemCaseSensitive(jargs, "priority")) && cJSON_IsNumber(j))
      priority = (int)j->valuedouble;
   if (!cause)
      cause = MEMORY_DIRECTIVE_CAUSE_USER_FOLLOW_UP;

   char *envelope = kb_client_memory_directive_create_json(question, topic, entity, file, cause,
                                                           priority, "", valid_until);
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *res = cJSON_CreateObject();
   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = resp ? cJSON_GetObjectItemCaseSensitive(resp, "message") : NULL;
      cJSON_AddBoolToObject(res, "ok", 0);
      cJSON_AddStringToObject(res, "error",
                              cJSON_IsString(msg) ? msg->valuestring : "directive create failed");
   }
   else
   {
      cJSON *dedup = cJSON_GetObjectItemCaseSensitive(resp, "dedup");
      cJSON *directive = cJSON_GetObjectItemCaseSensitive(resp, "directive");
      cJSON_AddBoolToObject(res, "ok", 1);
      if (cJSON_IsBool(dedup) && cJSON_IsTrue(dedup))
         cJSON_AddBoolToObject(res, "deduped", 1);
      else if (cJSON_IsObject(directive))
      {
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(directive, "id");
         cJSON *state_j = cJSON_GetObjectItemCaseSensitive(directive, "state");
         if (cJSON_IsNumber(id_j))
            cJSON_AddNumberToObject(res, "id", id_j->valuedouble);
         if (cJSON_IsString(state_j))
            cJSON_AddStringToObject(res, "state", state_j->valuestring);
      }
   }
   cJSON_Delete(resp);
   char *rendered = cJSON_PrintUnformatted(res);
   cJSON_Delete(res);
   cJSON *content = rendered ? text_content(rendered) : text_content("{}");
   free(rendered);
   return content;
}

static cJSON *mcph_resolve_epistemic_directive(struct mcp_call *c)
{
   cJSON *jargs = c->jargs;
   int64_t id = 0;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(jargs, "id");
   if (cJSON_IsNumber(jid))
      id = (int64_t)jid->valuedouble;
   if (id <= 0)
      return text_content("error: id is required");

   int suppress = 0;
   cJSON *jsp = cJSON_GetObjectItemCaseSensitive(jargs, "suppress");
   if (cJSON_IsBool(jsp))
      suppress = cJSON_IsTrue(jsp) ? 1 : 0;
   char *envelope;
   if (suppress)
      envelope = kb_client_memory_directive_suppress_json(id);
   else
   {
      int64_t resm = 0;
      cJSON *jm = cJSON_GetObjectItemCaseSensitive(jargs, "resolution_memory_id");
      if (cJSON_IsNumber(jm))
         resm = (int64_t)jm->valuedouble;
      const char *note = NULL;
      cJSON *jn = cJSON_GetObjectItemCaseSensitive(jargs, "note");
      if (cJSON_IsString(jn))
         note = jn->valuestring;
      envelope = kb_client_memory_directive_resolve_json(id, resm, note);
   }
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0);
   cJSON *res = cJSON_CreateObject();
   cJSON_AddNumberToObject(res, "id", (double)id);
   cJSON_AddBoolToObject(res, "ok", ok);
   if (!ok)
   {
      cJSON *msg = resp ? cJSON_GetObjectItemCaseSensitive(resp, "message") : NULL;
      cJSON_AddStringToObject(res, "error",
                              cJSON_IsString(msg) ? msg->valuestring : "not open or missing");
   }
   cJSON_Delete(resp);
   char *rendered = cJSON_PrintUnformatted(res);
   cJSON_Delete(res);
   cJSON *content = rendered ? text_content(rendered) : text_content("{}");
   free(rendered);
   return content;
}

static cJSON *mcph_memory_maintain(struct mcp_call *c)
{
   cJSON *jargs = c->jargs;
   unsigned int modes = 0;
   cJSON *jm = cJSON_GetObjectItemCaseSensitive(jargs, "modes");
   if (cJSON_IsString(jm) && jm->valuestring[0])
   {
      const char *csv = jm->valuestring;
      while (*csv)
      {
         while (*csv == ' ' || *csv == ',')
            csv++;
         if (!*csv)
            break;
         const char *start = csv;
         while (*csv && *csv != ',' && *csv != ' ')
            csv++;
         size_t len = (size_t)(csv - start);
         if (len == 6 && strncmp(start, "replay", 6) == 0)
            modes |= MEMORY_MAINTENANCE_MODE_REPLAY;
         else if (len == 7 && strncmp(start, "compact", 7) == 0)
            modes |= MEMORY_MAINTENANCE_MODE_COMPACT;
         else if (len == 5 && strncmp(start, "prune", 5) == 0)
            modes |= MEMORY_MAINTENANCE_MODE_PRUNE;
         else if (len == 9 && strncmp(start, "summarize", 9) == 0)
            modes |= MEMORY_MAINTENANCE_MODE_SUMMARIZE;
      }
   }
   int dry_run = 0, force = 0;
   cJSON *jd = cJSON_GetObjectItemCaseSensitive(jargs, "dry_run");
   if (cJSON_IsBool(jd))
      dry_run = cJSON_IsTrue(jd) ? 1 : 0;
   cJSON *jf = cJSON_GetObjectItemCaseSensitive(jargs, "force");
   if (cJSON_IsBool(jf))
      force = cJSON_IsTrue(jf) ? 1 : 0;
   char *envelope = kb_client_memory_maintenance_run_json(modes, force, dry_run);
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *summary = resp ? cJSON_GetObjectItemCaseSensitive(resp, "summary") : NULL;
   char *rendered = NULL;
   if (cJSON_IsObject(summary))
   {
      cJSON *detached = cJSON_DetachItemViaPointer(resp, summary);
      rendered = detached ? cJSON_PrintUnformatted(detached) : NULL;
      cJSON_Delete(detached);
   }
   cJSON_Delete(resp);
   cJSON *content = rendered ? text_content(rendered) : text_content("{}");
   free(rendered);
   return content;
}

static cJSON *mcph_autopilot(struct mcp_call *c)
{
   char *ap_result = handle_autopilot(c->jargs);
   cJSON *content = text_content(ap_result);
   free(ap_result);
   return content;
}

static cJSON *mcph_lsp_diagnostics(struct mcp_call *c)
{
   cJSON *jargs = c->jargs;
   cJSON *j_ws = cJSON_GetObjectItemCaseSensitive(jargs, "workspace");
   cJSON *j_file = cJSON_GetObjectItemCaseSensitive(jargs, "file");
   const char *workspace =
       (cJSON_IsString(j_ws) && j_ws->valuestring[0]) ? j_ws->valuestring : NULL;
   const char *file =
       (cJSON_IsString(j_file) && j_file->valuestring[0]) ? j_file->valuestring : NULL;

   /* Default workspace to first configured workspace */
   char ws_buf[MAX_PATH_LEN] = "";
   if (!workspace)
   {
      config_t cfg;
      if (config_load(&cfg) == 0 && cfg.workspace_count > 0)
         snprintf(ws_buf, sizeof(ws_buf), "%s", cfg.workspaces[0]);
      workspace = ws_buf[0] ? ws_buf : ".";
   }

   lsp_manager_init();
   lsp_diag_t diags[LSP_RENDER_MAX_DIAG * 4];
   int n = lsp_manager_diagnostics(workspace, file, diags, (int)(sizeof(diags) / sizeof(diags[0])));

   char out[8192];
   if (n == 0)
   {
      snprintf(out, sizeof(out), "No diagnostics found for %s", file ? file : workspace);
   }
   else
   {
      int pos = snprintf(out, sizeof(out), "%d diagnostic%s:\n", n, n == 1 ? "" : "s");
      for (int di = 0; di < n && pos < (int)sizeof(out) - 2; di++)
      {
         const lsp_diag_t *d = &diags[di];
         int w = snprintf(out + pos, sizeof(out) - (size_t)pos, " - %s:%d:%d [%s] %s\n",
                          d->file[0] ? d->file : "(unknown)", d->line + 1, d->col + 1,
                          lsp_severity_label(d->severity), d->message);
         if (w > 0)
            pos += w;
      }
   }
   return text_content(out);
}

static cJSON *mcph_lsp_definition_or_references(struct mcp_call *c)
{
   cJSON *jargs = c->jargs;
   const char *tool = c->tool;
   cJSON *j_ws = cJSON_GetObjectItemCaseSensitive(jargs, "workspace");
   cJSON *j_file = cJSON_GetObjectItemCaseSensitive(jargs, "file");
   cJSON *j_line = cJSON_GetObjectItemCaseSensitive(jargs, "line");
   cJSON *j_col = cJSON_GetObjectItemCaseSensitive(jargs, "col");

   const char *workspace =
       (cJSON_IsString(j_ws) && j_ws->valuestring[0]) ? j_ws->valuestring : NULL;
   const char *file =
       (cJSON_IsString(j_file) && j_file->valuestring[0]) ? j_file->valuestring : NULL;
   int ln = cJSON_IsNumber(j_line) ? (int)j_line->valuedouble : 0;
   int col = cJSON_IsNumber(j_col) ? (int)j_col->valuedouble : 0;

   if (!file)
      return text_content("lsp_definition/lsp_references: 'file' parameter is required");

   char ws_buf[MAX_PATH_LEN] = "";
   if (!workspace)
   {
      config_t cfg;
      if (config_load(&cfg) == 0 && cfg.workspace_count > 0)
         snprintf(ws_buf, sizeof(ws_buf), "%s", cfg.workspaces[0]);
      workspace = ws_buf[0] ? ws_buf : ".";
   }

   lsp_manager_init();
   lsp_location_t locs[LSP_RENDER_MAX_SYM * 4];
   char errbuf[256] = "";
   int n;

   if (strcmp(tool, "lsp_definition") == 0)
      n = lsp_manager_definition(workspace, file, ln, col, locs,
                                 (int)(sizeof(locs) / sizeof(locs[0])), errbuf, sizeof(errbuf));
   else
      n = lsp_manager_references(workspace, file, ln, col, locs,
                                 (int)(sizeof(locs) / sizeof(locs[0])), errbuf, sizeof(errbuf));

   char out[4096];
   if (n < 0)
   {
      snprintf(out, sizeof(out), "LSP error: %s", errbuf[0] ? errbuf : "request failed");
   }
   else if (n == 0)
   {
      snprintf(out, sizeof(out), "No %s found for %s:%d:%d",
               strcmp(tool, "lsp_definition") == 0 ? "definition" : "references", file, ln + 1,
               col + 1);
   }
   else
   {
      int pos = snprintf(out, sizeof(out), "%d location%s:\n", n, n == 1 ? "" : "s");
      for (int i = 0; i < n && pos < (int)sizeof(out) - 2; i++)
      {
         int w = snprintf(out + pos, sizeof(out) - (size_t)pos, " - %s:%d:%d\n",
                          locs[i].file[0] ? locs[i].file : "(unknown)", locs[i].line + 1,
                          locs[i].col + 1);
         if (w > 0)
            pos += w;
      }
   }
   return text_content(out);
}

static cJSON *mcph_session_search(struct mcp_call *c)
{
   cJSON *result = session_search_tool_result_for_uid(c->jargs, (unsigned)c->conn->peer_uid);
   *c->structured = cJSON_Duplicate(result, 1);
   return json_result_content(result);
}

static cJSON *mcph_payload_rewrite_status(struct mcp_call *c)
{
   cJSON *result = tool_payload_rewrite_status(c->jargs);
   char *rendered = cJSON_PrintUnformatted(result);
   cJSON_Delete(result);
   cJSON *content = rendered ? text_content(rendered) : text_content("{}");
   free(rendered);
   return content;
}

/* ── P3 extended read-only tools: roadmap / task / code-index / memory-explain.
 * Each wraps an existing kb_client_* call (no new client logic) and emits JSON
 * content. Definitions are in mcp_tools_extended.c. Consistent with the other
 * table tools, these are not capability-gated at the MCP layer. ────────────── */
static cJSON *mcph_roadmap_list(struct mcp_call *c)
{
   (void)c;
   char *json = kb_client_roadmap_list_json();
   cJSON *content = json ? text_content(json) : text_content("error: roadmap_list failed");
   free(json);
   return content;
}

static cJSON *mcph_roadmap_show(struct mcp_call *c)
{
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(c->jargs, "roadmap_id");
   if (!cJSON_IsString(jid) || !jid->valuestring[0])
      return text_content("error: roadmap_show requires 'roadmap_id'");
   char *json = kb_client_roadmap_show_json(jid->valuestring);
   cJSON *content = json ? text_content(json) : text_content("error: roadmap_show failed");
   free(json);
   return content;
}

static cJSON *mcph_task_list(struct mcp_call *c)
{
   cJSON *jstate = cJSON_GetObjectItemCaseSensitive(c->jargs, "state");
   cJSON *jsess = cJSON_GetObjectItemCaseSensitive(c->jargs, "session_id");
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(c->jargs, "limit");
   const char *state = cJSON_IsString(jstate) ? jstate->valuestring : NULL;
   const char *sess = cJSON_IsString(jsess) ? jsess->valuestring : NULL;
   int limit = (cJSON_IsNumber(jlimit) && jlimit->valueint > 0) ? jlimit->valueint : 100;
   if (limit > 500)
      limit = 500;
   aimee_task_t *tasks = calloc((size_t)limit, sizeof(*tasks));
   if (!tasks)
      return text_content("error: out of memory");
   int n = kb_client_task_list(state, sess, limit, tasks, limit);
   if (n < 0)
   {
      free(tasks);
      return text_content("error: task_list failed");
   }
   cJSON *result = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(result, "tasks");
   for (int i = 0; i < n; i++)
   {
      cJSON *t = cJSON_CreateObject();
      cJSON_AddNumberToObject(t, "id", (double)tasks[i].id);
      if (tasks[i].parent_id)
         cJSON_AddNumberToObject(t, "parent_id", (double)tasks[i].parent_id);
      cJSON_AddStringToObject(t, "title", tasks[i].title);
      cJSON_AddStringToObject(t, "state", tasks[i].state);
      cJSON_AddNumberToObject(t, "confidence", tasks[i].confidence);
      cJSON_AddStringToObject(t, "updated_at", tasks[i].updated_at);
      if (tasks[i].session_id[0])
         cJSON_AddStringToObject(t, "session_id", tasks[i].session_id);
      cJSON_AddItemToArray(arr, t);
   }
   cJSON_AddNumberToObject(result, "count", n);
   free(tasks);
   return json_result_content(result);
}

static cJSON *mcph_index_find_callers(struct mcp_call *c)
{
   cJSON *js = cJSON_GetObjectItemCaseSensitive(c->jargs, "symbol");
   if (!cJSON_IsString(js) || !js->valuestring[0])
      return text_content("error: index_find_callers requires 'symbol'");
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   const char *project = cJSON_IsString(jp) ? jp->valuestring : NULL;
   const int max = 200;
   caller_hit_t *hits = calloc((size_t)max, sizeof(*hits));
   if (!hits)
      return text_content("error: out of memory");
   int n = kb_client_index_find_callers(project, js->valuestring, hits, max);
   if (n < 0)
   {
      free(hits);
      return text_content("error: index_find_callers failed");
   }
   cJSON *result = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(result, "callers");
   for (int i = 0; i < n; i++)
   {
      cJSON *h = cJSON_CreateObject();
      cJSON_AddStringToObject(h, "project", hits[i].project);
      cJSON_AddStringToObject(h, "file", hits[i].file_path);
      if (hits[i].caller[0])
         cJSON_AddStringToObject(h, "caller", hits[i].caller);
      cJSON_AddNumberToObject(h, "line", hits[i].line);
      cJSON_AddItemToArray(arr, h);
   }
   cJSON_AddNumberToObject(result, "count", n);
   free(hits);
   return json_result_content(result);
}

static cJSON *mcph_index_structure(struct mcp_call *c)
{
   cJSON *jf = cJSON_GetObjectItemCaseSensitive(c->jargs, "file_path");
   if (!cJSON_IsString(jf) || !jf->valuestring[0])
      return text_content("error: index_structure requires 'file_path'");
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   const char *project = cJSON_IsString(jp) ? jp->valuestring : NULL;
   const int max = 1000;
   definition_t *defs = calloc((size_t)max, sizeof(*defs));
   if (!defs)
      return text_content("error: out of memory");
   int n = kb_client_index_structure(project, jf->valuestring, defs, max);
   if (n < 0)
   {
      free(defs);
      return text_content("error: index_structure failed");
   }
   cJSON *result = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(result, "definitions");
   for (int i = 0; i < n; i++)
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "name", defs[i].name);
      cJSON_AddStringToObject(d, "kind", defs[i].kind);
      cJSON_AddNumberToObject(d, "line", defs[i].line);
      if (defs[i].line_end)
         cJSON_AddNumberToObject(d, "line_end", defs[i].line_end);
      cJSON_AddItemToArray(arr, d);
   }
   cJSON_AddNumberToObject(result, "count", n);
   free(defs);
   return json_result_content(result);
}

static cJSON *mcph_memory_explain_match(struct mcp_call *c)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(c->jargs, "query");
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(c->jargs, "memory_id");
   if (!cJSON_IsString(jq) || !jq->valuestring[0] || !cJSON_IsNumber(jid))
      return text_content("error: memory_explain_match requires 'query' and 'memory_id'");
   memory_diagnostic_t diag;
   memset(&diag, 0, sizeof(diag));
   if (kb_client_memory_explain_match(jq->valuestring, (int64_t)jid->valuedouble, &diag) != 0)
      return text_content("error: memory_explain_match failed");
   cJSON *result = cJSON_CreateObject();
   cJSON *m = cJSON_AddObjectToObject(result, "memory");
   cJSON_AddNumberToObject(m, "id", (double)diag.memory.id);
   cJSON_AddStringToObject(m, "tier", diag.memory.tier);
   cJSON_AddStringToObject(m, "kind", diag.memory.kind);
   cJSON_AddStringToObject(m, "headline", diag.memory.headline);
   cJSON_AddStringToObject(m, "content", diag.memory.content);
   cJSON *p = cJSON_AddObjectToObject(result, "scores");
   cJSON_AddNumberToObject(p, "lexical", diag.parts.lexical);
   cJSON_AddNumberToObject(p, "semantic", diag.parts.semantic);
   cJSON_AddNumberToObject(p, "entity", diag.parts.entity);
   cJSON_AddNumberToObject(p, "temporal", diag.parts.temporal);
   cJSON_AddNumberToObject(p, "evidence", diag.parts.evidence);
   cJSON_AddNumberToObject(p, "confidence", diag.parts.confidence);
   cJSON_AddNumberToObject(p, "salience", diag.parts.salience);
   cJSON_AddNumberToObject(p, "cross_encoder", diag.parts.cross_encoder);
   cJSON_AddNumberToObject(p, "graph_score", diag.parts.graph_score);
   cJSON_AddNumberToObject(p, "hybrid_total", diag.parts.hybrid_total);
   cJSON_AddNumberToObject(p, "blended_total", diag.parts.blended_total);
   cJSON_AddNumberToObject(p, "total", diag.parts.total);
   return json_result_content(result);
}

/* ── P3b extended read-only tools: blast radius, memory provenance/history,
 * dashboard metrics. Same pattern: wrap an existing call, emit JSON. ───────── */
static cJSON *mcph_index_blast_radius(struct mcp_call *c)
{
   cJSON *jf = cJSON_GetObjectItemCaseSensitive(c->jargs, "file_path");
   if (!cJSON_IsString(jf) || !jf->valuestring[0])
      return text_content("error: index_blast_radius requires 'file_path'");
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   const char *project = cJSON_IsString(jp) ? jp->valuestring : NULL;
   blast_radius_t *br = calloc(1, sizeof(*br));
   if (!br)
      return text_content("error: out of memory");
   if (kb_client_index_blast_radius(project, jf->valuestring, br) < 0)
   {
      free(br);
      return text_content("error: index_blast_radius failed");
   }
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "file", br->file);
   cJSON *deps = cJSON_AddArrayToObject(result, "dependents");
   for (int i = 0; i < br->dependent_count && i < 64; i++)
      cJSON_AddItemToArray(deps, cJSON_CreateString(br->dependents[i]));
   cJSON *uses = cJSON_AddArrayToObject(result, "dependencies");
   for (int i = 0; i < br->dependency_count && i < 64; i++)
      cJSON_AddItemToArray(uses, cJSON_CreateString(br->dependencies[i]));
   cJSON_AddNumberToObject(result, "dependent_count", br->dependent_count);
   cJSON_AddNumberToObject(result, "dependency_count", br->dependency_count);
   free(br);
   return json_result_content(result);
}

static cJSON *mcph_memory_provenance(struct mcp_call *c)
{
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(c->jargs, "memory_id");
   if (!cJSON_IsNumber(jid))
      return text_content("error: memory_provenance requires 'memory_id'");
   const int max = 200;
   provenance_entry_t *ents = calloc((size_t)max, sizeof(*ents));
   if (!ents)
      return text_content("error: out of memory");
   int n = kb_client_memory_get_provenance((int64_t)jid->valuedouble, ents, max);
   if (n < 0)
   {
      free(ents);
      return text_content("error: memory_provenance failed");
   }
   cJSON *result = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(result, "provenance");
   for (int i = 0; i < n; i++)
   {
      cJSON *e = cJSON_CreateObject();
      cJSON_AddNumberToObject(e, "id", (double)ents[i].id);
      cJSON_AddStringToObject(e, "action", ents[i].action);
      if (ents[i].session_id[0])
         cJSON_AddStringToObject(e, "session_id", ents[i].session_id);
      if (ents[i].details[0])
         cJSON_AddStringToObject(e, "details", ents[i].details);
      cJSON_AddStringToObject(e, "created_at", ents[i].created_at);
      cJSON_AddItemToArray(arr, e);
   }
   cJSON_AddNumberToObject(result, "count", n);
   free(ents);
   return json_result_content(result);
}

static cJSON *mcph_memory_fact_history(struct mcp_call *c)
{
   cJSON *jk = cJSON_GetObjectItemCaseSensitive(c->jargs, "key");
   if (!cJSON_IsString(jk) || !jk->valuestring[0])
      return text_content("error: memory_fact_history requires 'key'");
   const int max = 100;
   memory_t *mems = calloc((size_t)max, sizeof(*mems));
   if (!mems)
      return text_content("error: out of memory");
   int n = kb_client_memory_fact_history(jk->valuestring, mems, max);
   if (n < 0)
   {
      free(mems);
      return text_content("error: memory_fact_history failed");
   }
   cJSON *result = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(result, "history");
   for (int i = 0; i < n; i++)
   {
      cJSON *m = cJSON_CreateObject();
      cJSON_AddNumberToObject(m, "id", (double)mems[i].id);
      cJSON_AddStringToObject(m, "tier", mems[i].tier);
      cJSON_AddStringToObject(m, "kind", mems[i].kind);
      cJSON_AddStringToObject(m, "content", mems[i].content);
      cJSON_AddNumberToObject(m, "confidence", mems[i].confidence);
      cJSON_AddStringToObject(m, "updated_at", mems[i].updated_at);
      cJSON_AddItemToArray(arr, m);
   }
   cJSON_AddNumberToObject(result, "count", n);
   free(mems);
   return json_result_content(result);
}

static cJSON *mcph_dashboard_metrics(struct mcp_call *c)
{
   (void)c;
   cJSON *result = cJSON_CreateObject();
   char *metrics = api_metrics();
   cJSON *mj = metrics ? cJSON_Parse(metrics) : NULL;
   free(metrics);
   if (mj)
      cJSON_AddItemToObject(result, "metrics", mj);
   char *vec = api_vector_status();
   cJSON *vj = vec ? cJSON_Parse(vec) : NULL;
   free(vec);
   if (vj)
      cJSON_AddItemToObject(result, "vector", vj);
   return json_result_content(result);
}

/* Append a work-queue row's common fields to a cJSON object. */
static void mcp_work_row_fields(cJSON *o, const db1_work_queue_list_row_t *row)
{
   cJSON_AddStringToObject(o, "id", row->id);
   cJSON_AddStringToObject(o, "title", row->title);
   if (row->source[0])
      cJSON_AddStringToObject(o, "source", row->source);
   cJSON_AddStringToObject(o, "status", row->status);
   if (row->created_at[0])
      cJSON_AddStringToObject(o, "created_at", row->created_at);
   if (row->claimed_by[0])
      cJSON_AddStringToObject(o, "claimed_by", row->claimed_by);
   if (row->result[0])
      cJSON_AddStringToObject(o, "result", row->result);
}

static cJSON *mcph_work_list(struct mcp_call *c)
{
   cJSON *jf = cJSON_GetObjectItemCaseSensitive(c->jargs, "status_filter");
   const char *filter = cJSON_IsString(jf) ? jf->valuestring : NULL;
   db1_work_queue_list_row_t *rows = NULL;
   size_t n = 0;
   if (db1_work_queue_alloc_list(filter, &rows, &n) != 0)
      return text_content("error: work_list failed");
   cJSON *result = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(result, "items");
   for (size_t i = 0; i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      mcp_work_row_fields(o, &rows[i]);
      cJSON_AddItemToArray(arr, o);
   }
   cJSON_AddNumberToObject(result, "count", (double)n);
   free(rows);
   return json_result_content(result);
}

static cJSON *mcph_work_board(struct mcp_call *c)
{
   (void)c;
   static const char *const statuses[] = {"pending", "claimed", "done", "failed", "cancelled"};
   db1_work_queue_list_row_t *rows = NULL;
   size_t n = 0;
   if (db1_work_queue_alloc_list("all", &rows, &n) != 0)
      return text_content("error: work_board failed");
   cJSON *result = cJSON_CreateObject();
   cJSON *board = cJSON_AddObjectToObject(result, "board");
   for (int s = 0; s < 5; s++)
   {
      cJSON *arr = cJSON_AddArrayToObject(board, statuses[s]);
      for (size_t i = 0; i < n; i++)
      {
         if (strcmp(rows[i].status, statuses[s]) != 0)
            continue;
         cJSON *o = cJSON_CreateObject();
         mcp_work_row_fields(o, &rows[i]);
         cJSON_AddItemToArray(arr, o);
      }
   }
   cJSON_AddNumberToObject(result, "total", (double)n);
   free(rows);
   return json_result_content(result);
}

/* ── Structured-PDF evidence ──────────────────────────────────────────────────
 * The /v1/pdf/... routes return purpose-built citation JSON (nested page/bbox/
 * quote geometry) that we forward verbatim — see kb_client_pdf.c for why we do
 * NOT round-trip it through structs. `json` is the route body (NULL on
 * non-2xx); `status` is the HTTP status used to craft an actionable error. */
static cJSON *pdf_passthrough(char *json, int status, const char *tool)
{
   if (json)
   {
      cJSON *content = text_content(json); /* copies json into the text node */
      free(json);
      return content;
   }
   char msg[192];
   if (status == 413)
      snprintf(msg, sizeof(msg),
               "error: %s result too large (413) — narrow the query or reduce max_results", tool);
   else if (status == 403)
      snprintf(msg, sizeof(msg),
               "error: %s access denied (403) — your access does not include that project", tool);
   else if (status == 404)
      snprintf(msg, sizeof(msg), "error: %s found no matching PDF evidence (404)", tool);
   else if (status == 400)
      snprintf(msg, sizeof(msg), "error: %s bad request (400) — check the arguments", tool);
   else
      snprintf(msg, sizeof(msg), "error: %s request failed (status %d)", tool, status);
   return text_content(msg);
}

/* Read a positive-integer JSON arg `key` into *out, requiring 1 <= value <= max.
 * Returns 0 when the arg is absent, non-numeric, NaN/inf, fractional-out-of-band,
 * or out of range. The bounded check also avoids the undefined behaviour of
 * casting an out-of-range double to an integer type (cJSON stores every number
 * as a double, so a caller can send 1e30). */
static int pdf_arg_pos_int(cJSON *args, const char *key, double max, long long *out)
{
   cJSON *j = cJSON_GetObjectItemCaseSensitive(args, key);
   if (!cJSON_IsNumber(j))
      return 0;
   double v = j->valuedouble;
   if (!(v >= 1.0 && v <= max)) /* false for NaN/inf/negative/over-range */
      return 0;
   *out = (long long)v;
   return 1;
}

/* ── Code-graph retrieval + analytics ─────────────────────────────────────────
 * /v1/code/hybrid and /v1/code/graph/hubs return purpose-built fused/ranked JSON
 * the agent consumes directly, so we forward the route body verbatim (see
 * kb_client_code_graph.c). Shares pdf_arg_pos_int for the bounded max_results. */
static cJSON *code_graph_passthrough(char *json, int status, const char *tool)
{
   if (json)
   {
      cJSON *content = text_content(json); /* copies json into the text node */
      free(json);
      return content;
   }
   char msg[160];
   if (status == 413)
      snprintf(msg, sizeof(msg), "error: %s result too large (413) — reduce max_results", tool);
   else if (status == 403)
      snprintf(msg, sizeof(msg), "error: %s access denied (403) for that project", tool);
   else if (status == 400)
      snprintf(msg, sizeof(msg), "error: %s bad request (400) — check the arguments", tool);
   else
      snprintf(msg, sizeof(msg), "error: %s request failed (status %d)", tool, status);
   return text_content(msg);
}

static cJSON *mcph_index_hybrid(struct mcp_call *c)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(c->jargs, "query");
   if (!cJSON_IsString(jq) || !jq->valuestring[0])
      return text_content("error: index_hybrid requires 'query'");
   cJSON *js = cJSON_GetObjectItemCaseSensitive(c->jargs, "symbol");
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   const char *symbol = cJSON_IsString(js) ? js->valuestring : NULL;
   const char *project = cJSON_IsString(jp) ? jp->valuestring : NULL;
   long long mr = 0;
   int max_results = pdf_arg_pos_int(c->jargs, "max_results", 100.0, &mr) ? (int)mr : 20;
   int status = -1;
   char *json = kb_client_code_hybrid(jq->valuestring, symbol, project, max_results, &status);
   /* §3 live cite-capture (default off): observe the retrieved file paths for this
    * session so a re-cited source earns trust across turns. Best-effort; gated by the
    * same flag as the retrieval-side trust tie-break. */
   if (json && project && c->sid && c->sid[0])
   {
      config_t ccfg;
      if (config_load(&ccfg) == 0 && ccfg.code_trust_actuation_enabled)
      {
         cJSON *root = cJSON_Parse(json);
         cJSON *results = root ? cJSON_GetObjectItemCaseSensitive(root, "results") : NULL;
         if (cJSON_IsArray(results))
         {
            int n = cJSON_GetArraySize(results);
            const char **paths = calloc((size_t)(n > 0 ? n : 1), sizeof(*paths));
            int cnt = 0;
            for (int i = 0; paths && i < n; i++)
            {
               cJSON *row = cJSON_GetArrayItem(results, i);
               cJSON *fp = row ? cJSON_GetObjectItemCaseSensitive(row, "file_path") : NULL;
               if (cJSON_IsString(fp) && fp->valuestring[0])
                  paths[cnt++] = fp->valuestring;
            }
            if (cnt > 0)
               kb_client_code_lessons_observe(project, c->sid, paths, cnt);
            free(paths);
         }
         cJSON_Delete(root);
      }
   }
   return code_graph_passthrough(json, status, "index_hybrid");
}

static cJSON *mcph_index_graph_hubs(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   if (!cJSON_IsString(jp) || !jp->valuestring[0])
      return text_content("error: index_graph_hubs requires 'project'");
   long long mr = 0;
   int max_results = pdf_arg_pos_int(c->jargs, "max_results", 200.0, &mr) ? (int)mr : 20;
   int status = -1;
   char *json = kb_client_code_graph_hubs(jp->valuestring, max_results, &status);
   return code_graph_passthrough(json, status, "index_graph_hubs");
}

static cJSON *mcph_index_graph_audit(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   if (!cJSON_IsString(jp) || !jp->valuestring[0])
      return text_content("error: index_graph_audit requires 'project'");
   long long mf = 0;
   int max_findings = pdf_arg_pos_int(c->jargs, "max_findings", 200.0, &mf) ? (int)mf : 20;
   int status = -1;
   char *json = kb_client_code_graph_audit(jp->valuestring, max_findings, &status);
   return code_graph_passthrough(json, status, "index_graph_audit");
}

static cJSON *mcph_index_lessons(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   if (!cJSON_IsString(jp) || !jp->valuestring[0])
      return text_content("error: index_lessons requires 'project'");
   int status = -1;
   char *json = kb_client_code_lessons(jp->valuestring, &status);
   return code_graph_passthrough(json, status, "index_lessons");
}

static cJSON *mcph_index_graph_diff(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   cJSON *jf = cJSON_GetObjectItemCaseSensitive(c->jargs, "from_gen");
   cJSON *jt = cJSON_GetObjectItemCaseSensitive(c->jargs, "to_gen");
   if (!cJSON_IsString(jp) || !jp->valuestring[0])
      return text_content("error: index_graph_diff requires 'project'");
   if (!cJSON_IsString(jf) || !jf->valuestring[0])
      return text_content("error: index_graph_diff requires 'from_gen' (id or 'default_latest')");
   if (!cJSON_IsString(jt) || !jt->valuestring[0])
      return text_content("error: index_graph_diff requires 'to_gen' (id or 'default_latest')");
   cJSON *jforce = cJSON_GetObjectItemCaseSensitive(c->jargs, "force");
   int force = cJSON_IsTrue(jforce) ? 1 : 0;
   int status = -1;
   char *json =
       kb_client_code_graph_diff(jp->valuestring, jf->valuestring, jt->valuestring, force, &status);
   return code_graph_passthrough(json, status, "index_graph_diff");
}

static cJSON *mcph_index_graph_node(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   cJSON *jn = cJSON_GetObjectItemCaseSensitive(c->jargs, "node");
   if (!cJSON_IsString(jp) || !jp->valuestring[0])
      return text_content("error: index_graph_node requires 'project'");
   if (!cJSON_IsString(jn) || !jn->valuestring[0])
      return text_content("error: index_graph_node requires 'node'");
   long long mr = 0;
   int max_results = pdf_arg_pos_int(c->jargs, "max_results", 200.0, &mr) ? (int)mr : 50;
   int status = -1;
   char *json = kb_client_code_graph_node(jp->valuestring, jn->valuestring, max_results, &status);
   return code_graph_passthrough(json, status, "index_graph_node");
}

static cJSON *mcph_index_graph_surprising(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   if (!cJSON_IsString(jp) || !jp->valuestring[0])
      return text_content("error: index_graph_surprising requires 'project'");
   long long mr = 0;
   int max_results = pdf_arg_pos_int(c->jargs, "max_results", 200.0, &mr) ? (int)mr : 20;
   cJSON *jj = cJSON_GetObjectItemCaseSensitive(c->jargs, "judge");
   int judge = cJSON_IsTrue(jj) ? 1 : 0;
   int status = -1;
   char *json = kb_client_code_graph_surprising(jp->valuestring, max_results, judge, &status);
   return code_graph_passthrough(json, status, "index_graph_surprising");
}

static cJSON *mcph_pdf_search_chunks(struct mcp_call *c)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(c->jargs, "query");
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   /* project is REQUIRED: omitting it would leave the request un-scoped, so the
    * kb_http token-scope gate (which only fires when the request names a scope)
    * would not run — letting a project-scoped caller read every project's PDF
    * content. Requiring it upholds the "project-scope all PDF reads" invariant,
    * mirroring pdf_open_neighbors. */
   if (!cJSON_IsString(jq) || !jq->valuestring[0] || !cJSON_IsString(jp) || !jp->valuestring[0])
      return text_content(
          "error: pdf_search_chunks requires 'query' and 'project' (project scopes the search)");
   /* max_results is optional; default 10 (the route's cap). An absent, junk, or
    * out-of-[1,10] value falls back to 10 rather than erroring. */
   long long mr = 0;
   int max_results = pdf_arg_pos_int(c->jargs, "max_results", 10.0, &mr) ? (int)mr : 10;
   int status = -1;
   char *json = kb_client_pdf_search_chunks(jq->valuestring, jp->valuestring, max_results, &status);
   return pdf_passthrough(json, status, "pdf_search_chunks");
}

static cJSON *mcph_pdf_open_page(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   cJSON *jd = cJSON_GetObjectItemCaseSensitive(c->jargs, "document_key");
   long long page_no = 0;
   if (!cJSON_IsString(jp) || !jp->valuestring[0] || !cJSON_IsString(jd) || !jd->valuestring[0] ||
       !pdf_arg_pos_int(c->jargs, "page_no", 2147483647.0, &page_no))
      return text_content(
          "error: pdf_open_page requires 'project', 'document_key', and a 'page_no' >= 1");
   int status = -1;
   char *json = kb_client_pdf_open_page(jp->valuestring, jd->valuestring, (int)page_no, &status);
   return pdf_passthrough(json, status, "pdf_open_page");
}

static cJSON *mcph_pdf_open_neighbors(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   long long chunk_id = 0;
   /* chunk_ids are DB row ids; bound to 2^53, the largest integer a double
    * (cJSON's number type) represents exactly. */
   if (!cJSON_IsString(jp) || !jp->valuestring[0] ||
       !pdf_arg_pos_int(c->jargs, "chunk_id", 9007199254740992.0, &chunk_id))
      return text_content("error: pdf_open_neighbors requires 'project' and a 'chunk_id' >= 1");
   int status = -1;
   char *json = kb_client_pdf_open_neighbors(jp->valuestring, chunk_id, &status);
   return pdf_passthrough(json, status, "pdf_open_neighbors");
}

static cJSON *mcph_pdf_inspect_structure(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   cJSON *jd = cJSON_GetObjectItemCaseSensitive(c->jargs, "document_key");
   if (!cJSON_IsString(jp) || !jp->valuestring[0] || !cJSON_IsString(jd) || !jd->valuestring[0])
      return text_content("error: pdf_inspect_structure requires 'project' and 'document_key'");
   int status = -1;
   char *json = kb_client_pdf_inspect_structure(jp->valuestring, jd->valuestring, &status);
   return pdf_passthrough(json, status, "pdf_inspect_structure");
}

static cJSON *mcph_pdf_lookup_table(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   cJSON *jd = cJSON_GetObjectItemCaseSensitive(c->jargs, "document_key");
   if (!cJSON_IsString(jp) || !jp->valuestring[0] || !cJSON_IsString(jd) || !jd->valuestring[0])
      return text_content("error: pdf_lookup_table requires 'project' and 'document_key'");
   /* page_no is optional; an absent or < 1 value means all pages (-1). */
   long long page_no = 0;
   int page = pdf_arg_pos_int(c->jargs, "page_no", 1000000.0, &page_no) ? (int)page_no : -1;
   int status = -1;
   char *json = kb_client_pdf_lookup_table(jp->valuestring, jd->valuestring, page, &status);
   return pdf_passthrough(json, status, "pdf_lookup_table");
}

static cJSON *mcph_pdf_list_assets(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   cJSON *jd = cJSON_GetObjectItemCaseSensitive(c->jargs, "document_key");
   if (!cJSON_IsString(jp) || !jp->valuestring[0] || !cJSON_IsString(jd) || !jd->valuestring[0])
      return text_content("error: pdf_list_assets requires 'project' and 'document_key'");
   int status = -1;
   char *json = kb_client_pdf_list_assets(jp->valuestring, jd->valuestring, &status);
   return pdf_passthrough(json, status, "pdf_list_assets");
}

static cJSON *mcph_pdf_open_asset(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   long long asset_id = 0;
   if (!cJSON_IsString(jp) || !jp->valuestring[0] ||
       !pdf_arg_pos_int(c->jargs, "asset_id", 9007199254740992.0, &asset_id))
      return text_content("error: pdf_open_asset requires 'project' and an 'asset_id' >= 1");
   int status = -1;
   char *json = kb_client_pdf_open_asset(jp->valuestring, asset_id, &status);
   return pdf_passthrough(json, status, "pdf_open_asset");
}

/* code_span_get (ingress-compression P2): the recovery resolver for a folded code
 * reference. Resolves the project's indexed root, then reads a validated, clamped
 * line range through the active workspace provider (code_span_read does the B4
 * path-safety + slicing). Bounds the line args via pdf_arg_pos_int so an
 * out-of-range double can't UB-cast. */
static cJSON *mcph_code_span_get(struct mcp_call *c)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(c->jargs, "project");
   cJSON *jf = cJSON_GetObjectItemCaseSensitive(c->jargs, "file_path");
   if (!cJSON_IsString(jp) || !jp->valuestring[0])
      return text_content("error: code_span_get requires 'project'");
   if (!cJSON_IsString(jf) || !jf->valuestring[0])
      return text_content("error: code_span_get requires 'file_path'");

   long long ls = 0, le = 0;
   int line_start = pdf_arg_pos_int(c->jargs, "line_start", 2000000000.0, &ls) ? (int)ls : 1;
   int line_end = pdf_arg_pos_int(c->jargs, "line_end", 2000000000.0, &le) ? (int)le : line_start;

   /* Resolve the project's recorded root (the containment anchor). project_info_t
    * is ~4KB (root[MAX_PATH_LEN]), so heap-allocate the list rather than blow the
    * server-thread stack. */
   const int max_projs = 256;
   project_info_t *projs = calloc((size_t)max_projs, sizeof(*projs));
   if (!projs)
      return text_content("error: out of memory");
   int np = kb_client_index_list(projs, max_projs);
   if (np < 0)
   {
      free(projs);
      return text_content("error: code index unavailable");
   }
   char root[MAX_PATH_LEN] = "";
   for (int i = 0; i < np; i++)
      if (strcmp(projs[i].name, jp->valuestring) == 0 && projs[i].root[0])
      {
         snprintf(root, sizeof(root), "%s", projs[i].root);
         break;
      }
   free(projs);
   if (!root[0])
      return text_content("error: unknown project (no indexed root)");

   config_t cfg;
   config_load(&cfg);
   int max_lines = cfg.code_span_max_lines > 0 ? cfg.code_span_max_lines : 400;

   cJSON *result =
       code_span_read(jp->valuestring, root, jf->valuestring, line_start, line_end, max_lines);
   if (!result)
      return text_content("error: out of memory");
   return json_result_content(result);
}

/* Primary-as-manager S2: the server-authoritative interactive-driver advance. The
 * primary calls this to move its bound work-item forward one block; the outcome
 * (ok/replay/stale/unbound/terminal/disabled) is decided from authoritative DB
 * state and audited, never inferred from prose. Dormant until sub-slice 4 injects
 * the tool per bound block; inert while the enforcement dial is off. */
static cJSON *mcph_advance_request(struct mcp_call *c)
{
   char *args = c->jargs ? cJSON_PrintUnformatted(c->jargs) : NULL;
   char outbuf[1024];
   wfe_advance_request_run(c->sid, args, outbuf, sizeof outbuf);
   free(args);
   return text_content(outbuf);
}

/* workflow_run: start a saved workflow run from a written proposal. Shares the
 * capped/audited intake (dev_submit_run) with POST /v1/dev/submit; the run is
 * bound to this connection's principal. Async — returns the work_item_id; the
 * run advances server-side. */
static cJSON *mcph_workflow_run(struct mcp_call *c)
{
   cJSON *jwf = cJSON_GetObjectItemCaseSensitive(c->jargs, "workflow");
   cJSON *jprop = cJSON_GetObjectItemCaseSensitive(c->jargs, "proposal_md");
   cJSON *jrepo = cJSON_GetObjectItemCaseSensitive(c->jargs, "repo");
   const char *workflow = (cJSON_IsString(jwf) && jwf->valuestring[0]) ? jwf->valuestring : NULL;
   const char *proposal = cJSON_IsString(jprop) ? jprop->valuestring : "";
   const char *repo = cJSON_IsString(jrepo) ? jrepo->valuestring : "";
   const char *submitter = (c->conn && c->conn->vault_principal[0]) ? c->conn->vault_principal : "";

   cJSON *out = NULL;
   char err[256] = "";
   int st = dev_submit_run(proposal, workflow, repo, submitter, &out, err, sizeof err);
   if (st != 200 || !out)
   {
      cJSON_Delete(out);
      char msg[320];
      snprintf(msg, sizeof msg, "workflow_run failed (%d): %s", st, err[0] ? err : "unknown error");
      return text_content(msg);
   }
   /* Read the summary fields before handing `out` to the structured channel. */
   cJSON *ji = cJSON_GetObjectItemCaseSensitive(out, "work_item_id");
   cJSON *jw = cJSON_GetObjectItemCaseSensitive(out, "workflow");
   cJSON *js = cJSON_GetObjectItemCaseSensitive(out, "state");
   const char *id = cJSON_IsString(ji) ? ji->valuestring : "";
   const char *wf = cJSON_IsString(jw) ? jw->valuestring : "";
   const char *state = cJSON_IsString(js) ? js->valuestring : "";
   char msg[320];
   snprintf(msg, sizeof msg, "Started workflow '%s' — work_item_id %s (state: %s).", wf, id, state);
   cJSON *content = text_content(msg);
   if (c->structured)
      *c->structured = out; /* transfer ownership to the structured channel */
   else
      cJSON_Delete(out);
   return content;
}

/* ── name → handler table (exact match; order is irrelevant — names unique) ──
 *
 * THIS TABLE IS THE SINGLE SOURCE OF TRUTH for which tools aimee has.
 *
 * `native` names the toolset(s) — comma-separated — whose roles may call the tool
 * as one of aimee's OWN agents (a delegate, a review panelist). NULL means the tool
 * is for external MCP clients only. Set it and the tool becomes native everywhere:
 * advertised, schema'd from this same handler's MCP schema, dispatchable, and
 * present in the toolset — no second registry to keep in step, because the second
 * registry is what kept going wrong. git_commit/git_push/git_pr were MCP-only, so
 * aimee's implement delegate had no way to land work but shelling out to git — the
 * one thing require_aimee_git forbids. index_find_callers was MCP-only, so a review
 * panel asked "is this still called?" hedged on a symbol with twelve callers one
 * query away. Both shipped green.
 *
 * Marking a tool native asserts it works with NO client session behind it: a native
 * call passes ctx=NULL and conn=NULL. Today no handler touches ctx and only
 * session_search / workflow_run touch conn — keep those two external-only (they are
 * about an external client's own session anyway, and are EXEMPT in
 * check-native-tool-parity.py for that reason).
 *
 * scripts/check-native-tool-parity.py fails the build on an unmarked, unexempted
 * new tool, so this stays true rather than becoming a comment that used to be. */

static const struct
{
   const char *name;
   mcp_tool_handler_fn fn;
   const char *native;
} mcp_tool_table[] = {
    {"get_help", mcph_get_help, NULL},
    {"ast_grep_search", mcph_ast_grep_search, "core,review_indexed"},
    {"search_memory", mcph_search_memory, NULL},
    {"mutate", mcph_mutate, NULL},
    {"memory_ask", mcph_memory_ask, NULL},
    {"search_graph", mcph_search_graph, "core,review_indexed"},
    {"get_episode", mcph_get_episode, NULL},
    {"get_entity", mcph_get_entity, NULL},
    {"get_entity_edges", mcph_get_entity_edges, NULL},
    {"get_context_block", mcph_get_context_block, "core,review_indexed"},
    {"memory_get", mcph_memory_get, NULL},
    {"list_facts", mcph_list_facts, NULL},
    {"memory_briefing", mcph_memory_briefing, NULL},
    {"get_identity", mcph_get_identity, NULL},
    {"list_curiosity_items", mcph_list_curiosity_items, NULL},
    {"create_prospective_memory", mcph_create_prospective_memory, NULL},
    {"list_prospective_memories", mcph_list_prospective_memories, NULL},
    {"complete_prospective_memory", mcph_complete_prospective_memory, NULL},
    {"memory_alerts", mcph_memory_alerts, NULL},
    {"memory_recall", mcph_memory_recall, NULL},
    {"list_epistemic_directives", mcph_list_epistemic_directives, NULL},
    {"create_epistemic_directive", mcph_create_epistemic_directive, NULL},
    {"resolve_epistemic_directive", mcph_resolve_epistemic_directive, NULL},
    {"memory_maintain", mcph_memory_maintain, NULL},
    {"get_host", mcph_get_host, NULL},
    {"list_hosts", mcph_list_hosts, NULL},
    {"find_symbol", mcph_find_symbol, NULL},
    {"preview_blast_radius", mcph_preview_blast_radius, NULL},
    {"record_attempt", mcph_record_attempt, NULL},
    {"list_attempts", mcph_list_attempts, NULL},
    {"rules_propose", mcph_rules_propose, NULL},
    {"rules_list", mcph_rules_list, NULL},
    {"store_workflow", mcph_store_workflow, NULL},
    {"learning_propose", mcph_learning_propose, NULL},
    {"learning_review", mcph_learning_review, NULL},
    {"skill_manage", mcph_skill_manage, NULL},
    {"create_note", mcph_create_note, NULL},
    {"list_notes", mcph_list_notes, NULL},
    {"search_notes", mcph_search_notes, NULL},
    {"job_start", mcph_job_start, NULL},
    {"job_status", mcph_job_status, NULL},
    {"autopilot", mcph_autopilot, NULL},
    {"lsp_diagnostics", mcph_lsp_diagnostics, NULL},
    {"lsp_definition", mcph_lsp_definition_or_references, NULL},
    {"lsp_references", mcph_lsp_definition_or_references, NULL},
    {"session_context_search", mcph_session_context_search, NULL},
    {"session_context_expand", mcph_session_context_expand, NULL},
    {"session_context_status", mcph_session_context_status, NULL},
    {"session_search", mcph_session_search, NULL},
    {"payload_rewrite_status", mcph_payload_rewrite_status, NULL},
    {"compact_context", mcph_compact_context, NULL},
    {"set_primary_agent", mcph_set_primary_agent, NULL},
    {"upsert_persona", mcph_upsert_persona, NULL},
    {"upsert_role_template", mcph_upsert_role_template, NULL},
    /* P3 extended read-only tools */
    {"roadmap_list", mcph_roadmap_list, NULL},
    {"roadmap_show", mcph_roadmap_show, NULL},
    {"task_list", mcph_task_list, NULL},
    {"index_find_callers", mcph_index_find_callers, "core,review_indexed"},
    {"index_structure", mcph_index_structure, "core,review_indexed"},
    {"code_span_get", mcph_code_span_get, NULL},
    {"index_hybrid", mcph_index_hybrid, NULL},
    {"index_graph_hubs", mcph_index_graph_hubs, NULL},
    {"index_graph_audit", mcph_index_graph_audit, NULL},
    {"index_graph_diff", mcph_index_graph_diff, NULL},
    {"index_lessons", mcph_index_lessons, NULL},
    {"index_graph_surprising", mcph_index_graph_surprising, NULL},
    {"index_graph_node", mcph_index_graph_node, NULL},
    {"memory_explain_match", mcph_memory_explain_match, NULL},
    /* P3b */
    {"index_blast_radius", mcph_index_blast_radius, "core,review_indexed"},
    {"memory_provenance", mcph_memory_provenance, NULL},
    {"memory_fact_history", mcph_memory_fact_history, NULL},
    {"dashboard_metrics", mcph_dashboard_metrics, NULL},
    /* P3c */
    {"work_list", mcph_work_list, NULL},
    {"work_board", mcph_work_board, NULL},
    /* Structured-PDF evidence (access-gated citation retrieval) */
    {"pdf_search_chunks", mcph_pdf_search_chunks, NULL},
    {"pdf_open_page", mcph_pdf_open_page, NULL},
    {"pdf_open_neighbors", mcph_pdf_open_neighbors, NULL},
    {"pdf_inspect_structure", mcph_pdf_inspect_structure, NULL},
    {"pdf_lookup_table", mcph_pdf_lookup_table, NULL},
    {"pdf_list_assets", mcph_pdf_list_assets, NULL},
    {"pdf_open_asset", mcph_pdf_open_asset, NULL},
    /* Primary-as-manager S2 interactive driver */
    {"advance_request", mcph_advance_request, NULL},
    /* Start a saved workflow-engine run from a written proposal */
    {"workflow_run", mcph_workflow_run, NULL},
};

mcp_tool_handler_fn mcp_tool_lookup(const char *tool)
{
   for (size_t i = 0; i < sizeof(mcp_tool_table) / sizeof(mcp_tool_table[0]); i++)
      if (strcmp(mcp_tool_table[i].name, tool) == 0)
         return mcp_tool_table[i].fn;
   return NULL;
}

/* ── the native surface, derived from the table above ────────────────────── */

/* Run an MCP tool for one of aimee's own agents: the same handler an external MCP
 * client reaches, so there is no second implementation that can drift. ctx/conn are
 * NULL — see the table's header for why only ctx/conn-free tools may be native. */
static cJSON *mcp_native_call(const char *tool, cJSON *args, const char *sid)
{
   mcp_tool_handler_fn fn = mcp_tool_lookup(tool);
   if (!fn)
      return NULL;
   struct mcp_call c = {
       .ctx = NULL, .conn = NULL, .jargs = args, .sid = sid, .tool = tool, .structured = NULL};
   return fn(&c);
}

/* The tool's tools/list entry ({"description","inputSchema"}), so the native surface
 * advertises the tool's OWN schema instead of a hand-copied second one. Copying is
 * how git_commit came to advertise parameters its handler never accepted.
 * Caller owns the result. */
static cJSON *mcp_native_advert(const char *tool)
{
   static cJSON *list = NULL;
   static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   pthread_mutex_lock(&lock);
   if (!list)
      list = mcp_build_tools_list(); /* process-lifetime cache; the list is static */
   cJSON *found = NULL;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, list)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
      if (cJSON_IsString(name) && strcmp(name->valuestring, tool) == 0)
      {
         found = cJSON_Duplicate(item, 1);
         break;
      }
   }
   pthread_mutex_unlock(&lock);
   return found;
}

void mcp_tool_register_native_surface(void)
{
   agent_tools_set_mcp_provider(mcp_native_call, mcp_native_advert);
   for (size_t i = 0; i < sizeof(mcp_tool_table) / sizeof(mcp_tool_table[0]); i++)
   {
      if (!mcp_tool_table[i].native)
         continue;
      agent_tools_register_mcp_tool(mcp_tool_table[i].name);
      /* `native` is a comma-separated set list: one tool can belong to several
       * toolsets that do not include one another (coding roles and review
       * panelists both need the code-intelligence tools). */
      char sets[128];
      snprintf(sets, sizeof(sets), "%s", mcp_tool_table[i].native);
      for (char *save = NULL, *tok = strtok_r(sets, ",", &save); tok;
           tok = strtok_r(NULL, ",", &save))
         toolset_register_native_tool(mcp_tool_table[i].name, tok);
   }
}
