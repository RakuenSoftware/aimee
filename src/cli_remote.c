/* cli_remote.c: `aimee remote` subcommand + persisted remote-server config.
 *
 * Thin-client only. The remote target lives in <aimee_home>/remote.conf as two
 * lines: the URL, then an optional bearer token. This keeps the thin client
 * free of config.c (which it does not link) while letting users point at a
 * remote aimee-server persistently.
 */
#include "cli_remote.h"
#include "aimee_client.h"
#include "aimee_home.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define AIMEE_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define AIMEE_MKDIR(p) mkdir((p), 0700)
#endif

static void remote_conf_path(char *out, size_t out_sz)
{
   snprintf(out, out_sz, "%s/remote.conf", aimee_home());
}

/* Create |dir| and any missing parents (best effort, ignores existing/errors).
 * A thin-client-only host (the README's recommended deployment) has nothing
 * else that creates <aimee_home> (~/.config/aimee), so `aimee remote set` must
 * make it before writing remote.conf or the first call fails with ENOENT. */
static void ensure_dir_p(const char *dir)
{
   if (!dir || !dir[0])
      return;
   char tmp[512];
   snprintf(tmp, sizeof(tmp), "%s", dir);
   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         AIMEE_MKDIR(tmp);
         *p = '/';
      }
   }
   AIMEE_MKDIR(tmp);
}

/* Read the persisted URL (line 1) and token (line 2, optional) into the given
 * buffers. Returns 1 if a non-empty URL was read, else 0. */
static int read_remote_conf(char *url, size_t url_sz, char *token, size_t token_sz)
{
   if (url_sz)
      url[0] = '\0';
   if (token_sz)
      token[0] = '\0';
   char path[512];
   remote_conf_path(path, sizeof(path));
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   if (fgets(url, (int)url_sz, f))
      url[strcspn(url, "\r\n")] = '\0';
   if (fgets(token, (int)token_sz, f))
      token[strcspn(token, "\r\n")] = '\0';
   fclose(f);
   return url[0] != '\0';
}

void cli_remote_load_persisted(void)
{
   /* A --server flag or AIMEE_SERVER_URL already wins; don't override it. */
   if (aimee_client_remote_active(NULL, 0))
      return;
   char url[512], token[256];
   if (read_remote_conf(url, sizeof(url), token, sizeof(token)) && url[0])
      aimee_client_set_remote(url, token[0] ? token : NULL);
}

static int remote_set(const char *url, const char *token, int json_output)
{
   if (!url || !*url)
   {
      fprintf(stderr, "usage: aimee remote set <url> [token]\n");
      return 2;
   }
   char path[512];
   remote_conf_path(path, sizeof(path));
   ensure_dir_p(aimee_home());
   FILE *f = fopen(path, "w");
   if (!f)
   {
      fprintf(stderr, "aimee: cannot write %s\n", path);
      return 1;
   }
   fprintf(f, "%s\n%s\n", url, token ? token : "");
   fclose(f);
   if (json_output)
      printf("{\"ok\":true,\"url\":\"%s\",\"token\":%s}\n", url,
             token && *token ? "true" : "false");
   else
      printf("Remote server set to %s%s\n", url, token && *token ? " (with token)" : "");
   return 0;
}

static int remote_clear(int json_output)
{
   char path[512];
   remote_conf_path(path, sizeof(path));
   int removed = (remove(path) == 0);
   if (json_output)
      printf("{\"ok\":true,\"cleared\":%s}\n", removed ? "true" : "false");
   else
      printf(removed ? "Remote server config cleared.\n" : "No persisted remote server config.\n");
   return 0;
}

static int remote_status(int json_output)
{
   /* Apply persisted config so status reflects what a real command would use. */
   cli_remote_load_persisted();
   char desc[300] = {0};
   int active = aimee_client_remote_active(desc, sizeof(desc));

   /* Probe reachability over whichever transport is in effect. */
   int st = 0;
   char *body = aimee_client_request("GET", "/v1/health", NULL, &st);
   int reachable = (body != NULL && st >= 200 && st < 500);
   free(body);

   if (json_output)
   {
      printf("{\"remote\":%s,\"target\":\"%s\",\"reachable\":%s,\"status\":%d}\n",
             active ? "true" : "false", active ? desc : "local-uds", reachable ? "true" : "false",
             st);
   }
   else
   {
      if (active)
         printf("Transport: remote TCP -> %s\n", desc);
      else
         printf("Transport: local Unix socket (no remote configured)\n");
      printf("Reachable: %s (GET /v1/health -> %d)\n", reachable ? "yes" : "no", st);
   }
   return reachable ? 0 : 1;
}

int cli_remote_cmd(int argc, char **argv, int json_output)
{
   const char *sub = argc > 0 ? argv[0] : "status";
   if (strcmp(sub, "set") == 0)
      return remote_set(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL, json_output);
   if (strcmp(sub, "clear") == 0)
      return remote_clear(json_output);
   if (strcmp(sub, "status") == 0)
      return remote_status(json_output);
   fprintf(stderr, "usage: aimee remote <set <url> [token] | status | clear>\n");
   return 2;
}
