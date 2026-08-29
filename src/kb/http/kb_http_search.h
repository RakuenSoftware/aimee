/* kb_http_search.h: POST /v1/search typed-facet artifact filter (deep-curator). */
#ifndef DEC_KB_HTTP_SEARCH_H
#define DEC_KB_HTTP_SEARCH_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Handle the typed-facet branch of POST /v1/search. When |body| carries a
    * non-empty "filters" object, narrow live artifacts over their narrative
    * payload (status / priority / component) and kind, write a facet-shaped
    * search response into out_buf, and return the HTTP status (200, or 503 on
    * allocation/backend failure). Returns -1 when no filters are present, so
    * the caller falls through to the default ranked search path. */
   int kb_http_search_facets(const char *body, const char *preferred_project, int all_projects,
                             char *out_buf, int out_cap);

   /* Resolve the caller-bound project scope shared by the facet and ranked
    * search paths. Returns zero on success or an HTTP status after writing the
    * error response. */
   int kb_http_search_project_scope(const char *body, char *project, size_t project_cap,
                                    int *all_projects, char *out_buf, int out_cap);

   /* Reject dependency/storage errors and malformed ranked-backend envelopes
    * before the public route reshapes their results. */
   int kb_http_search_validate_backend(const char *body, char *out_buf, int out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_HTTP_SEARCH_H */
