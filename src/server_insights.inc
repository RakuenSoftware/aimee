static int handle_insights_overview(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jdays = cJSON_GetObjectItemCaseSensitive(req, "days");
   int days = cJSON_IsNumber(jdays) ? (int)jdays->valuedouble : 30;
   if (days < 1)
      days = 1;
   if (days > 365)
      days = 365;
   int since_hours = days * 24;

   db1_token_audit_totals_t totals;
   memset(&totals, 0, sizeof(totals));
   if (db1_token_audit_totals(since_hours, &totals) != 0)
      return server_send_error(conn, "insights.overview: db1_token_audit_totals failed", NULL);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "days", days);
   cJSON_AddNumberToObject(resp, "total_calls", totals.total_calls);
   cJSON_AddNumberToObject(resp, "prompt_tokens", (double)totals.prompt_tokens);
   cJSON_AddNumberToObject(resp, "completion_tokens", (double)totals.completion_tokens);
   cJSON_AddNumberToObject(resp, "cache_write_tokens", (double)totals.cache_write_tokens);
   cJSON_AddNumberToObject(resp, "cache_read_tokens", (double)totals.cache_read_tokens);
   cJSON_AddNumberToObject(resp, "estimated_cost_usd", totals.estimated_cost_usd);

   /* Models */
   db1_token_audit_model_summary_t models[16];
   int n_models = db1_token_audit_by_model(since_hours, models, 16);
   cJSON *jmodels = cJSON_CreateArray();
   for (int i = 0; i < n_models; i++)
   {
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "model", models[i].model);
      cJSON_AddNumberToObject(m, "calls", models[i].calls);
      cJSON_AddNumberToObject(m, "prompt_tokens", (double)models[i].prompt_tokens);
      cJSON_AddNumberToObject(m, "completion_tokens", (double)models[i].completion_tokens);
      cJSON_AddNumberToObject(m, "estimated_cost_usd", models[i].estimated_cost_usd);
      cJSON_AddItemToArray(jmodels, m);
   }
   cJSON_AddItemToObject(resp, "models", jmodels);

   /* Sources (turn origin: internal "agent" vs the ingress surfaces) */
   db1_token_audit_source_summary_t sources[16];
   int n_sources = db1_token_audit_by_source(since_hours, sources, 16);
   cJSON *jsources = cJSON_CreateArray();
   for (int i = 0; i < n_sources; i++)
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "source", sources[i].source);
      cJSON_AddNumberToObject(s, "calls", sources[i].calls);
      cJSON_AddNumberToObject(s, "prompt_tokens", (double)sources[i].prompt_tokens);
      cJSON_AddNumberToObject(s, "completion_tokens", (double)sources[i].completion_tokens);
      cJSON_AddNumberToObject(s, "estimated_cost_usd", sources[i].estimated_cost_usd);
      cJSON_AddItemToArray(jsources, s);
   }
   cJSON_AddItemToObject(resp, "sources", jsources);

   /* Roles */
   db1_token_audit_role_summary_t roles[16];
   int n_roles = db1_token_audit_by_role(since_hours, roles, 16);
   cJSON *jroles = cJSON_CreateArray();
   for (int i = 0; i < n_roles; i++)
   {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "role", roles[i].role);
      cJSON_AddNumberToObject(r, "calls", roles[i].calls);
      cJSON_AddNumberToObject(r, "prompt_tokens", (double)roles[i].prompt_tokens);
      cJSON_AddNumberToObject(r, "completion_tokens", (double)roles[i].completion_tokens);
      cJSON_AddNumberToObject(r, "estimated_cost_usd", roles[i].estimated_cost_usd);
      cJSON_AddItemToArray(jroles, r);
   }
   cJSON_AddItemToObject(resp, "roles", jroles);

   /* Tools */
   db1_token_audit_tool_summary_t tools[16];
   int n_tools = db1_token_audit_by_tool(since_hours, tools, 16);
   cJSON *jtools = cJSON_CreateArray();
   for (int i = 0; i < n_tools; i++)
   {
      cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "tool", tools[i].tool_name);
      cJSON_AddNumberToObject(t, "calls", tools[i].calls);
      cJSON_AddNumberToObject(t, "prompt_tokens", (double)tools[i].prompt_tokens);
      cJSON_AddNumberToObject(t, "completion_tokens", (double)tools[i].completion_tokens);
      cJSON_AddNumberToObject(t, "estimated_cost_usd", tools[i].estimated_cost_usd);
      cJSON_AddItemToArray(jtools, t);
   }
   cJSON_AddItemToObject(resp, "tools", jtools);

   /* Platforms */
   db1_insights_platform_t platforms[8];
   int n_platforms = db1_insights_by_platform(since_hours, platforms, 8);
   cJSON *jplatforms = cJSON_CreateArray();
   for (int i = 0; i < n_platforms; i++)
   {
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "platform", platforms[i].platform);
      cJSON_AddNumberToObject(p, "session_count", platforms[i].session_count);
      cJSON_AddItemToArray(jplatforms, p);
   }
   cJSON_AddItemToObject(resp, "platforms", jplatforms);

   /* Top sessions */
   db1_insights_top_session_t top_sess[10];
   int n_top = db1_insights_top_sessions(since_hours, top_sess, 10);
   cJSON *jtop = cJSON_CreateArray();
   for (int i = 0; i < n_top; i++)
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "session_id", top_sess[i].session_id);
      cJSON_AddStringToObject(s, "title", top_sess[i].title);
      cJSON_AddStringToObject(s, "model", top_sess[i].model);
      cJSON_AddNumberToObject(s, "prompt_tokens", (double)top_sess[i].prompt_tokens);
      cJSON_AddNumberToObject(s, "completion_tokens", (double)top_sess[i].completion_tokens);
      cJSON_AddNumberToObject(s, "estimated_cost_usd", top_sess[i].estimated_cost_usd);
      cJSON_AddStringToObject(s, "created_at", top_sess[i].created_at);
      cJSON_AddItemToArray(jtop, s);
   }
   cJSON_AddItemToObject(resp, "top_sessions", jtop);

   /* Delegates by role */
   db1_insights_delegate_role_t deleg[16];
   int n_deleg = db1_insights_delegates_by_role(since_hours, deleg, 16);
   cJSON *jdeleg = cJSON_CreateArray();
   for (int i = 0; i < n_deleg; i++)
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "role", deleg[i].role);
      cJSON_AddNumberToObject(d, "total", deleg[i].total);
      cJSON_AddNumberToObject(d, "completed", deleg[i].completed);
      cJSON_AddItemToArray(jdeleg, d);
   }
   cJSON_AddItemToObject(resp, "delegates", jdeleg);

   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}
