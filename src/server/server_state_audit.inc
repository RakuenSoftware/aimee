/* server_state_audit.inc — auditable-correctness dispatch handlers for
 * server_state.c. Textually included (shares its file-static helpers like
 * send_and_free); split out only to keep server_state.c under the line cap. */

/* /v1/audit/trace handler. Its op (evidence.trace_retrieval_event) is a KB-only
 * action, so — like handle_kb_search and the curator handlers — the server
 * forwards it to aimee-kb via kb_client and passes the JSON response through.
 * rh_dispatch_op alone cannot reach a KB-only method (server_dispatch resolves
 * only server-side methods), which is why the cloned /v1/code/audit route never
 * reached the KB. */
int handle_evidence_trace(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *turn_id = jo_str(req, "turn_id", "");
   if (!turn_id[0])
      return server_send_error(conn, "audit trace requires turn_id", NULL);
   char *json = kb_client_evidence_trace_retrieval_event(turn_id);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service audit trace failed", NULL);
   return send_and_free(conn, resp);
}

/* /v1/audit/provenance handler. Like handle_evidence_trace, its op
 * (evidence.provenance_retrieval_event) is a KB-only action, so the server
 * forwards it to aimee-kb via kb_client and passes the JSON response through. */
int handle_evidence_provenance(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *turn_id = jo_str(req, "turn_id", "");
   if (!turn_id[0])
      return server_send_error(conn, "audit provenance requires turn_id", NULL);
   char *json = kb_client_evidence_provenance_retrieval_event(turn_id);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service audit provenance failed", NULL);
   return send_and_free(conn, resp);
}

/* /v1/audit/fidelity handler. Like the trace/provenance handlers, its op
 * (evidence.fidelity_retrieval_event) is a KB-only action (the fidelity_report /
 * fidelity_attribution artifacts live in DB2), so the server forwards it to
 * aimee-kb via kb_client and passes the JSON response through. */
int handle_evidence_fidelity(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *turn_id = jo_str(req, "turn_id", "");
   if (!turn_id[0])
      return server_send_error(conn, "audit fidelity requires turn_id", NULL);
   char *json = kb_client_evidence_fidelity_retrieval_event(turn_id);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service audit fidelity failed", NULL);
   return send_and_free(conn, resp);
}
