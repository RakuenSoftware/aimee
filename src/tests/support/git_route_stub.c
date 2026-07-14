/* git_route_stub.c: no-op stubs for the WP-D clone route's collaborators, for
 * tests that link server_http.o (whose rh_workspace_clone references
 * git_project_clone + index_scan_project — retained under CI LTO) but exercise
 * only HTTP parsing / routing, not the real git-clone or indexer subsystems.
 * Binaries that need the real behavior (unit-test-git-project) link the real
 * objects and must NOT also link this TU. */
#include "deploy_apply.h"
#include "git_host_cred.h"
#include "git_oauth_device.h"
#include "git_oauth_gh.h"
#include "git_oauth_github.h"
#include "git_ops.h"
#include "git_org_repos.h"
#include "git_project.h"
#include "git_ssh_agent.h"
#include "index.h"
#include "webuser_editor.h"
#include "workspace_scope.h"

#include <stdio.h>

int git_project_clone(const char *principal, const char *url, const char *name, const char *org,
                      const char *token, char *out_path, size_t path_cap, char *out_ref,
                      size_t ref_cap, char *err, size_t errlen)
{
   (void)principal;
   (void)url;
   (void)name;
   (void)org;
   (void)token;
   if (out_path && path_cap)
      out_path[0] = '\0';
   if (out_ref && ref_cap)
      out_ref[0] = '\0';
   if (err && errlen)
      snprintf(err, errlen, "stub");
   return -1;
}

int git_project_derive_org(const char *url, char *out, size_t cap, int *multi_segment)
{
   (void)url;
   if (out && cap)
      out[0] = '\0';
   if (multi_segment)
      *multi_segment = 0;
   return -1;
}

int git_project_remote(const char *principal, const char *ref, char *out, size_t cap)
{
   (void)principal;
   (void)ref;
   if (out && cap)
      out[0] = '\0';
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

int git_org_repos_list(const char *host, const char *owner, struct cJSON **out, char *provider,
                       size_t provider_cap, char *err, size_t errlen)
{
   (void)host;
   (void)owner;
   if (out)
      *out = NULL;
   if (provider && provider_cap)
      provider[0] = '\0';
   if (err && errlen)
      snprintf(err, errlen, "stub");
   return 502;
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

int git_ops_run_session(const char *principal, const char *project, const char *session_id,
                        const char *op, const char *text_arg, int num_arg, char **out, char *err,
                        size_t errlen)
{
   (void)session_id;
   return git_ops_run(principal, project, op, text_arg, num_arg, out, err, errlen);
}

int git_ops_session_dir(const char *principal, const char *project, const char *session_id,
                        char *out, size_t out_len, char *err, size_t errlen)
{
   (void)principal;
   (void)project;
   (void)session_id;
   if (out && out_len)
      out[0] = '\0';
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

int git_oauth_github_available(void)
{
   return 0;
}

int git_oauth_github_start(const char *principal, char *user_code, size_t uc_len, char *verify_uri,
                           size_t vu_len, int *interval, char *err, size_t errlen)
{
   (void)principal;
   (void)user_code;
   (void)uc_len;
   (void)verify_uri;
   (void)vu_len;
   (void)interval;
   if (err && errlen)
      err[0] = '\0';
   return -1;
}

int git_oauth_github_poll(const char *principal, char *err, size_t errlen)
{
   (void)principal;
   if (err && errlen)
      err[0] = '\0';
   return -1;
}

int git_oauth_github_set_client_id(const char *client_id)
{
   (void)client_id;
   return -1;
}

int git_oauth_github_get_client_id(char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   return 0;
}

int git_oauth_github_set_client_secret(const char *client_secret)
{
   (void)client_secret;
   return -1;
}

int git_oauth_github_web_available(void)
{
   return 0;
}

int git_oauth_github_web_start(const char *principal, const char *redirect_uri, char *out_url,
                               size_t url_len, char *err, size_t errlen)
{
   (void)principal;
   (void)redirect_uri;
   if (out_url && url_len)
      out_url[0] = '\0';
   if (err && errlen)
      err[0] = '\0';
   return -1;
}

int git_oauth_github_web_callback(const char *principal, const char *code, const char *state,
                                  char *err, size_t errlen)
{
   (void)principal;
   (void)code;
   (void)state;
   if (err && errlen)
      err[0] = '\0';
   return -1;
}

int git_oauth_gh_available(void)
{
   return 0;
}

int git_oauth_gh_pending(void)
{
   return 0;
}

int git_oauth_gh_start(const char *principal, char *user_code, size_t uc_len, char *verify_uri,
                       size_t vu_len, int *interval, char *err, size_t errlen)
{
   (void)principal;
   (void)user_code;
   (void)uc_len;
   (void)verify_uri;
   (void)vu_len;
   (void)interval;
   if (err && errlen)
      err[0] = '\0';
   return -1;
}

int git_oauth_gh_poll(const char *principal, char *err, size_t errlen)
{
   (void)principal;
   if (err && errlen)
      err[0] = '\0';
   return -1;
}

/* GitLab/Gitea device-flow stubs (git_oauth_device.h) — the HTTP-routing tests link
 * server_http_routes.o (whose device-flow oauth handlers reference these) but
 * exercise only routing, not the real OAuth device grant. */
int oauth_dev_provider_from_name(const char *name, oauth_dev_provider_t *out)
{
   (void)name;
   (void)out;
   return -1;
}

const char *oauth_dev_provider_name(oauth_dev_provider_t p)
{
   (void)p;
   return "gitea";
}

int oauth_dev_available(oauth_dev_provider_t p, const char *host)
{
   (void)p;
   (void)host;
   return 0;
}

int oauth_dev_set_client_id(oauth_dev_provider_t p, const char *host, const char *client_id)
{
   (void)p;
   (void)host;
   (void)client_id;
   return -1;
}

int oauth_dev_get_client_id(oauth_dev_provider_t p, const char *host, char *out, size_t cap)
{
   (void)p;
   (void)host;
   if (out && cap)
      out[0] = '\0';
   return 0;
}

int oauth_dev_start(oauth_dev_provider_t p, const char *host, const char *principal,
                    char *user_code, size_t uc_len, char *verify_uri, size_t vu_len, int *interval,
                    char *err, size_t errlen)
{
   (void)p;
   (void)host;
   (void)principal;
   if (user_code && uc_len)
      user_code[0] = '\0';
   if (verify_uri && vu_len)
      verify_uri[0] = '\0';
   (void)interval;
   if (err && errlen)
      err[0] = '\0';
   return -1;
}

int oauth_dev_poll(oauth_dev_provider_t p, const char *host, const char *principal, char *err,
                   size_t errlen)
{
   (void)p;
   (void)host;
   (void)principal;
   if (err && errlen)
      err[0] = '\0';
   return -1;
}

/* Server-orchestrated deploy stubs (deploy_apply.h): the HTTP-routing tests link
 * the relocated /v1/deploy handlers but never launch containers. */
int deploy_apply_enabled(void)
{
   return 0;
}

int deploy_apply_start(void)
{
   return -1;
}

void deploy_apply_state(int *running, int *last_exit, char *out, size_t out_cap)
{
   if (running)
      *running = 0;
   if (last_exit)
      *last_exit = 0;
   if (out && out_cap)
      out[0] = '\0';
}

int deploy_apply_status(char *out, size_t out_cap, int *exit_code)
{
   if (out && out_cap)
      out[0] = '\0';
   if (exit_code)
      *exit_code = -1;
   return -1;
}

int ws_scope_user_root(const char *principal, int create, char *out, size_t cap)
{
   (void)principal;
   (void)create;
   if (out && cap)
      out[0] = '\0';
   return -1;
}

/* The git credential/ssh-key routes drop any live ssh-agent handle on
 * revoke/replace; a no-op suffices for HTTP-routing tests. */
void git_ssh_agent_stop(const char *principal)
{
   (void)principal;
}
