/* kb_http_search.h: POST /v1/search typed-facet artifact filter (deep-curator). */
#ifndef DEC_KB_HTTP_SEARCH_H
#define DEC_KB_HTTP_SEARCH_H 1

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

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_HTTP_SEARCH_H */
