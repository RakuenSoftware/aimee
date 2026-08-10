/* cli_index_bootstrap.c: make a thin client's canonical repository visible to
 * the remote code index before session isolation enters a hidden worktree. */
#include "cli_client.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>

int cli_index_ensure_remote(const char *root)
{
   if (!root || !root[0] || !cli_v1_remote_endpoint_is_network())
      return 0;

   char *abs = realpath(root, NULL);
   if (!abs)
   {
      fprintf(stderr, "aimee: index bootstrap: cannot resolve repository '%s'\n", root);
      return 1;
   }
   const char *base = strrchr(abs, '/');
   base = (base && base[1]) ? base + 1 : abs;

   char *remote = cli_v1_client_endpoint();
   char *bearer = cli_v1_client_bearer();
   const char *verb = NULL;
   const char *path = cli_v1_route_for_method("index.list", &verb);
   int status = 0;
   cJSON *resp = NULL;
   if (remote && path)
      resp = cli_http_request(remote, verb ? verb : "POST", path, "{}", bearer, 15000, &status);

   /* A failed list is not evidence that the project is absent. Do not turn one
    * unavailable dependency into a long, doomed upload during MCP initialize. */
   cJSON *projects = resp ? cJSON_GetObjectItemCaseSensitive(resp, "projects") : NULL;
   if (!resp || status < 200 || status >= 300 || !cJSON_IsArray(projects))
   {
      cJSON_Delete(resp);
      free(remote);
      free(bearer);
      free(abs);
      return 1;
   }

   int present = 0;
   cJSON *project = NULL;
   cJSON_ArrayForEach(project, projects)
   {
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(project, "name"));
      const char *indexed_root =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(project, "root"));
      if (name && indexed_root && strcmp(name, base) == 0 && strcmp(indexed_root, abs) == 0)
      {
         present = 1;
         break;
      }
   }
   cJSON_Delete(resp);
   free(remote);
   free(bearer);

   if (present)
   {
      free(abs);
      return 0;
   }

   /* The server cannot read this path. Reuse the explicit remote scan path,
    * which walks the repository on the client and uploads bounded batches to
    * /v1/index/ingest. It records the canonical root/project rather than the
    * hidden per-session worktree the MCP proxy enters later. */
   char *scan_argv[] = {abs};
   int rc = cli_index_scan_remote(1, scan_argv);
   free(abs);
   return rc;
}

#else

int cli_index_ensure_remote(const char *root)
{
   (void)root;
   return 0;
}

#endif
