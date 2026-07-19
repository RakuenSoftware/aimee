/* kb_http_team.h: /v1/team and /v1/project routes (P1 slice 4). */
#ifndef DEC_KB_HTTP_TEAM_H
#define DEC_KB_HTTP_TEAM_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Handle /v1/team* and /v1/project* routes. Returns an HTTP status (>=0) when it
    * handled the path, or -1 when the path is not one of ours (router falls through).
    * Writes the JSON response into out_buf[out_cap]. Reads the authenticated actor
    * from kb_reqctx; org-admin capability is enforced at the DB layer (RLS write
    * policies), so a non-admin create returns 403. */
   int kb_http_team_route(const char *method, const char *path, const char *query_string,
                          const char *body, char *out_buf, int out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_HTTP_TEAM_H */
