/* git_route_stub.c: no-op stubs for the WP-D clone route's collaborators, for
 * tests that link server_http.o (whose rh_workspace_clone references
 * git_project_clone + index_scan_project — retained under CI LTO) but exercise
 * only HTTP parsing / routing, not the real git-clone or indexer subsystems.
 * Binaries that need the real behavior (unit-test-git-project) link the real
 * objects and must NOT also link this TU. */
#include "git_host_cred.h"
#include "git_ops.h"
#include "git_project.h"
#include "index.h"
#include "webuser_editor.h"

#include <stdio.h>

int git_project_clone(const char *principal, const char *url, const char *name, const char *token,
                      char *out_path, size_t path_cap, char *out_name, size_t name_cap, char *err,
                      size_t errlen)
{
   (void)principal;
   (void)url;
   (void)name;
   (void)token;
   if (out_path && path_cap)
      out_path[0] = '\0';
   if (out_name && name_cap)
      out_name[0] = '\0';
   if (err && errlen)
      snprintf(err, errlen, "stub");
   return -1;
}

int index_scan_project(const char *name, const char *root, int force)
{
   (void)name;
   (void)root;
   (void)force;
   return 0;
}

int git_project_list(const char *principal, char out[][GIT_PROJECT_NAME_MAX], int max)
{
   (void)principal;
   (void)out;
   (void)max;
   return 0;
}

int git_ops_run(const char *principal, const char *project, const char *op, const char *text_arg,
                int num_arg, char **out, char *err, size_t errlen)
{
   (void)principal;
   (void)project;
   (void)op;
   (void)text_arg;
   (void)num_arg;
   if (out)
      *out = NULL;
   if (err && errlen)
      snprintf(err, errlen, "stub");
   return -1;
}

int webuser_editor_ensure(const char *principal, int *out_port, char *err, size_t errlen)
{
   (void)principal;
   if (out_port)
      *out_port = 0;
   if (err && errlen)
      err[0] = '\0';
   return 0; /* feature unavailable in the stub */
}

int git_host_cred_set(const char *host, const char *token)
{
   (void)host;
   (void)token;
   return -1;
}

int git_host_cred_delete(const char *host)
{
   (void)host;
   return -1;
}

int git_host_cred_list(char out[][GIT_HOST_MAX], int max)
{
   (void)out;
   (void)max;
   return 0;
}

int git_host_from_url(const char *url, char *out, size_t out_len)
{
   (void)url;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}
