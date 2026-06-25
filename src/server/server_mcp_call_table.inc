/* server_mcp_call_table.inc: data-driven dispatch for handle_mcp_call's
 * content-producing MCP tools.
 *
 * Included into server_mcp.c after its file-static tool_* helpers are defined,
 * so the handlers below share that translation unit's statics (text_content,
 * json_result_content, tool_*, kb_client_*, …) without exporting them. This
 * replaces a 45-branch `strcmp(tool, …)` ladder with one {name, fn} table;
 * adding a content tool is now a single table row.
 *
 * Behaviour is identical to the prior ladder: each handler produces the same
 * `content` (and, where applicable, `structured`) the matching else-if branch
 * produced. The terminal tools that send their own response (delegate*,
 * the workflow tools) and the prefix-routed git_ tools stay in handle_mcp_call;
 * the workflow / git_ name spaces are disjoint from this table, so a
 * table-first lookup preserves the ladder's first-match precedence.
 *
 * This is a textual relief valve (line-check exempt), not a module boundary. */

/* Per-call bundle passed to every handler: the request context plus the
 * out-param for tools that emit an MCP `structured` payload alongside text. */
struct mcp_call
{
   server_ctx_t *ctx;
   server_conn_t *conn;
   cJSON *jargs;
   const char *sid;
   const char *tool;
   cJSON **structured;
};

typedef cJSON *(*mcp_tool_handler_fn)(struct mcp_call *c);

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
   return tool_find_symbol(c->jargs);
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
   return tool_create_note(c->jargs);
}
static cJSON *mcph_list_notes(struct mcp_call *c)
{
   return tool_list_notes(c->jargs);
}
static cJSON *mcph_search_notes(struct mcp_call *c)
{
   return tool_search_notes(c->jargs);
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

/* ── name → handler table (exact match; order is irrelevant — names unique) ── */

static const struct
{
   const char *name;
   mcp_tool_handler_fn fn;
} mcp_tool_table[] = {
    {"get_help", mcph_get_help},
    {"ast_grep_search", mcph_ast_grep_search},
    {"search_memory", mcph_search_memory},
    {"mutate", mcph_mutate},
    {"memory_ask", mcph_memory_ask},
    {"search_graph", mcph_search_graph},
    {"get_episode", mcph_get_episode},
    {"get_entity", mcph_get_entity},
    {"get_entity_edges", mcph_get_entity_edges},
    {"get_context_block", mcph_get_context_block},
    {"memory_get", mcph_memory_get},
    {"list_facts", mcph_list_facts},
    {"memory_briefing", mcph_memory_briefing},
    {"get_identity", mcph_get_identity},
    {"list_curiosity_items", mcph_list_curiosity_items},
    {"create_prospective_memory", mcph_create_prospective_memory},
    {"list_prospective_memories", mcph_list_prospective_memories},
    {"complete_prospective_memory", mcph_complete_prospective_memory},
    {"memory_alerts", mcph_memory_alerts},
    {"memory_recall", mcph_memory_recall},
    {"list_epistemic_directives", mcph_list_epistemic_directives},
    {"create_epistemic_directive", mcph_create_epistemic_directive},
    {"resolve_epistemic_directive", mcph_resolve_epistemic_directive},
    {"memory_maintain", mcph_memory_maintain},
    {"get_host", mcph_get_host},
    {"list_hosts", mcph_list_hosts},
    {"find_symbol", mcph_find_symbol},
    {"preview_blast_radius", mcph_preview_blast_radius},
    {"record_attempt", mcph_record_attempt},
    {"list_attempts", mcph_list_attempts},
    {"rules_propose", mcph_rules_propose},
    {"rules_list", mcph_rules_list},
    {"store_workflow", mcph_store_workflow},
    {"learning_propose", mcph_learning_propose},
    {"learning_review", mcph_learning_review},
    {"skill_manage", mcph_skill_manage},
    {"create_note", mcph_create_note},
    {"list_notes", mcph_list_notes},
    {"search_notes", mcph_search_notes},
    {"job_start", mcph_job_start},
    {"job_status", mcph_job_status},
    {"autopilot", mcph_autopilot},
    {"lsp_diagnostics", mcph_lsp_diagnostics},
    {"lsp_definition", mcph_lsp_definition_or_references},
    {"lsp_references", mcph_lsp_definition_or_references},
    {"session_context_search", mcph_session_context_search},
    {"session_context_expand", mcph_session_context_expand},
    {"session_context_status", mcph_session_context_status},
    {"session_search", mcph_session_search},
    {"payload_rewrite_status", mcph_payload_rewrite_status},
    {"compact_context", mcph_compact_context},
    {"set_primary_agent", mcph_set_primary_agent},
    {"upsert_persona", mcph_upsert_persona},
    {"upsert_role_template", mcph_upsert_role_template},
    /* P3 extended read-only tools */
    {"roadmap_list", mcph_roadmap_list},
    {"roadmap_show", mcph_roadmap_show},
    {"task_list", mcph_task_list},
    {"index_find_callers", mcph_index_find_callers},
    {"index_structure", mcph_index_structure},
    {"memory_explain_match", mcph_memory_explain_match},
};

static mcp_tool_handler_fn mcp_tool_lookup(const char *tool)
{
   for (size_t i = 0; i < sizeof(mcp_tool_table) / sizeof(mcp_tool_table[0]); i++)
      if (strcmp(mcp_tool_table[i].name, tool) == 0)
         return mcp_tool_table[i].fn;
   return NULL;
}
