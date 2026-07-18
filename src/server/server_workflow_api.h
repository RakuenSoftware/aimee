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

/* Create/edit a delegate custom block (blocks.yaml). body is JSON
 * {consumes, produces, persona, prompt}; command executors are refused. */
int wf_api_block_put(const char *name, const char *body, char *resp, int cap);
/* Delete a custom delegate block (command blocks are operator-only). */
int wf_api_block_delete(const char *name, char *resp, int cap);

/* GET /v1/workflow/defs -- list saved workflow definitions under
 * $AIMEE_HOME/workflows (name + valid + version). */
int wf_api_list(char *resp, int cap);

/* GET /v1/workflow/triggers -- the configured trigger rules (aimee.yaml
 * `trigger_rules`) that auto-start runs. Returns {max_concurrent, triggers:[
 * {source,event,schedule,mode,template,workspace,max_spend_usd?}]}. Read-only. */
int wf_api_triggers(char *resp, int cap);

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

/* GET /v1/workflow/items -- list work items OWNED BY the calling principal
 * (run-state rows). Ownership = item.submitter == server_http_identity_principal().
 * The Proposals page's default list. */
int wf_api_items(char *resp, int cap);

/* GET /v1/workflow/items/all -- list ALL work items regardless of submitter.
 * Route-gated by CAP_WORKFLOW_ADMIN (operator view). */
int wf_api_items_all(char *resp, int cap);

/* GET /v1/workflow/items/{id} -- one work item's run-state. Owner-only: 403 if
 * the item's submitter is not the calling principal. 404 if unknown. */
int wf_api_item(const char *id, char *resp, int cap);

/* GET /v1/workflow/items/{id}/events?after=<id>&limit=<n> -- the append-only
 * lifecycle timeline (proposal history), oldest-first, paginated by event id.
 * Owner-only (403). Returns {events:[{id,stage,kind,actor,detail,cost_usd,
 * created_at}], next_after}. `after` (default 0) returns events with id>after;
 * `limit` is clamped to [1,200] (default 200); `next_after` is the id of the last
 * event returned (echoes `after` when the page is empty). */
int wf_api_events(const char *id, long after, int limit, char *resp, int cap);

/* GET /v1/workflow/items/{id}/proposal -- the source proposal markdown the run is
 * executing. Owner-only (403). Reads the item's server-minted proposal_path,
 * confined to $AIMEE_HOME/workflows/proposals via a dirfd + openat(O_NOFOLLOW).
 * Returns {proposal_md, truncated}; 404 if the file is missing. */
int wf_api_proposal(const char *id, char *resp, int cap);

/* Lifecycle mutations on one run. Access = the item's submitter OR an operator
 * (is_operator, derived from the caller's CAP_WORKFLOW_ADMIN); else 403. Each
 * returns the updated item row (or an error envelope). `is_operator` non-zero
 * lifts the owner-only restriction. 404 unknown id, 409 on an invalid transition.
 *
 * pause  -- park an active, un-paused run with pause_reason=operator_paused so the
 *           scheduler stops advancing it.
 * resume -- clear the pause and (caller then wakes the scheduler). 409 if the run
 *           is parked at pending_human (that must go through Approve/Reject).
 * stop   -- set the run terminal (abandoned); the scheduler reclaims its worktree.
 * delete -- if still active, abandon first, then permanently remove the run's rows
 *           and best-effort unlink its proposal file. */
int wf_api_item_pause(const char *id, int is_operator, char *resp, int cap);
int wf_api_item_resume(const char *id, int is_operator, char *resp, int cap);
int wf_api_item_stop(const char *id, int is_operator, char *resp, int cap);
int wf_api_item_delete(const char *id, int is_operator, char *resp, int cap);

/* GET /v1/workflow/repo/tree?path=<rel> -- list immediate directory entries under
 * the server's local project checkout (root = $AIMEE_WORKFLOW_REPO, else cwd), for
 * the composer's "load a proposal from the project" browser. Returns {path,
 * entries:[{name,type:"dir"|"file"}]} with directories + `.md` files only; hidden
 * dirs and node_modules are skipped. Path-confined via realpath (rejects `..` /
 * symlink escape): 400 on escape, 404 on a missing dir. */
int wf_api_repo_tree(const char *rel, char *resp, int cap);

/* GET /v1/workflow/repo/file?path=<rel> -- read one `.md` file under the same
 * confined project root. Returns {path, content, truncated} (size-capped like the
 * proposal read). 400 on escape / non-.md, 404 if missing / not a regular file. */
int wf_api_repo_file(const char *rel, char *resp, int cap);

#endif /* DEC_SERVER_WORKFLOW_API_H */
