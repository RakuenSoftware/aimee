/* server_css_query.inc: /v1/css/signals handler (included by server_state.c).
 *
 * The CSS migration assistant's style graph + component join + migration tables
 * live in the KB's DB2 (the KB runs the indexer), and aimee-server is built with
 * -DAIMEE_DB2_DISABLED. So this is a thin forward to the kb_handle_css_signals
 * action over kb_client, mirroring handle_evidence_provenance. */

/* Defined in kb_client.c; takes ownership of req. */
char *kb_v1_action_request(const char *method, cJSON *req);

int handle_css_signals(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *op = jo_str(req, "op", "");
   const char *project = jo_str(req, "project", "");
   if (!op[0])
      return server_send_error(conn, "css requires an 'op'", NULL);
   if (!project[0])
      return server_send_error(conn, "css requires a 'project'", NULL);

   /* Forward the whole validated request so op-specific fields (render-store's
    * unit/phase/snapshot, render-verify's unit, ...) pass through unchanged. */
   cJSON *kreq = cJSON_Duplicate(req, 1);
   char *json = kb_v1_action_request("css.signals", kreq);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "css signals query failed", NULL);
   return send_and_free(conn, resp);
}
