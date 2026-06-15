/* server_workflow_api.h -- /v1/workflow read+author surface for the web visual
 * composer (W7). Thin HTTP layer over the wfe_ definition model (parse/validate/
 * canonical/version, all CORE) + the DB1 work-item store for run-state. Each
 * function fills `resp` with a JSON body and returns the HTTP status. They are
 * self-contained (own error envelopes) so the route adapters stay one-liners. */
#ifndef DEC_SERVER_WORKFLOW_API_H
#define DEC_SERVER_WORKFLOW_API_H 1

/* GET /v1/workflow/blocks -- the composable block catalog (built-ins + the
 * config-defined custom registry) with each block's typed I/O for the palette. */
int wf_api_blocks(char *resp, int cap);

/* GET /v1/workflow/defs -- list saved workflow definitions under
 * $AIMEE_HOME/workflows (name + valid + version). */
int wf_api_list(char *resp, int cap);

/* GET /v1/workflow/defs/{name} -- one definition: canonical form, version, and a
 * structured node graph for rendering. 400 on an unsafe name, 404 if missing. */
int wf_api_get(const char *name, char *resp, int cap);

/* POST /v1/workflow/validate {"yaml":"..."} -- validate posted text without
 * saving; returns {valid, error?, name, version, canonical, def}. */
int wf_api_validate(const char *body, char *resp, int cap);

/* POST /v1/workflow/save {"name","yaml","prev_version"} -- canonical-normalize +
 * write with an optimistic lock on prev_version (409 on mismatch; create requires
 * an empty prev_version and a non-existent file). 400 on invalid def/name. */
int wf_api_save(const char *body, char *resp, int cap);

/* GET /v1/workflow/items -- list work items (run-state rows). */
int wf_api_items(char *resp, int cap);

/* GET /v1/workflow/items/{id} -- one work item's run-state (workflow, pinned
 * version, current stage, state, pause reason). 404 if unknown. */
int wf_api_item(const char *id, char *resp, int cap);

#endif /* DEC_SERVER_WORKFLOW_API_H */
