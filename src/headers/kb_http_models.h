/* kb_http_models.h: /v1/models routes (P2a org model catalog + entitlement).
 *
 * GET /v1/models/entitled          — the caller's entitled models (tenant-scoped read).
 * POST /v1/models/org/add|set      — admin: create/update a catalog entry (WORM-audited).
 * POST /v1/models/org/remove       — admin: remove a catalog entry + its entitlements.
 * POST /v1/models/org/entitle      — admin: grant (model, team).
 * POST /v1/models/org/unentitle    — admin: revoke (model, team).
 *
 * The authenticated actor comes from kb_reqctx; the org-admin capability for the /org
 * mutations is enforced at the DB layer (kb_principal_is_admin inside the SECURITY
 * DEFINER functions) — a non-admin write surfaces here as 403. Catalog-only: the
 * entitled surface carries NO credential/slot field. */
#ifndef DEC_KB_HTTP_MODELS_H
#define DEC_KB_HTTP_MODELS_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Handle /v1/models* routes. Returns an HTTP status (>=0) when it handled the path,
    * or -1 when the path is not one of ours (router falls through). Writes the JSON
    * response into out_buf[out_cap]. */
   int kb_http_models_route(const char *method, const char *path, const char *body, char *out_buf,
                            int out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_HTTP_MODELS_H */
