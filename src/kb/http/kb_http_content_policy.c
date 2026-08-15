#include "kb_http.h"
#include <stddef.h>
#include <string.h>

typedef enum
{
   KB_CONTENT_EXACT,
   KB_CONTENT_PREFIX,
} kb_content_match_t;

typedef struct
{
   const char *method;
   const char *path;
   kb_content_match_t match;
   int content;
} kb_content_route_t;

/* TLS content authorization and tenant-scope setup both consume this table.
 * Denials precede broad prefixes so lifecycle/admin endpoints cannot acquire
 * read semantics through a misleading GET. Every content family belongs here,
 * not in a second ingress-specific allowlist. */
static const kb_content_route_t g_kb_content_routes[] = {
    {"GET", "/v1/code/build", KB_CONTENT_EXACT, 0},
    {"GET", "/v1/code/update", KB_CONTENT_EXACT, 0},
    {"GET", "/v1/code/scan", KB_CONTENT_EXACT, 0},
    {"GET", "/v1/code/project/detach", KB_CONTENT_EXACT, 0},
    {"GET", "/v1/code/project/purge", KB_CONTENT_EXACT, 0},
    {"GET", "/v1/code/project/gc", KB_CONTENT_EXACT, 0},
    {"GET", "/v1/code/lessons/observe", KB_CONTENT_EXACT, 0},
    {"GET", "/v1/code/repo-trust", KB_CONTENT_EXACT, 0},
    {"GET", "/v1/docs/manifest", KB_CONTENT_EXACT, 0},
    {"POST", "/v1/search", KB_CONTENT_EXACT, 1},
    {"POST", "/v1/implements", KB_CONTENT_EXACT, 1},
    {"POST", "/v1/synthesize", KB_CONTENT_EXACT, 1},
    {"POST", "/v1/contradictions", KB_CONTENT_EXACT, 1},
    {"POST", "/v1/entities/search", KB_CONTENT_EXACT, 1},
    {"GET", "/v1/review", KB_CONTENT_EXACT, 1},
    {"GET", "/v1/releases/active", KB_CONTENT_EXACT, 1},
    {"GET", "/v1/artifacts/", KB_CONTENT_PREFIX, 1},
    {"GET", "/v1/entities/", KB_CONTENT_PREFIX, 1},
    {"GET", "/v1/pdf/", KB_CONTENT_PREFIX, 1},
    {"GET", "/v1/code/", KB_CONTENT_PREFIX, 1},
    {"GET", "/v1/docs/", KB_CONTENT_PREFIX, 1},
    {NULL, NULL, KB_CONTENT_EXACT, 0},
};

int kb_http_is_content_read(const char *method, const char *path)
{
   if (!method || !path)
      return 0;
   for (const kb_content_route_t *route = g_kb_content_routes; route->method; ++route)
   {
      if (strcmp(method, route->method) != 0)
         continue;
      int matches = route->match == KB_CONTENT_EXACT
                        ? strcmp(path, route->path) == 0
                        : strncmp(path, route->path, strlen(route->path)) == 0;
      if (matches)
         return route->content;
   }
   return 0;
}
