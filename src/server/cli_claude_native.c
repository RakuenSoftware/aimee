/* Credential-free Claude scheduler launch and actual SDK profile readback. */
#include "cli_claude_native.h"
#include "cJSON.h"
#include "aimee_sha256.h"
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
static const char *field(const cJSON *value, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, key));
}
static int native_identity_config(const cli_claude_host_launch_t *host)
{
   if (!host->sdk_identity || strlen(host->sdk_identity) != 64 ||
       strspn(host->sdk_identity, "0123456789abcdef") != 64)
      return -1;
   int home = open(host->home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   int directory =
       home >= 0 ? openat(home, ".claude", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
   struct stat root, dir, st, linked;
   int valid = home >= 0 && directory >= 0 && !fstat(home, &root) && !fstat(directory, &dir) &&
               root.st_uid == geteuid() && dir.st_uid == geteuid() &&
               (root.st_mode & 0777) == 0700 && (dir.st_mode & 0777) == 0700;
   int fd = valid
                ? openat(directory, ".claude.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
                : -1;
   valid = valid && fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
           (st.st_mode & 0777) == 0600 && st.st_nlink == 1 && st.st_size > 0 && st.st_size <= 16384;
   char bytes[16385];
   size_t used = 0;
   while (valid && used < sizeof bytes)
   {
      ssize_t n = read(fd, bytes + used, sizeof bytes - used);
      if (n < 0 && errno == EINTR)
         continue;
      if (n < 0)
         valid = 0;
      if (n <= 0)
         break;
      used += (size_t)n;
   }
   valid = valid && used == (size_t)st.st_size && used <= 16384 &&
           !fstatat(directory, ".claude.json", &linked, AT_SYMLINK_NOFOLLOW) &&
           linked.st_dev == st.st_dev && linked.st_ino == st.st_ino;
   cJSON *config = valid ? aimee_ir_json_parse_frame(bytes, used) : NULL;
   const char *identity = field(config, "userID");
   valid = cJSON_IsObject(config) && identity && !strcmp(identity, host->sdk_identity);
   cJSON_Delete(config);
   if (fd >= 0)
      close(fd);
   if (directory >= 0)
      close(directory);
   if (home >= 0)
      close(home);
   return valid ? 0 : -1;
}
const char *cli_claude_native_profile_error(const cli_claude_host_launch_t *host,
                                            const cJSON *frame)
{
   if (!host || !host->catalog)
      return "host catalog";
   if (!field(frame, "type") || strcmp(field(frame, "type"), "system") ||
       !field(frame, "subtype") || strcmp(field(frame, "subtype"), "init"))
      return "init frame";
   if (!field(frame, "cwd") || strcmp(field(frame, "cwd"), host->cwd))
      return "workspace";
   if (!field(frame, "apiKeySource") || strcmp(field(frame, "apiKeySource"), "ANTHROPIC_API_KEY"))
      return "API source";
   if (!field(frame, "permissionMode") || strcmp(field(frame, "permissionMode"), "default"))
      return "permission mode";
   const cJSON *tools = cJSON_GetObjectItemCaseSensitive(frame, "tools");
   const cJSON *servers = cJSON_GetObjectItemCaseSensitive(frame, "mcp_servers");
   const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(frame, "plugins");
   if (!cJSON_IsArray(tools) || cJSON_GetArraySize(tools) != host->catalog->n_tools)
      return "tool count";
   if (!cJSON_IsArray(servers) || cJSON_GetArraySize(servers) != (host->catalog->n_tools ? 1 : 0))
      return "MCP server count";
   if (!cJSON_IsArray(plugins) || cJSON_GetArraySize(plugins))
      return "plugins";
   const cJSON *name;
   cJSON_ArrayForEach(name, tools)
   {
      if (!cJSON_IsString(name))
         return "tool names";
      int matches = 0;
      for (int i = 0; i < host->catalog->n_tools; i++)
         matches += !strcmp(host->catalog->tools[i].name, name->valuestring);
      if (matches != 1)
         return "tool names";
      const cJSON *other;
      for (other = tools->child; other != name; other = other->next)
         if (cJSON_IsString(other) && !strcmp(other->valuestring, name->valuestring))
            return "tool names";
   }
   if (host->catalog->n_tools)
   {
      const cJSON *server = cJSON_GetArrayItem(servers, 0);
      if (!field(server, "name") || strcmp(field(server, "name"), "aimee") ||
          !field(server, "status") || strcmp(field(server, "status"), "connected"))
         return "MCP connection";
   }
   return NULL;
}
const char *cli_claude_mcp_status_name(cli_claude_mcp_status_t status)
{
   switch (status)
   {
   case CLI_CLAUDE_MCP_PENDING:
      return "pending";
   case CLI_CLAUDE_MCP_FAILED:
      return "failed";
   case CLI_CLAUDE_MCP_NEEDS_AUTH:
      return "needs-auth";
   case CLI_CLAUDE_MCP_DISABLED:
      return "disabled";
   case CLI_CLAUDE_MCP_CONNECTED:
      return "connected";
   default:
      return "unknown";
   }
}
static cli_claude_mcp_status_t native_mcp_status(const char *status)
{
   if (status)
      for (int i = CLI_CLAUDE_MCP_PENDING; i <= CLI_CLAUDE_MCP_CONNECTED; i++)
         if (!strcmp(status, cli_claude_mcp_status_name((cli_claude_mcp_status_t)i)))
            return (cli_claude_mcp_status_t)i;
   return CLI_CLAUDE_MCP_UNKNOWN;
}
void cli_claude_native_profile_summary(const cli_claude_host_launch_t *host, const cJSON *frame,
                                       cli_claude_profile_summary_t *summary)
{
   if (!summary)
      return;
   *summary = (cli_claude_profile_summary_t){
       .expected_tools = -1, .reported_tools = -1, .mcp_servers = -1, .plugins = -1};
   snprintf(summary->missing_owned_sha256, sizeof summary->missing_owned_sha256, "-");
   snprintf(summary->unexpected_reported_sha256, sizeof summary->unexpected_reported_sha256, "-");
   const cJSON *tools = cJSON_GetObjectItemCaseSensitive(frame, "tools");
   const cJSON *servers = cJSON_GetObjectItemCaseSensitive(frame, "mcp_servers");
   const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(frame, "plugins");
   if (cJSON_IsArray(tools))
      summary->reported_tools = cJSON_GetArraySize(tools);
   if (cJSON_IsArray(servers))
   {
      summary->mcp_servers = cJSON_GetArraySize(servers);
      const cJSON *server;
      int matches = 0;
      cJSON_ArrayForEach(server, servers)
      {
         if (!field(server, "name") || strcmp(field(server, "name"), "aimee"))
            continue;
         matches++;
         summary->aimee_status = native_mcp_status(field(server, "status"));
         if (field(server, "status") && !strcmp(field(server, "status"), "connected"))
            summary->aimee_connected = 1;
      }
      if (matches != 1)
         summary->aimee_status = CLI_CLAUDE_MCP_UNKNOWN;
   }
   if (cJSON_IsArray(plugins))
      summary->plugins = cJSON_GetArraySize(plugins);
   if (!host || !host->catalog || host->catalog->n_tools < 0 ||
       host->catalog->n_tools > AIMEE_IR_MAX_BLOCKS ||
       (host->catalog->n_tools && !host->catalog->tools))
      return;
   summary->expected_tools = host->catalog->n_tools;
   if (!cJSON_IsArray(tools) || summary->reported_tools > AIMEE_IR_MAX_BLOCKS)
      return;
   for (int i = 0; i < host->catalog->n_tools; i++)
   {
      const char *owned = host->catalog->tools[i].name;
      if (!owned)
         continue;
      int present = 0;
      const cJSON *tool;
      cJSON_ArrayForEach(tool, tools) present |=
          cJSON_IsString(tool) && !strcmp(tool->valuestring, owned);
      if (!present)
      {
         (void)aimee_sha256_hex(owned, strlen(owned), summary->missing_owned_sha256);
         break;
      }
   }
   const cJSON *tool;
   cJSON_ArrayForEach(tool, tools)
   {
      if (!cJSON_IsString(tool))
         continue;
      int matches = 0, duplicate = 0;
      for (int i = 0; i < host->catalog->n_tools; i++)
         matches += host->catalog->tools[i].name &&
                    !strcmp(tool->valuestring, host->catalog->tools[i].name);
      const cJSON *previous;
      for (previous = tools->child; previous != tool; previous = previous->next)
         duplicate |= cJSON_IsString(previous) && !strcmp(previous->valuestring, tool->valuestring);
      if (matches != 1 || duplicate)
      {
         (void)aimee_sha256_hex(tool->valuestring, strlen(tool->valuestring),
                                summary->unexpected_reported_sha256);
         break;
      }
   }
}
int cli_claude_native_verify(const cli_claude_host_launch_t *host, const cJSON *frame)
{
   return cli_claude_native_profile_error(host, frame) ? -1 : 0;
}
static char **native_arguments(const provider_cli_cfg_t *cfg)
{
   const cli_claude_host_launch_t *host = cfg ? cfg->claude_host : NULL;
   if (!host || !host->catalog || host->catalog->n_tools < 0 ||
       host->catalog->n_tools > AIMEE_IR_MAX_BLOCKS || !host->mcp_config || !cfg->agent)
      return NULL;
   size_t capacity = (size_t)host->catalog->n_tools + 40;
   char **args = calloc(capacity, sizeof *args);
   if (!args)
      return NULL;
   size_t n = 0;
#define ARG(value) args[n++] = (char *)(value)
   ARG("claude");
   ARG("--bare");
   ARG("--setting-sources");
   ARG("");
   ARG("--strict-mcp-config");
   ARG("--tools");
   ARG("");
   ARG("--mcp-config");
   ARG(host->mcp_config);
   ARG("--print");
   ARG("--output-format");
   ARG("stream-json");
   ARG("--verbose");
   ARG("--include-partial-messages");
   if (cfg->control_channel)
   {
      ARG("--input-format");
      ARG("stream-json");
      ARG("--permission-prompt-tool");
      ARG("stdio");
   }
   if (cfg->agent->model[0])
   {
      ARG("--model");
      ARG(cfg->agent->model);
   }
   if (cfg->resume_id && cfg->resume_id[0])
   {
      ARG("--resume");
      ARG(cfg->resume_id);
   }
   if (host->catalog->n_tools)
   {
      ARG("--allowedTools");
      for (int i = 0; i < host->catalog->n_tools; i++)
         ARG(host->catalog->tools[i].name);
   }
#undef ARG
   return args;
}

static cJSON *native_mcp_profile(const cli_claude_host_launch_t *host)
{
   cJSON *value = aimee_ir_json_parse_frame(host->mcp_config, strlen(host->mcp_config));
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(value, "mcpServers");
   if (!cJSON_IsObject(value) || cJSON_GetArraySize(value) != 1 || !cJSON_IsObject(servers) ||
       cJSON_GetArraySize(servers) != (host->catalog->n_tools ? 1 : 0))
      goto refused;
   if (host->catalog->n_tools)
   {
      cJSON *server = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
      cJSON *headers = cJSON_GetObjectItemCaseSensitive(server, "headers");
      char url[512], authorization[512];
      if (!cJSON_IsObject(server) || cJSON_GetArraySize(server) != 3 || !field(server, "type") ||
          strcmp(field(server, "type"), "http") || !cJSON_IsObject(headers) ||
          cJSON_GetArraySize(headers) != 1 ||
          snprintf(url, sizeof url, "%s/v1/mcp/call", host->gateway_url) >= (int)sizeof url ||
          snprintf(authorization, sizeof authorization, "Bearer %s", host->gateway_token) >=
              (int)sizeof authorization ||
          !field(server, "url") || strcmp(field(server, "url"), url) ||
          !field(headers, "Authorization") ||
          strcmp(field(headers, "Authorization"), authorization) ||
          !cJSON_SetValuestring(cJSON_GetObjectItem(server, "url"), "$OWNED_GATEWAY/v1/mcp/call") ||
          !cJSON_SetValuestring(cJSON_GetObjectItem(headers, "Authorization"), "$OWNED_CAPABILITY"))
         goto refused;
   }
   return value;
refused:
   cJSON_Delete(value);
   return NULL;
}
int cli_claude_native_launch_digest(const provider_cli_cfg_t *cfg, char digest[65])
{
   const cli_claude_host_launch_t *host = cfg ? cfg->claude_host : NULL;
   if (!host || !host->home || !host->gateway_url || !host->gateway_token || !host->environment ||
       !digest || native_identity_config(host))
      return -1;
   char **arguments = native_arguments(cfg);
   cJSON *plan = cJSON_CreateObject();
   cJSON *args = plan ? cJSON_AddArrayToObject(plan, "argv") : NULL;
   cJSON *environment = plan ? cJSON_AddObjectToObject(plan, "environment") : NULL;
   /* Claude keys saved conversations by scheduler cwd. Old ephemeral-cwd
    * handles cannot be adopted under the stable lease/epoch path contract. */
   if (!arguments || !args || !environment ||
       !cJSON_AddNumberToObject(plan, "scheduler_workspace_version", 1) ||
       !cJSON_AddNumberToObject(plan, "sdk_identity_version", 1) ||
       !cJSON_AddStringToObject(plan, "sdk_identity", host->sdk_identity))
      goto refused;
   for (int i = 0; arguments[i]; i++)
   {
      if (!strcmp(arguments[i], "--resume"))
      {
         if (!arguments[++i])
            goto refused;
         continue;
      }
      cJSON *entry = NULL;
      if (i && !strcmp(arguments[i - 1], "--mcp-config"))
         entry = native_mcp_profile(host);
      else
         entry = cJSON_CreateString(arguments[i]);
      if (!entry || !cJSON_AddItemToArray(args, entry))
      {
         cJSON_Delete(entry);
         goto refused;
      }
   }
   const char *names[] = {"HOME",
                          "CLAUDE_CONFIG_DIR",
                          "TMPDIR",
                          "PATH",
                          "LANG",
                          "OTEL_SDK_DISABLED",
                          "HTTP_PROXY",
                          "HTTPS_PROXY",
                          "NO_PROXY",
                          "ANTHROPIC_API_KEY",
                          "ANTHROPIC_BASE_URL",
                          "DISABLE_AUTOUPDATER",
                          "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC",
                          "CLAUDE_CODE_ATTRIBUTION_HEADER",
                          "MCP_CONNECTION_NONBLOCKING"};
   char config[4200], temporary[4200];
   if (snprintf(config, sizeof config, "%s/.claude", host->home) >= (int)sizeof config ||
       snprintf(temporary, sizeof temporary, "%s/tmp", host->home) >= (int)sizeof temporary)
      goto refused;
   const char *expected[] = {host->home,
                             config,
                             temporary,
                             "/usr/bin:/bin",
                             "C.UTF-8",
                             "true",
                             "http://127.0.0.1:1",
                             "http://127.0.0.1:1",
                             "127.0.0.1,localhost",
                             host->gateway_token,
                             host->gateway_url,
                             "1",
                             "1",
                             "0",
                             "0"};
   const char *normalized[] = {"$NATIVE_HOME",
                               "$NATIVE_HOME/.claude",
                               "$NATIVE_HOME/tmp",
                               "/usr/bin:/bin",
                               "C.UTF-8",
                               "true",
                               "http://127.0.0.1:1",
                               "http://127.0.0.1:1",
                               "127.0.0.1,localhost",
                               "$OWNED_CAPABILITY",
                               "$OWNED_GATEWAY",
                               "1",
                               "1",
                               "0",
                               "0"};
   unsigned seen = 0;
   for (int i = 0; host->environment[i]; i++)
   {
      if ((unsigned)i >= sizeof names / sizeof names[0])
         goto refused;
      const char *entry = host->environment[i], *equals = strchr(entry, '=');
      int found = 0;
      for (unsigned j = 0; equals && j < sizeof names / sizeof names[0]; j++)
         if ((size_t)(equals - entry) == strlen(names[j]) &&
             !memcmp(entry, names[j], strlen(names[j])))
         {
            if ((seen & (1u << j)) || strcmp(equals + 1, expected[j]) ||
                !cJSON_AddStringToObject(environment, names[j], normalized[j]))
               goto refused;
            seen |= 1u << j;
            found = 1;
            break;
         }
      if (!found)
         goto refused;
   }
   if (seen != ((1u << (sizeof names / sizeof names[0])) - 1))
      goto refused;
   cJSON *canonical = aimee_ir_json_canonical_copy(plan);
   char *wire = canonical ? cJSON_PrintUnformatted(canonical) : NULL;
   cJSON_Delete(canonical);
   int rc = wire ? aimee_sha256_hex(wire, strlen(wire), digest) : -1;
   free(wire);
   free(arguments);
   cJSON_Delete(plan);
   return rc;
refused:
   free(arguments);
   cJSON_Delete(plan);
   return -1;
}

int cli_claude_native_spawn(const provider_cli_cfg_t *cfg, int *input, int *output, pid_t *pid)
{
   const cli_claude_host_launch_t *host = cfg ? cfg->claude_host : NULL;
   if (!host || !input || !output || !pid || host->executable_fd < 0 || host->cwd_fd < 0 ||
       !host->environment || !host->catalog || !host->mcp_config || !cfg->agent)
      return -1;
   if (host->spawn_guard && (!host->spawn_guard->child || !host->spawn_guard->parent))
      return -1;
   char proof[65];
   if (!host->expected_profile || cli_claude_native_launch_digest(cfg, proof) ||
       strcmp(proof, host->expected_profile))
      return -1;
   char **args = native_arguments(cfg);
   if (!args)
      return -1;

   int in[2] = {-1, -1}, out[2] = {-1, -1};
   if (pipe(in) || pipe(out))
      goto failure;
   pid_t child = fork();
   if (child < 0)
      goto failure;
   if (!child)
   {
      if (setsid() < 0 || fchdir(host->cwd_fd) || dup2(in[0], STDIN_FILENO) < 0 ||
          dup2(out[1], STDOUT_FILENO) < 0 || dup2(out[1], STDERR_FILENO) < 0)
         _exit(126);
      close(in[0]);
      close(in[1]);
      close(out[0]);
      close(out[1]);
      long limit = sysconf(_SC_OPEN_MAX);
      if (limit < 0)
         _exit(126);
      for (int fd = 3; fd < limit; fd++)
         if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 && errno != EBADF)
            _exit(126);
      if (host->spawn_guard &&
          host->spawn_guard->child(host->spawn_guard->context, host->executable_fd, host->cwd_fd))
         _exit(126);
#if defined(__linux__)
      fexecve(host->executable_fd, args, host->environment);
#endif
      _exit(127);
   }
   if (host->spawn_guard && host->spawn_guard->parent(host->spawn_guard->context, child))
   {
      (void)kill(child, SIGKILL);
      while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
      {
      }
      goto failure;
   }
   free(args);
   close(in[0]);
   close(out[1]);
   *input = in[1];
   *output = out[0];
   *pid = child;
   return 0;
failure:
   for (int i = 0; i < 2; i++)
   {
      if (in[i] >= 0)
         close(in[i]);
      if (out[i] >= 0)
         close(out[i]);
   }
   free(args);
   return -1;
}
