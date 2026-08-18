/* cli_launch.c: parse __LAUNCH__ metadata from server output */
#include "cli_client.h"
#include "aimee_client.h"
#include "cJSON.h"
#include "client_session_worktree.h"
#include "platform_process.h"
#include "platform_path.h"
#include "platform_random.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#include <process.h>
#define launch_chdir _chdir
/* _execvp takes a const-qualified vector where POSIX execvp does not, so the
 * one call site cannot spell both. Cast here rather than at the call, which
 * keeps that line one shape on both platforms -- and keeps the POSIX spelling
 * exactly `execvp`, which is what the unit test interposes on. */
static int launch_execvp(const char *file, char *const argv[])
{
   return (int)_execvp(file, (const char *const *)argv);
}
#else
#include <unistd.h>
#define launch_chdir  chdir
#define launch_execvp execvp
#endif

int parse_launch_meta(const char *output, launch_meta_t *meta)
{
   memset(meta, 0, sizeof(*meta));
   snprintf(meta->provider, sizeof(meta->provider), "claude");

   const char *launch_marker = strstr(output, "__LAUNCH__");
   if (!launch_marker)
      return 0;

   meta->context_len = (size_t)(launch_marker - output);

   const char *json_start = launch_marker + strlen("__LAUNCH__");
   const char *line_end = strchr(json_start, '\n');
   size_t json_len = line_end ? (size_t)(line_end - json_start) : strlen(json_start);
   char *json_buf = malloc(json_len + 1);
   if (!json_buf)
      return 0;

   memcpy(json_buf, json_start, json_len);
   json_buf[json_len] = '\0';

   cJSON *jmeta = cJSON_Parse(json_buf);
   free(json_buf);
   if (!jmeta)
      return 0;

   cJSON *jprovider = cJSON_GetObjectItemCaseSensitive(jmeta, "provider");
   cJSON *jmodel = cJSON_GetObjectItemCaseSensitive(jmeta, "model");
   cJSON *jbuiltin = cJSON_GetObjectItemCaseSensitive(jmeta, "builtin");
   cJSON *jauto = cJSON_GetObjectItemCaseSensitive(jmeta, "autonomous");
   cJSON *jwt_cwd = cJSON_GetObjectItemCaseSensitive(jmeta, "worktree_cwd");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(jmeta, "session_id");

   if (cJSON_IsString(jprovider))
      snprintf(meta->provider, sizeof(meta->provider), "%s", jprovider->valuestring);
   if (cJSON_IsString(jmodel))
      snprintf(meta->model, sizeof(meta->model), "%s", jmodel->valuestring);
   meta->builtin = cJSON_IsTrue(jbuiltin);
   meta->autonomous = cJSON_IsTrue(jauto);
   if (cJSON_IsString(jwt_cwd))
      snprintf(meta->worktree_cwd, sizeof(meta->worktree_cwd), "%s", jwt_cwd->valuestring);
   if (cJSON_IsString(jsid))
      snprintf(meta->session_id, sizeof(meta->session_id), "%s", jsid->valuestring);

   cJSON_Delete(jmeta);
   return 1;
}

static void launch_session_id(char out[33])
{
   unsigned char rnd[16];
   if (platform_random_bytes(rnd, sizeof(rnd)) != 0)
   {
      uint64_t mix = (uint64_t)time(NULL) ^ ((uint64_t)platform_getppid() << 32);
      for (size_t i = 0; i < sizeof(rnd); i++)
      {
         mix = mix * 6364136223846793005ULL + 1442695040888963407ULL;
         rnd[i] = (unsigned char)(mix >> 56);
      }
   }
   for (size_t i = 0; i < sizeof(rnd); i++)
      snprintf(out + (i * 2), 3, "%02x", rnd[i]);
}

/* Route launched provider clients through the same conversation gateway. This
 * is expressed with standard API environments, not client hooks or persona-
 * shaped arguments: adapters choose a wire protocol while shared IR ingress
 * owns persona placement for all of them. */
static int launch_route_conversation_gateway(const char *session_id)
{
   char remote[512];
   if (!aimee_client_remote_active(remote, sizeof remote))
   {
      fprintf(stderr,
              "aimee: launch requires a model-capable HTTP Aimee server; configure one with "
              "`aimee remote set` (the local Unix socket cannot carry provider traffic)\n");
      return -1;
   }

   const char *resolved = aimee_client_transport_label();
   if (!resolved || !strstr(resolved, "://"))
   {
      fprintf(stderr, "aimee: configured Aimee server is not an HTTP conversation gateway\n");
      return -1;
   }

   char origin[512];
   snprintf(origin, sizeof origin, "%s", resolved);
   size_t n = strlen(origin);
   while (n > 0 && origin[n - 1] == '/')
      origin[--n] = '\0';
   if (n >= 3 && strcmp(origin + n - 3, "/v1") == 0)
   {
      n -= 3;
      origin[n] = '\0';
   }

   char openai_base[520];
   snprintf(openai_base, sizeof openai_base, "%s/v1", origin);
   char token[512];
   if (!aimee_client_remote_token(token, sizeof token))
      token[0] = '\0';
   const char *base_auth = token[0] ? token : "aimee-local";
   char auth[640];
   snprintf(auth, sizeof auth, "%s.aimee-session.%s", base_auth, session_id);

   if (platform_setenv("AIMEE_CONVERSATION_GATEWAY", openai_base) != 0 ||
       platform_setenv("OPENAI_BASE_URL", openai_base) != 0 ||
       platform_setenv("OPENAI_API_KEY", auth) != 0 ||
       platform_setenv("ANTHROPIC_BASE_URL", origin) != 0 ||
       platform_setenv("ANTHROPIC_AUTH_TOKEN", auth) != 0)
   {
      fprintf(stderr, "aimee: could not route the launched client through Aimee ingress\n");
      return -1;
   }
   return 1;
}

int client_launch_exec(int argc, char **argv)
{
   if (argc > 0 && strcmp(argv[0], "--") == 0)
   {
      argc--;
      argv++;
   }
   if (argc <= 0 || !argv || !argv[0] || !argv[0][0])
   {
      fprintf(stderr, "usage: aimee launch -- <client> [args...]\n");
      return 2;
   }

   char sid[33];
   launch_session_id(sid);
   if (platform_setenv("AIMEE_SESSION_ID", sid) != 0)
   {
      fprintf(stderr, "aimee: could not establish the launch session id\n");
      return 1;
   }

   if (launch_route_conversation_gateway(sid) < 0)
      return 1;

   char worktree[4096];
   int wt = client_session_worktree_ensure(sid, worktree, sizeof(worktree));
   if (wt == -2)
      return 1;
   if (wt == 0 && launch_chdir(worktree) != 0)
   {
      fprintf(stderr, "aimee: could not enter session worktree %s: %s\n", worktree,
              strerror(errno));
      return 1;
   }

   launch_execvp(argv[0], argv);
   fprintf(stderr, "aimee: could not launch %s: %s\n", argv[0], strerror(errno));
   return 127;
}
