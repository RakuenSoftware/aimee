/* ===================================================================
 * /v1 thin-client routing: route CLI subcommands through the server's native /v1 HTTP endpoints.
 * Unported commands fail in cli_main before reaching the server.
 * =================================================================== */

#include "cli_v1_routes_internal.h"
#include "platform_path.h"
#include "platform_random.h"
#include "cli_client.h"
#define V1_PROTOCOL_VERSION 1
#include "util.h"         /* safe_exec_capture (workspace.mirror-sync ships the client diff) */
#include "aimee_client.h" /* aimee_client_request: transport-agnostic /v1 client (Windows path) */
#include "code_collect.h" /* code_collect_files + code_collect_discover_repos (thin-client push) */
#if !defined(_WIN32) && !defined(_WIN64)
#include "aimee_home.h"
#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif /* !_WIN32 (preamble guard) */

/* config.deploy_env prints the env text and NOTHING else -- no banner, no quotes,
 * no trailing JSON. Its only caller is a shell wrapper, `eval "$(aimee config
 * deploy-env)"`, so any decoration becomes a shell command. The generic JSON
 * fallback would emit the whole envelope, which eval would then try to run. */
static void pt_print_deploy_env(const char *method, cJSON *resp)
{
   (void)method;
   cJSON *env = cJSON_GetObjectItemCaseSensitive(resp, "env");
   if (cJSON_IsString(env) && env->valuestring)
      fputs(env->valuestring, stdout);
}

typedef void (*pt_print_fn)(const char *method, cJSON *resp);
static const struct
{
   const char *method;
   pt_print_fn fn;
} pt_print_table[] = {
    {"config.deploy_env", pt_print_deploy_env},
    {"roundtable.review", pt_print_roundtable_review},
    {"audit.verify", pt_print_audit},
    {"audit.checkpoint", pt_print_audit},
    {"audit.seal", pt_print_audit},
    {"audit.snapshot", pt_print_audit},
    {"init.run", pt_print_init_run},
    {"rules.generate", pt_print_rules_generate},
    {"kb.grant.set", pt_print_grant_set},
    {"kb.grant.revoke", pt_print_grant_revoke},
    /* show is list filtered to one subject and shares its row shape, so it shares
     * the printer too; they are distinct methods only so the marshaller can require
     * show's --subject. */
    {"kb.grant.list", pt_print_grant_list},
    {"kb.grant.show", pt_print_grant_list},
    {"skill.list", pt_print_skill_list},
    {"skill.show", pt_print_skill_show},
    {"git.verify", pt_print_git_verify},
    /* Every git command returns MCP content blocks, the same shape verify does. */
    {"git.cli", pt_print_git_verify},
    {"get_help", pt_print_get_help},
    {"server.health", pt_print_server_health},
    {"session.list", pt_print_session_list},
    {"session.get", pt_print_session_get},
    {"session.close", pt_print_session_close},
    {"session.brief", pt_print_session_brief},
    {"session.attach", pt_print_session_attach},
    {"session.detach", pt_print_session_detach},
    {"session.presence", pt_print_session_presence},
    {"trajectory.export", pt_print_trajectory_export},
    {"trajectory.batch", pt_print_trajectory_batch},
    {"insights.overview", pt_print_insights_overview},
    {"worktree.gc", pt_print_worktree_gc},
    {"rules.delete", pt_print_rules_delete},
    {"memory.search", pt_print_memory_search},
    {"memory.store", pt_print_memory_store},
    {"memory.list", pt_print_memory_list},
    {"memory.get", pt_print_memory_get},
    {"memory.read", pt_print_memory_read},
    {"memory.stats", pt_print_memory_stats},
    {"index.scan", pt_print_index_scan},
    {"index.list", pt_print_index_list},
    {"index.find", pt_print_index_find},
    {"index.blast_radius", pt_print_index_blast_radius},
    {"index.structure", pt_print_index_structure},
    {"index.span", pt_print_index_span},
    {"index.investigate", pt_print_index_investigate},
    {"index.hybrid", pt_print_index_hybrid},
    {"index.find_callers", pt_print_index_find_callers},
    {"index.deps", pt_print_index_deps},
    {"graph.sync_code", pt_print_graph_sync_code},
    {"workspace.add", pt_print_workspace_add},
    {"workspace.list", pt_print_workspace_list},
    {"workspace.get", pt_print_workspace_get},
    {"workspace.remove", pt_print_workspace_remove},
    {"workspace.mirror-sync", pt_print_workspace_mirror_sync},
    {"hud.status", pt_print_hud_status},
    {"model.list", pt_print_agent_list},
    {"model.local", pt_print_agent_local},
    {"model.add", pt_print_agent_add},
    {"model.remove", pt_print_agent_remove},
    {"model.enable", pt_print_agent_enable},
    {"model.roles", pt_print_agent_roles},
    {"model.personas", pt_print_agent_personas},
    {"model.disable", pt_print_agent_disable},
    {"model.probe", pt_print_agent_probe},
    {"mcp.audit", pt_print_mcp_audit},
    {"mcp.recheck", pt_print_mcp_recheck},
    {"toolset.list", pt_print_toolset_list},
    {"toolset.show", pt_print_toolset_show},
    {"toolset.resolve", pt_print_toolset_resolve},
    {"trigger.list", pt_print_trigger_list},
    {"trigger.status", pt_print_trigger_status},
    {"trigger.fire", pt_print_trigger_fire},
    {"trigger.cancel", pt_print_trigger_cancel},
    {"cron.list", pt_print_cron_list},
    {"cron.add", pt_print_cron_add},
    {"cron.enable", pt_print_cron_add},
    {"cron.disable", pt_print_cron_add},
    {"cron.remove", pt_print_cron_add},
    {"cron.show", pt_print_cron_show},
    {"cron.history", pt_print_cron_history},
    {"cron.run", pt_print_cron_run},
    {"wm.get", pt_print_wm_get},
    {"wm.set", pt_print_wm_set},
    {"wm.list", pt_print_wm_list},
    {"delegate", pt_print_delegate},
    {"delegate.status", pt_print_delegate_status},
    {"jobs.list", pt_print_jobs_list},
    {"jobs.status", pt_print_jobs_status},
    {"jobs.logs", pt_print_jobs_logs},
    {"jobs.cancel", pt_print_jobs_cancel},
    {"job.start", pt_print_job_start},
    {"job.list", pt_print_job_list},
    {"job.status", pt_print_job_status},
    {"job.cancel", pt_print_job_cancel},
    {"aux.config_show", pt_print_aux_config_show},
    {"config.show", pt_print_config_show},
    {"config.get", pt_print_config_get},
    {"config.set", pt_print_config_set},
    {"aux.test", pt_print_aux_test},
    {"delegate.log", pt_print_delegate_log},
    {"episode.list", pt_print_delegate_log},
    {"model.episodes", pt_print_delegate_log},
    {"delegate.launch", pt_print_delegate_launch},
    {"kb.search", pt_print_kb_search},
    {"kb.build", pt_print_kb_build},
    {"kb.update", pt_print_kb_build},
    {"kb.ingest", pt_print_kb_ingest},
    {"kb.docs.push", pt_print_kb_docs_push},
    {"kb.ingest.status", pt_print_kb_ingest_status},
    {"kb.health", pt_print_kb_status},
    {"kb.status", pt_print_kb_status},
    {"workers", pt_print_workers},
    {"provider.list", pt_print_provider_list},
    {"provider.show", pt_print_provider_show},
    {"provider.models", pt_print_provider_models},
    {"provider.test", pt_print_provider_test},
    {"provider.quota", pt_print_provider_quota},
    {"catalog.show", pt_print_model_show},
    {"catalog.list", pt_print_model_list},
    {"provider.get", pt_print_provider_get},
    {"provider.set", pt_print_provider_set},
    {"catalog.refresh", pt_print_model_refresh},
    {"dogfood.tag", pt_print_dogfood_tag},
    {"dogfood.report", pt_print_dogfood_report},
    {"eval.run", pt_print_eval_run},
    {"eval.results", pt_print_eval_results},
    {"identity.show", pt_print_identity_show},
    {"api.status", pt_print_api_status},
    {"api.enable", pt_print_api_status},
    {"api.disable", pt_print_api_status},
    {"primary.set", pt_print_primary_set},
    {"identity.snapshot", pt_print_identity_snapshot},
    {"identity.diff", pt_print_identity_diff},
};

static void print_text_output(const char *method, cJSON *resp)
{
   if (!resp)
      return;
   for (size_t i = 0; i < sizeof(pt_print_table) / sizeof(pt_print_table[0]); i++)
      if (strcmp(method, pt_print_table[i].method) == 0)
      {
         pt_print_table[i].fn(method, resp);
         return;
      }
   if (strncmp(method, "skill.", 6) == 0)
   {
      pt_print_skill_group(method, resp);
      return;
   }

   /* NO PRINTER: SHOW THE PAYLOAD, DO NOT SHOW NOTHING.
    *
    * 50 of the 181 dispatchable methods reach here. Falling through in silence made
    * each of them exit 0 having printed nothing at all, which reads as "it worked and
    * there was nothing to say" and is indistinguishable from it. `aimee vault list`
    * returned 348 bytes of vault entries over /v1 and printed zero of them; `aimee kb
    * curator status` and `aimee economizer stats` did the same the moment their
    * marshallers were added.
    *
    * Printing the JSON is never worse than printing nothing. It cannot break a script
    * that parses this output either, because there was no output to parse -- the only
    * thing a caller could have depended on is emptiness, and emptiness was the bug.
    * A method that deserves prose gets an entry in pt_print_table; this is the floor,
    * not the target. */
   char *raw = cJSON_PrintUnformatted(resp);
   if (raw)
   {
      puts(raw);
      free(raw);
   }
}

static const char *delegate_output_path_from_args(int argc, char **argv)
{
   static const char *bool_flags[] = {"json",         "background",   "durable",
                                      "coordination", "plan",         "dry-run",
                                      "tools",        "handoff-json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   return rpc_get(&opts, "output");
}

static int delegate_timeout_from_args(int argc, char **argv)
{
   static const char *bool_flags[] = {"json",         "background",   "durable",
                                      "coordination", "plan",         "dry-run",
                                      "tools",        "handoff-json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   return rpc_get_int(&opts, "timeout", 0);
}

/* `agent probe` is a diagnostic command, so its process status must agree with
 * the result it prints.  A 2xx response only means the server completed the
 * probe; it does not mean the provider was usable.  Prefer the execution probe
 * when it ran because some hosted providers reject /models while accepting
 * inference.  With --no-run, model availability is the strongest result we
 * have. */
static int agent_probe_response_is_failure(cJSON *resp)
{
   cJSON *execution_ok = cJSON_GetObjectItemCaseSensitive(resp, "execution_ok");
   if (execution_ok)
      return !cJSON_IsTrue(execution_ok);

   cJSON *model_available = cJSON_GetObjectItemCaseSensitive(resp, "model_available");
   return model_available && !cJSON_IsTrue(model_available);
}

static int write_delegate_output_file(const char *path, const char *text)
{
   if (!path || !path[0] || !text)
      return -1;

   char parent[4096];
   snprintf(parent, sizeof(parent), "%s", path);
   char *slash = strrchr(parent, '/');
   if (slash && slash != parent)
   {
      *slash = '\0';
      if (platform_mkdir_p(parent, 0755) != 0)
         return -1;
   }

   FILE *f = fopen(path, "w");
   if (!f)
      return -1;
   fputs(text, f);
   fputc('\n', f);
   fclose(f);
   return 0;
}

#if !defined(_WIN32) && !defined(_WIN64)
/* cli_v1_http_request: minimal HTTP/1.1 request (any verb) over aimee-server's
 * /v1 Unix socket (<aimee_home>/aimee-http.sock). Kept local to the thin client
 * so the api.client_transport cutover adds no new link dependency
 * (http_uds_client.c is not linked into the CLI). Returns the response body
 * (heap; caller frees) and sets *status_out to the HTTP status; NULL on
 * transport failure. Mirrors http_uds_client.c, which serves the same role for
 * the TUI.
 *
 * timeout_ms bounds the WAIT FOR A REPLY. It used not to take one at all: the
 * caller's budget was honoured on the remote branch of cli_v1_send and silently
 * dropped here, and the read loop below had no timeout of any kind. A
 * co-located server that accepted the connection and then went quiet -- or died
 * between accept and reply -- hung the CLI forever, with no way for the caller
 * to bound it. That is the default transport for a co-located install. */
static char *cli_v1_http_request(const char *verb, const char *path, const char *body,
                                 int timeout_ms, int *status_out)
{
   if (status_out)
      *status_out = 0;
   if (timeout_ms <= 0)
      timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;
   const char *home = aimee_home();
   if (!home || !home[0])
      return NULL;

   char sock_path[512];
   snprintf(sock_path, sizeof(sock_path), "%s/aimee-http.sock", home);

   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return NULL;
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
   if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
   {
      close(fd);
      return NULL;
   }

   int blen = body ? (int)strlen(body) : 0;
   char head[512];
   int hlen = snprintf(head, sizeof(head),
                       "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                       "Content-Length: %d\r\nConnection: close\r\n\r\n",
                       verb, path, blen);
   if (hlen <= 0 || hlen >= (int)sizeof(head))
   {
      close(fd);
      return NULL;
   }

   int off = 0;
   while (off < hlen)
   {
      int n = (int)write(fd, head + off, (size_t)(hlen - off));
      if (n <= 0)
      {
         close(fd);
         return NULL;
      }
      off += n;
   }
   off = 0;
   while (off < blen)
   {
      int n = (int)write(fd, body + off, (size_t)(blen - off));
      if (n <= 0)
      {
         close(fd);
         return NULL;
      }
      off += n;
   }

   const long long deadline_ms = util_now_ms() + timeout_ms;
   size_t cap = 8192, len = 0;
   char *resp = malloc(cap);
   if (!resp)
   {
      close(fd);
      return NULL;
   }
   for (;;)
   {
      if (len + 4096 > cap)
      {
         cap *= 2;
         char *grown = realloc(resp, cap);
         if (!grown)
         {
            free(resp);
            close(fd);
            return NULL;
         }
         resp = grown;
      }
      /* Wait for readability against the caller's deadline rather than
       * blocking in read(). A server that accepts and never answers must cost
       * the budget, not the session. */
      long long left = deadline_ms - util_now_ms();
      if (left <= 0)
      {
         free(resp);
         close(fd);
         return NULL;
      }
      struct pollfd readable = {.fd = fd, .events = POLLIN};
      int pr = poll(&readable, 1, (int)left);
      if (pr <= 0 || !(readable.revents & POLLIN))
      {
         free(resp);
         close(fd);
         return NULL;
      }
      int n = (int)read(fd, resp + len, 4096);
      if (n <= 0)
         break;
      len += (size_t)n;
   }
   close(fd);
   resp[len] = '\0';

   int status = 0;
   if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1)
   {
      free(resp);
      return NULL;
   }
   if (status_out)
      *status_out = status;
   char *bstart = strstr(resp, "\r\n\r\n");
   char *out = bstart ? strdup(bstart + 4) : strdup("");
   free(resp);
   return out;
}

/* The thin client is a strict /v1 consumer: there is no socket/auto transport
 * selection any more (the legacy NDJSON transport was removed). One-shot
 * commands go over the local aimee-http.sock, or a configured remote /v1
 * endpoint (cli_v1_client_endpoint) — see cli_v1_forward below. */
#endif /* !_WIN32 — the aimee-http.sock helpers above are POSIX-only */

/* The remote-endpoint resolvers below are portable (env + aimee.yaml, no UDS)
 * and MUST compile on Windows too: cli_v1_forward calls them unconditionally
 * and the Windows thin client always takes the remote /v1 path. */

/* cli_v1_aimee_yaml_value: scan <aimee_home>/aimee.yaml for the first
 * (non-comment) line containing <key> and return its malloc'd, unquoted value,
 * or NULL when absent/empty. The full config_t parser is not linked into the
 * thin client, so the api.client_* settings are read with this lightweight
 * scan. Caller frees. */
static char *cli_v1_aimee_yaml_value(const char *key)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return NULL;
   char path[512];
   snprintf(path, sizeof(path), "%s/aimee.yaml", home);
   FILE *fp = fopen(path, "r");
   if (!fp)
      return NULL;

   char *result = NULL;
   char line[512];
   while (fgets(line, sizeof(line), fp))
   {
      const char *p = line;
      while (*p == ' ' || *p == '\t')
         p++;
      if (*p == '#')
         continue; /* comment line */
      char *k = strstr(line, key);
      if (!k)
         continue;
      k += strlen(key);
      while (*k == ' ' || *k == '\t')
         k++;
      char val[256];
      int i = 0;
      while (*k && *k != '\n' && *k != '\r' && *k != '#' && i < (int)sizeof(val) - 1)
      {
         if (*k != '"' && *k != '\'')
            val[i++] = *k;
         k++;
      }
      while (i > 0 && (val[i - 1] == ' ' || val[i - 1] == '\t'))
         i--;
      val[i] = '\0';
      if (val[0])
         result = strdup(val);
      break;
   }
   fclose(fp);
   return result;
}

/* cli_v1_client_endpoint: a remote aimee-server /v1 endpoint for the HTTP
 * transport — "tcp:host:port" (or an explicit "unix:/path"). AIMEE_API_ENDPOINT
 * overrides; otherwise aimee.api.client_endpoint in aimee.yaml. NULL means no
 * remote is configured and the caller falls back to the local aimee-http.sock.
 * This is the only client path that reaches an aimee-server on another host
 * (e.g. a container's published port); the aimee-http.sock helpers above are
 * loopback-only. Caller frees. */
/* Normalize a configured endpoint to a cli_http transport scheme. An https://
 * URL becomes "tls:host:port" (native client TLS to the server's #304 TLS
 * listener); http:// becomes "tcp:host:port"; an explicit tcp:/tls:/unix:
 * endpoint passes through unchanged. Caller frees. */
static char *cli_v1_normalize_endpoint(const char *ep)
{
   if (!ep || !ep[0])
      return NULL;
   const char *scheme = NULL;
   const char *rest = NULL;
   if (strncmp(ep, "https://", 8) == 0)
   {
      scheme = "tls:";
      rest = ep + 8;
   }
   else if (strncmp(ep, "http://", 7) == 0)
   {
      scheme = "tcp:";
      rest = ep + 7;
   }
   if (!scheme)
      return strdup(ep); /* tcp:/tls:/unix:/host:port already */
   /* Take host[:port], dropping any trailing path; default the port by scheme. */
   char hostport[300];
   size_t n = strcspn(rest, "/");
   if (n >= sizeof(hostport))
      n = sizeof(hostport) - 1;
   memcpy(hostport, rest, n);
   hostport[n] = '\0';
   /* Strip any userinfo ("user:pass@host") so it can't corrupt the host:port the
    * TLS/connect layer parses; credentials belong in the bearer, not the URL. */
   char *at = strrchr(hostport, '@');
   char *hp = at ? at + 1 : hostport;
   char out[320];
   if (strchr(hp, ':'))
      snprintf(out, sizeof(out), "%s%s", scheme, hp);
   else
      snprintf(out, sizeof(out), "%s%s:%s", scheme, hp, strcmp(scheme, "tls:") == 0 ? "443" : "80");
   return strdup(out);
}

char *cli_v1_client_endpoint(void)
{
   const char *env = getenv("AIMEE_API_ENDPOINT");
   if (env && env[0])
      return cli_v1_normalize_endpoint(env);
   char *yaml = cli_v1_aimee_yaml_value("client_endpoint:");
   if (yaml)
   {
      char *norm = cli_v1_normalize_endpoint(yaml);
      free(yaml);
      return norm;
   }
   /* Fall back to a --server / AIMEE_SERVER_URL / remote.conf target (aimee_client)
    * by synthesizing the transport endpoint cli_http_request expects: "tls:" for
    * an https:// target (native client TLS, #304), else "tcp:". This makes the
    * README's headline remote flow drive the WHOLE data/control plane (status,
    * memory, kb, …), not just the lower-level AIMEE_API_ENDPOINT form. */
   char hostport[300];
   int is_https = 0;
   if (aimee_client_remote_active_scheme(hostport, sizeof(hostport), &is_https))
   {
      char ep[320];
      snprintf(ep, sizeof(ep), "%s%s", is_https ? "tls:" : "tcp:", hostport);
      return strdup(ep);
   }
   return NULL;
}

/* cli_v1_client_bearer: bearer token sent with the remote HTTP transport.
 * AIMEE_API_BEARER overrides; otherwise the `bearer_token:` key in aimee.yaml
 * (the same value aimee-server reads for aimee.api.bearer_token). NULL means no
 * Authorization header. Caller frees. */
char *cli_v1_client_bearer(void)
{
   const char *env = getenv("AIMEE_API_BEARER");
   if (env && env[0])
      return strdup(env);
   char *yaml = cli_v1_aimee_yaml_value("bearer_token:");
   if (yaml)
      return yaml;
   /* Token for a synthesized aimee_client endpoint: --server-token /
    * AIMEE_SERVER_TOKEN / remote.conf line 2. */
   char tok[300];
   if (aimee_client_remote_token(tok, sizeof(tok)) && tok[0])
      return strdup(tok);
   return NULL;
}

/* cli_v1_has_remote_endpoint: true when the thin client is configured to reach
 * a remote aimee-server over /v1 (client_transport != socket AND an endpoint is
 * set). The dispatcher uses this to skip the local-socket preflight, which would
 * otherwise fail with "server unavailable" because there is no aimee.sock to
 * find. Non-static so cli_main.c (a separate TU) can call it; mirrors the
 * linkage of cli_v1_forward below. */
int cli_v1_has_remote_endpoint(void)
{
#if !defined(_WIN32) && !defined(_WIN64)
   /* cli_v1_client_endpoint() also synthesizes a tcp: endpoint from a --server /
    * AIMEE_SERVER_URL / remote.conf target (aimee_client), so this transparently
    * reports those too and the dispatcher skips the dead local-socket preflight. */
   char *ep = cli_v1_client_endpoint();
   if (ep)
   {
      free(ep);
      return 1;
   }
   return 0;
#else
   /* Windows has no UDS path and no AIMEE_API_ENDPOINT/aimee.yaml config: its
    * remote target comes from aimee_client (AIMEE_SERVER_URL or --server). Report
    * that so the dispatcher skips the (always-failing) local-socket preflight and
    * lets cli_v1_forward route over the remote /v1 via aimee_client_request. */
   return aimee_client_has_remote();
#endif
}

/* cli_v1_remote_endpoint_is_network: like cli_v1_has_remote_endpoint, but true
 * only for a genuinely remote network endpoint — "tcp:host:port" (http) OR
 * "tls:host:port" (https) — never a local "unix:" one. Two callers rely on it:
 * interactive/co-located commands (chat, launch) refuse a remote server cleanly
 * (the agent + tools + worktree run on this host); and the thin-client
 * workspace/index push uses it to detect that the server cannot read this host's
 * filesystem. A TLS remote is no less remote than a plaintext one — matching
 * only "tcp:" here silently broke client-side push once remotes went TLS-only.
 * Non-static so cli_main.c can call it. */
int cli_v1_remote_endpoint_is_network(void)
{
#if !defined(_WIN32) && !defined(_WIN64)
   /* A synthesized aimee_client target (--server / AIMEE_SERVER_URL / remote.conf)
    * is "tcp:host:port" (http) or "tls:host:port" (https) — a network host, never
    * a local unix: path. Either scheme means the server cannot see this host. */
   char *ep = cli_v1_client_endpoint();
   int is_net = ep && (strncmp(ep, "tcp:", 4) == 0 || strncmp(ep, "tls:", 4) == 0);
   free(ep);
   return is_net;
#else
   /* The Windows thin client's only remote target comes from aimee_client
    * (AIMEE_SERVER_URL / --server), which is always a tcp:host:port network
    * endpoint — never a local unix: socket. Report it so chat/launch start the
    * reverse-channel and route the turn over /v1 (matching has_remote_endpoint). */
   return aimee_client_has_remote();
#endif
}

/* The complete method -> first-class /v1 route map, generated from the server
 * registry (every synchronous rh_dispatch_op route). DO NOT EDIT — see
 * scripts/gen-cli-v1-routes.py; scripts/check-cli-v1-routes.py guards drift. */
/* @@GEN-CLI-V1-ROUTES BEGIN — generated by scripts/gen-cli-v1-routes.py; DO NOT EDIT @@ */
/* Auto-generated from src/server/server_http_routes.c — DO NOT EDIT.
 *
 * method (op) -> first-class /v1 route for the thin client.
 *  - CLI_V1_GEN_ROUTES   : synchronous rh_dispatch_op routes; the HTTP
 *                          response is byte-identical to the NDJSON socket.
 *  - CLI_V1_ASYNC_ROUTES : rh_dispatch_op_async routes; POSTing returns a
 *                          run handle ({id}) the client polls at
 *                          GET /v1/runs/{id} until it completes.
 * Regenerate after changing the registry; scripts/check-cli-v1-routes.py
 * (make lint) guards against drift. */
static const struct
{
   const char *method;
   const char *verb;
   const char *path;
} CLI_V1_GEN_ROUTES[] = {
    {"api.disable", "POST", "/v1/api/disable"},
    {"api.enable", "POST", "/v1/api/enable"},
    {"api.enroll_bearer", "POST", "/v1/api/enroll_bearer"},
    {"api.rotate_bearer", "POST", "/v1/api/rotate_bearer"},
    {"api.status", "GET", "/v1/api/status"},
    {"attempt.list", "POST", "/v1/attempts/list"},
    {"attempt.record", "POST", "/v1/attempts/record"},
    {"audit.captures", "GET", "/v1/audit/captures"},
    {"audit.checkpoint", "POST", "/v1/audit/checkpoint"},
    {"audit.replay", "POST", "/v1/audit/replay"},
    {"audit.seal", "POST", "/v1/audit/seal"},
    {"audit.snapshot", "POST", "/v1/audit/snapshot"},
    {"audit.verify", "GET", "/v1/audit/verify"},
    {"aux.config_show", "GET", "/v1/aux/config"},
    {"aux.test", "POST", "/v1/aux/test"},
    {"blast_radius.preview", "POST", "/v1/blast_radius/preview"},
    {"calibration.readiness", "GET", "/v1/calibration/readiness"},
    {"catalog.list", "GET", "/v1/catalog/list"},
    {"catalog.refresh", "POST", "/v1/catalog/refresh"},
    {"catalog.show", "GET", "/v1/catalog/show"},
    {"cert.issue", "POST", "/v1/cert/issue"},
    {"cert.list", "POST", "/v1/cert/list"},
    {"cert.revoke", "POST", "/v1/cert/revoke"},
    {"cert.sign", "POST", "/v1/cert/sign"},
    {"chat.interrupt", "POST", "/v1/chat/interrupt"},
    {"code.audit", "POST", "/v1/code/audit"},
    {"collab_rules.approve", "POST", "/v1/collab_rules/approve"},
    {"collab_rules.list", "GET", "/v1/collab_rules"},
    {"collab_rules.list_active", "GET", "/v1/collab_rules/active"},
    {"collab_rules.reject", "POST", "/v1/collab_rules/reject"},
    {"collab_rules.retire", "POST", "/v1/collab_rules/retire"},
    {"config.deploy_env", "GET", "/v1/config/deploy-env"},
    {"config.get", "POST", "/v1/config/get"},
    {"config.set", "POST", "/v1/config/set"},
    {"config.show", "GET", "/v1/config"},
    {"cron.add", "POST", "/v1/cron/add"},
    {"cron.disable", "POST", "/v1/cron/disable"},
    {"cron.enable", "POST", "/v1/cron/enable"},
    {"cron.history", "POST", "/v1/cron/history"},
    {"cron.list", "GET", "/v1/cron"},
    {"cron.remove", "POST", "/v1/cron/remove"},
    {"cron.run", "POST", "/v1/cron/run"},
    {"cron.show", "POST", "/v1/cron/show"},
    {"css.signals", "POST", "/v1/css/signals"},
    {"curator.contradictions", "POST", "/v1/curator/contradictions"},
    {"curator.implements", "POST", "/v1/curator/implements"},
    {"curator.invalidated", "POST", "/v1/curator/invalidated"},
    {"curator.stages", "POST", "/v1/curator/stages"},
    {"dashboard.all", "GET", "/v1/dashboard/all"},
    {"dashboard.audit", "GET", "/v1/dashboard/audit"},
    {"dashboard.delegations", "GET", "/v1/dashboard/delegations"},
    {"dashboard.logs", "GET", "/v1/dashboard/logs"},
    {"dashboard.memory_stats", "GET", "/v1/dashboard/memory_stats"},
    {"dashboard.metrics", "GET", "/v1/dashboard/metrics"},
    {"dashboard.onboard", "GET", "/v1/dashboard/onboard"},
    {"dashboard.plans", "GET", "/v1/dashboard/plans"},
    {"dashboard.traces", "GET", "/v1/dashboard/traces"},
    {"delegate", "POST", "/v1/delegate/run"},
    {"delegate.backend_exec", "POST", "/v1/delegate/backend_exec"},
    {"delegate.backend_list", "GET", "/v1/delegate/backend_list"},
    {"delegate.cancel_unassigned", "POST", "/v1/delegate/cancel_unassigned"},
    {"delegate.launch", "POST", "/v1/delegate/launch"},
    {"delegate.log", "GET", "/v1/delegate/log"},
    {"delegate.reply", "POST", "/v1/delegate/reply"},
    {"delegate.reservation.forget", "POST", "/v1/delegate/reservation/forget"},
    {"delegate.sandbox_gc", "POST", "/v1/delegate/sandbox/gc"},
    {"delegate.sandbox_list", "GET", "/v1/delegate/sandbox/images"},
    {"delegate.status", "POST", "/v1/delegate/status"},
    {"demotion.check", "GET", "/v1/demotion/check"},
    {"dogfood.report", "GET", "/v1/dogfood/report"},
    {"dogfood.review", "POST", "/v1/dogfood/review"},
    {"dogfood.tag", "POST", "/v1/dogfood/tag"},
    {"economizer.stats", "GET", "/v1/economizer/stats"},
    {"embedders.list", "GET", "/v1/embedders"},
    {"episode.list", "GET", "/v1/episode/list"},
    {"eval.results", "GET", "/v1/eval/results"},
    {"evidence.fidelity_retrieval_event", "POST", "/v1/audit/fidelity"},
    {"evidence.provenance_retrieval_event", "POST", "/v1/audit/provenance"},
    {"evidence.trace_retrieval_event", "POST", "/v1/audit/trace"},
    {"graph.explain", "POST", "/v1/graph/explain"},
    {"help.get", "POST", "/v1/help"},
    {"hooks.post", "POST", "/v1/hooks/post"},
    {"hooks.pre", "POST", "/v1/hooks/pre"},
    {"hooks.session_start", "POST", "/v1/hooks/session_start"},
    {"hosts.list", "GET", "/v1/hosts"},
    {"hud.status", "GET", "/v1/hud"},
    {"identity.diff", "POST", "/v1/identity/diff"},
    {"identity.show", "GET", "/v1/identity/show"},
    {"identity.snapshot", "POST", "/v1/identity/snapshot"},
    {"index.blast_radius", "POST", "/v1/index/blast_radius"},
    {"index.deps", "POST", "/v1/index/deps"},
    {"index.find", "POST", "/v1/index/find"},
    {"index.find_callers", "POST", "/v1/index/find_callers"},
    {"index.hybrid", "POST", "/v1/index/hybrid"},
    {"index.investigate", "POST", "/v1/index/investigate"},
    {"index.list", "POST", "/v1/index/list"},
    {"index.span", "POST", "/v1/index/span"},
    {"index.structure", "POST", "/v1/index/structure"},
    {"init.run", "POST", "/v1/init/run"},
    {"insights.overview", "GET", "/v1/insights/overview"},
    {"job.cancel", "POST", "/v1/job/cancel"},
    {"job.list", "GET", "/v1/job/list"},
    {"job.start", "POST", "/v1/job/start"},
    {"job.status", "POST", "/v1/job/status"},
    {"jobs.cancel", "POST", "/v1/jobs/cancel"},
    {"jobs.list", "GET", "/v1/jobs/list"},
    {"jobs.logs", "POST", "/v1/jobs/logs"},
    {"jobs.status", "POST", "/v1/jobs/status"},
    {"launch.run", "POST", "/v1/launch/run"},
    {"lsp.diagnostics_summary", "POST", "/v1/lsp/diagnostics_summary"},
    {"mcp.audit", "POST", "/v1/mcp/audit"},
    {"mcp.call", "POST", "/v1/mcp/call"},
    {"mcp.recheck", "POST", "/v1/mcp/recheck"},
    {"mcp.tools_list", "GET", "/v1/mcp/tools_list"},
    {"memory.delete", "POST", "/v1/memory/delete"},
    {"memory.get", "POST", "/v1/memory/get"},
    {"memory.list", "POST", "/v1/memory/list"},
    {"memory.read", "GET", "/v1/memory/read"},
    {"memory.search", "POST", "/v1/memory/search"},
    {"memory.stats", "GET", "/v1/memory/stats"},
    {"memory.store", "POST", "/v1/memory/store"},
    {"memory.supersede", "POST", "/v1/memory/supersede"},
    {"memory.user_capture", "POST", "/v1/memory/user_capture"},
    {"model.add", "POST", "/v1/model/add"},
    {"model.cli_oauth_code", "POST", "/v1/model/cli_oauth_code"},
    {"model.cli_oauth_poll", "POST", "/v1/model/cli_oauth_poll"},
    {"model.cli_oauth_start", "POST", "/v1/model/cli_oauth_start"},
    {"model.disable", "POST", "/v1/model/disable"},
    {"model.draft", "POST", "/v1/model/draft"},
    {"model.enable", "POST", "/v1/model/enable"},
    {"model.episodes", "POST", "/v1/model/episodes"},
    {"model.list", "GET", "/v1/model/list"},
    {"model.local", "GET", "/v1/model/local"},
    {"model.personas", "POST", "/v1/model/personas"},
    {"model.probe", "POST", "/v1/model/probe"},
    {"model.remove", "POST", "/v1/model/remove"},
    {"model.roles", "POST", "/v1/model/roles"},
    {"model.set", "POST", "/v1/model/set"},
    {"model.setup", "POST", "/v1/model/setup"},
    {"model.setup_poll", "POST", "/v1/model/setup_poll"},
    {"model.stats", "GET", "/v1/model/stats"},
    {"optimize.export", "GET", "/v1/optimize/export"},
    {"optimize.promote", "POST", "/v1/optimize/promote"},
    {"optimize.replay_record", "POST", "/v1/optimize/replay-record"},
#if AIMEE_WITH_ROUNDTABLE
    {"pipeline.advance", "POST", "/v1/pipeline/advance"},
    {"pipeline.cancel", "POST", "/v1/pipeline/cancel"},
    {"pipeline.gate", "POST", "/v1/pipeline/gate"},
    {"pipeline.list", "GET", "/v1/pipeline/list"},
    {"pipeline.resume", "POST", "/v1/pipeline/resume"},
    {"pipeline.start", "POST", "/v1/pipeline/start"},
    {"pipeline.status", "POST", "/v1/pipeline/status"},
#endif
    {"provider.get", "POST", "/v1/provider/get"},
    {"provider.list", "GET", "/v1/provider/list"},
    {"provider.models", "GET", "/v1/provider/models"},
    {"provider.quota", "POST", "/v1/provider/quota"},
    {"provider.set", "POST", "/v1/provider/set"},
    {"provider.show", "POST", "/v1/provider/show"},
    {"provider.test", "POST", "/v1/provider/test"},
    {"ranker.export_view", "GET", "/v1/intelligence/ranker/export-view"},
    {"ranker.fit", "POST", "/v1/intelligence/ranker/fit"},
    {"rules.delete", "POST", "/v1/rules/delete"},
    {"server.health", "GET", "/v1/server/health"},
    {"server.info", "GET", "/v1/server/info"},
    {"session.brief", "POST", "/v1/sessions/brief"},
    {"session.brief_assemble", "POST", "/v1/session/brief_assemble"},
    {"session.close", "POST", "/v1/sessions/close"},
    {"session.create", "POST", "/v1/sessions/create"},
    {"session.get", "POST", "/v1/sessions/get"},
    {"session.list", "POST", "/v1/sessions/list"},
    {"session.presence", "GET", "/v1/sessions/presence"},
    {"session.record_transcript", "POST", "/v1/sessions/record_transcript"},
    {"skill.archive", "POST", "/v1/skills/archive"},
    {"skill.autostub", "POST", "/v1/skills/autostub"},
    {"skill.create", "POST", "/v1/skills/create"},
    {"skill.edit", "POST", "/v1/skills/edit"},
    {"skill.eval", "POST", "/v1/skills/eval"},
    {"skill.lifecycle", "POST", "/v1/skills/lifecycle"},
    {"skill.lint", "POST", "/v1/skills/lint"},
    {"skill.list", "GET", "/v1/skills"},
    {"skill.patch", "POST", "/v1/skills/patch"},
    {"skill.pin", "POST", "/v1/skills/pin"},
    {"skill.show", "POST", "/v1/skills/show"},
    {"skill.unpin", "POST", "/v1/skills/unpin"},
    {"tool.execute", "POST", "/v1/tools/execute"},
    {"toolset.list", "GET", "/v1/toolsets"},
    {"toolset.resolve", "POST", "/v1/toolsets/resolve"},
    {"toolset.show", "POST", "/v1/toolsets/show"},
    {"trajectory.batch", "POST", "/v1/trajectory/batch"},
    {"trajectory.export", "POST", "/v1/trajectory/export"},
    {"trigger.cancel", "POST", "/v1/trigger/cancel"},
    {"trigger.fire", "POST", "/v1/trigger/fire"},
    {"trigger.list", "GET", "/v1/trigger/list"},
    {"trigger.status", "POST", "/v1/trigger/status"},
    {"vault.capability", "POST", "/v1/vault/capability"},
    {"vault.delete", "POST", "/v1/vault/delete"},
    {"vault.list", "POST", "/v1/vault/list"},
    {"vault.lock", "POST", "/v1/vault/lock"},
    {"vault.rekey", "POST", "/v1/vault/rekey"},
    {"vault.set", "POST", "/v1/vault/set"},
    {"vault.set_server", "POST", "/v1/vault/set_server"},
    {"vault.unlock", "POST", "/v1/vault/unlock"},
    {"wm.context", "POST", "/v1/wm/context"},
    {"wm.get", "POST", "/v1/wm/get"},
    {"wm.list", "POST", "/v1/wm/list"},
    {"wm.set", "POST", "/v1/wm/set"},
    {"workers", "GET", "/v1/workers"},
    {"workspace.context", "POST", "/v1/workspaces/context"},
    {"workspace.list", "GET", "/v1/workspaces"},
    {"workspace.mirror-sync", "POST", "/v1/workspace/mirror-sync"},
    {"worktree.gc", "POST", "/v1/worktree/gc"},
};

static const struct
{
   const char *method;
   const char *verb;
   const char *path;
} CLI_V1_ASYNC_ROUTES[] = {
    {"curator.synthesize", "POST", "/v1/curator/synthesize"},
#if AIMEE_WITH_ROUNDTABLE
    {"delegate.aggregate", "POST", "/v1/delegate/aggregate"},
#endif
    {"dev.sweep", "POST", "/v1/dev/sweep"},
    {"eval.run", "POST", "/v1/eval/run"},
    {"graph.sync_code", "POST", "/v1/graph/sync_code"},
    {"index.ingest", "POST", "/v1/index/ingest"},
    {"index.scan", "POST", "/v1/index/scan"},
    {"kb.build", "POST", "/v1/kb/build"},
    {"kb.docs.push", "POST", "/v1/kb/docs/push"},
    {"kb.ingest", "POST", "/v1/kb/ingest"},
    {"kb.reembed", "POST", "/v1/kb/reembed"},
    {"kb.update", "POST", "/v1/kb/update"},
    {"memory.benchmark", "POST", "/v1/memory/benchmark"},
    {"memory.embed", "POST", "/v1/memory/embed"},
    {"roundtable.review", "POST", "/v1/roundtable/review"},
    {"rules.generate", "POST", "/v1/rules/generate"},
};
/* @@GEN-CLI-V1-ROUTES END @@ */

/* First-class /v1 REST route resolvers — SHARED across POSIX and Windows so both
 * thin clients route a method to its dedicated dispatch-backed endpoint. Pure
 * string-map lookups (no platform deps).
 * The HTTP response is byte-identical to the NDJSON socket. Data-write routes
 * are denied on TCP unless aimee.api.remote_writes is data/full; privileged
 * exec/control routes require full. Both GET and POST routes carry the request
 * body — the server reads it via Content-Length regardless of verb — so
 * param-bearing GET reads (skill.list, work.list, …) keep their filters. */
const char *cli_v1_route_for_method(const char *method, const char **verb_out)
{
   if (verb_out)
      *verb_out = "POST";
   if (!method || !method[0])
      return NULL;
   /* Bespoke-but-dispatch-compatible routes the generator can't auto-detect:
    * the server handler is custom but still echoes the raw dispatch response
    * (e.g. ws_dispatch_args -> loopback_rpc for workspace.add). */
   static const struct
   {
      const char *method;
      const char *verb;
      const char *path;
   } bespoke[] = {
       {"workspace.add", "POST", "/v1/workspaces"},
       /* The mirror tier's client-diff upload. Without this mapping the client
        * resolves no route and ships nothing, so a remote server reconstructs a
        * clean checkout at head and silently drops every uncommitted change. */
       {"workspace.mirror-sync", "POST", "/v1/workspace/mirror-sync"},
       /* Detached-workspace runner reverse channel (aimee workspace serve); the
        * REST twins return the same {ok, have_op, op?} / {ok} as the NDJSON ops. */
       {"runner.poll", "POST", "/v1/runner/poll"},
       {"runner.respond", "POST", "/v1/runner/respond"},
       /* Custom-handler routes whose response still matches the dispatch method. */
       {"kb.search", "POST", "/v1/kb/search"},
       /* Write-tier grant administration. Bespoke handlers, so the generator cannot see
        * them; POST on all three because the thin client marshals flags into a body. The
        * server refuses these over TCP (v1_route_requires_uds), so a remote endpoint fails
        * there rather than here. */
       /* Same route as list: `show` is that listing filtered to one subject, so the row shape
        * has one definition. Only the METHOD differs, so the marshaller can require a
        * subject — without that separation, `show` with no subject silently lists everything. */
       {"kb.health", "GET", "/v1/kb/status"},
       {"kb.ingest.status", "GET", "/v1/kb/ingest/status"},
       {"kb.status", "GET", "/v1/kb/status"},
       {"kb.curator", "GET", "/v1/kb/curator"},
       {"memory.recall", "POST", "/v1/memory/recall"},
       {"rules.list", "GET", "/v1/rules"},
       {"notes.list", "GET", "/v1/notes"},
       {"notes.search", "POST", "/v1/notes/search"},
       {"doctor.forensics", "GET", "/v1/server/forensics"},
   };
   for (size_t i = 0; i < sizeof(bespoke) / sizeof(bespoke[0]); i++)
   {
      if (strcmp(method, bespoke[i].method) == 0)
      {
         if (verb_out)
            *verb_out = bespoke[i].verb;
         return bespoke[i].path;
      }
   }
   for (size_t i = 0; i < sizeof(CLI_V1_GEN_ROUTES) / sizeof(CLI_V1_GEN_ROUTES[0]); i++)
   {
      if (strcmp(method, CLI_V1_GEN_ROUTES[i].method) == 0)
      {
         if (verb_out)
            *verb_out = CLI_V1_GEN_ROUTES[i].verb;
         return CLI_V1_GEN_ROUTES[i].path;
      }
   }
   return NULL;
}

/* {id}-bearing /v1 routes: PREFIX{id}SUFFIX (e.g. /v1/workspaces/{path},
 * /v1/sessions/{id}/attach, /v1/sessions/{id}/primary). Returns the static
 * prefix and fills the verb, the suffix after {id}, and the request field that
 * carries the id ("session_id"; NULL means the first positional arg). NULL = not
 * a path-id route. Dispatch-backed responses, so they parse like the socket. */
const char *cli_v1_pathid_route_for_method(const char *method, const char **verb_out,
                                           const char **suffix_out, const char **id_field_out)
{
   if (verb_out)
      *verb_out = "GET";
   if (suffix_out)
      *suffix_out = "";
   if (id_field_out)
      *id_field_out = NULL;
   if (!method || !method[0])
      return NULL;
   static const struct
   {
      const char *method;
      const char *verb;
      const char *prefix;
      const char *suffix;
      const char *id_field;
   } map[] = {
       {"workspace.get", "GET", "/v1/workspaces/", "", NULL},
       {"workspace.remove", "DELETE", "/v1/workspaces/", "", NULL},
       {"session.attach", "POST", "/v1/sessions/", "/attach", "session_id"},
       {"session.detach", "POST", "/v1/sessions/", "/detach", "session_id"},
       {"primary.get", "GET", "/v1/sessions/", "/primary", "session_id"},
       {"primary.set", "POST", "/v1/sessions/", "/primary", "session_id"},
       {"primary.clear", "DELETE", "/v1/sessions/", "/primary", "session_id"},
   };
   for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
   {
      if (strcmp(method, map[i].method) == 0)
      {
         if (verb_out)
            *verb_out = map[i].verb;
         if (suffix_out)
            *suffix_out = map[i].suffix;
         if (id_field_out)
            *id_field_out = map[i].id_field;
         return map[i].prefix;
      }
   }
   return NULL;
}

/* Build the full {id}-bearing URL for `method` into buf (PREFIX + pct(id) +
 * SUFFIX), returning buf and the verb, or NULL if `method` is not a path-id
 * route or its id (req's id_field, else the first positional arg) is missing or
 * too long. */
static const char *cli_v1_pathid_build(const char *method, cJSON *req, char *buf, size_t cap,
                                       const char **verb_out)
{
   const char *suffix = NULL;
   const char *id_field = NULL;
   const char *prefix = cli_v1_pathid_route_for_method(method, verb_out, &suffix, &id_field);
   if (!prefix)
      return NULL;
   const char *id = NULL;
   if (id_field)
   {
      cJSON *f = cJSON_GetObjectItemCaseSensitive(req, id_field);
      id = (cJSON_IsString(f) && f->valuestring) ? f->valuestring : NULL;
   }
   else
   {
      cJSON *aarr = cJSON_GetObjectItemCaseSensitive(req, "args");
      cJSON *a0 = cJSON_IsArray(aarr) ? cJSON_GetArrayItem(aarr, 0) : NULL;
      id = (cJSON_IsString(a0) && a0->valuestring) ? a0->valuestring : NULL;
   }
   if (!id || !id[0])
      return NULL;
   char enc[1024];
   if (cli_v1_pct_encode(id, enc, sizeof(enc)) != 0)
      return NULL;
   if (snprintf(buf, cap, "%s%s%s", prefix, enc, suffix) >= (int)cap)
      return NULL;
   return buf;
}

/* cli_v1_async_route_for_method: a queued (rh_dispatch_op_async) route. POSTing
 * returns a run handle to poll at GET /v1/runs/{id}. NULL = not async. */
const char *cli_v1_async_route_for_method(const char *method, const char **verb_out)
{
   if (verb_out)
      *verb_out = "POST";
   if (!method || !method[0])
      return NULL;
   for (size_t i = 0; i < sizeof(CLI_V1_ASYNC_ROUTES) / sizeof(CLI_V1_ASYNC_ROUTES[0]); i++)
   {
      if (strcmp(method, CLI_V1_ASYNC_ROUTES[i].method) == 0)
      {
         if (verb_out)
            *verb_out = CLI_V1_ASYNC_ROUTES[i].verb;
         return CLI_V1_ASYNC_ROUTES[i].path;
      }
   }
   return NULL;
}

/* cli_v1_send: one /v1 request over the active transport — a remote endpoint
 * (cli_http_request), the co-located local UDS (cli_v1_http_request), or the
 * Windows remote client (aimee_client_request) — returning the parsed JSON
 * response (caller frees) or NULL on a transport error. Unifies the platform
 * branches so cli_v1_forward and the async poller share one code path. */
static cJSON *cli_v1_send(const char *remote, const char *bearer, const char *verb,
                          const char *path, const char *body, int timeout_ms, int *status_out)
{
   int status = 0;
   cJSON *resp = NULL;
#if !defined(_WIN32) && !defined(_WIN64)
   if (remote)
   {
      resp = cli_http_request(remote, verb, path, body, bearer, timeout_ms, &status);
   }
   else
   {
      char *r = cli_v1_http_request(verb, path, body, timeout_ms, &status);
      if (r)
      {
         resp = cJSON_Parse(r);
         free(r);
      }
   }
#else
   (void)remote;
   (void)bearer;
   (void)timeout_ms;
   char *r = aimee_client_request(verb, path, body, &status);
   if (r)
   {
      /* Parse the body whatever the status, as the POSIX branch above does. The
       * server states WHY it refused in the body of its 4xx ({"status":"error",
       * "message":...}), and the caller already renders that. Gating the parse on
       * 2xx threw those bodies away and left resp==NULL, which the caller can only
       * report as "could not reach the aimee-server /v1 endpoint (is the server
       * running?)" -- so on Windows an authorization refusal was indistinguishable
       * from an outage, and sent users to debug a server that had just answered.
       * A non-JSON body still parses to NULL and falls through as before. */
      resp = cJSON_Parse(r);
      free(r);
   }
#endif
   if (status_out)
      *status_out = status;
   return resp;
}

static void cli_v1_print_json_response(cJSON *resp)
{
   char *str = cJSON_PrintUnformatted(resp);
   if (str)
   {
      puts(str);
      free(str);
   }
}

static int cli_v1_response_is_error(cJSON *resp, int http_status)
{
   if (!resp)
      return 0;
   if (http_status >= 400)
      return 1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
      return 1;
   return cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(resp, "error"));
}

/* Extract the most specific cause carried by a terminal failed op-run. The
 * returned pointer is owned by result/snapshot and remains valid until those
 * objects are deleted. Kept separate so every error-envelope shape has a small
 * regression-testable contract. */
const char *cli_v1_run_failure_reason(cJSON *result, cJSON *snapshot)
{
   if (result)
   {
      cJSON *error = cJSON_GetObjectItemCaseSensitive(result, "error");
      if (cJSON_IsString(error) && error->valuestring[0])
         return error->valuestring;
      if (cJSON_IsObject(error))
      {
         cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
         if (cJSON_IsString(message) && message->valuestring[0])
            return message->valuestring;
      }

      /* Dispatch handlers commonly return the ordinary error envelope
       * {"status":"error","message":"..."}. The op-run worker correctly
       * marks that as failed, so preserve the handler's useful cause. */
      cJSON *result_status = cJSON_GetObjectItemCaseSensitive(result, "status");
      cJSON *result_message = cJSON_GetObjectItemCaseSensitive(result, "message");
      if (cJSON_IsString(result_status) && strcmp(result_status->valuestring, "error") == 0 &&
          cJSON_IsString(result_message) && result_message->valuestring[0])
         return result_message->valuestring;
   }

   if (snapshot)
   {
      cJSON *error = cJSON_GetObjectItemCaseSensitive(snapshot, "error");
      if (cJSON_IsString(error) && error->valuestring[0])
         return error->valuestring;
      if (cJSON_IsObject(error))
      {
         cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
         if (cJSON_IsString(message) && message->valuestring[0])
            return message->valuestring;
      }
      cJSON *message = cJSON_GetObjectItemCaseSensitive(snapshot, "message");
      if (cJSON_IsString(message) && message->valuestring[0])
         return message->valuestring;
   }
   return NULL;
}

/* A terminal async run may carry far more than its error string. Roundtable
 * failures, for example, retain per-seat failure categories and deadline state.
 * Preserve an object result intact and add the dispatch error markers needed by
 * cli_v1_forward; synthesize the legacy object envelope only when the run did
 * not record a structured result. Takes ownership of result. May return NULL on
 * allocation failure; cli_v1_forward rejects a NULL response before calling the
 * response finisher. */
static cJSON *cli_v1_failed_run_response(cJSON *result, cJSON *snapshot)
{
   const char *why = cli_v1_run_failure_reason(result, snapshot);
   char msg[320];
   if (why)
      snprintf(msg, sizeof(msg), "%s", why);
   else
   {
      cJSON *status = snapshot ? cJSON_GetObjectItemCaseSensitive(snapshot, "status") : NULL;
      const char *status_name =
          cJSON_IsString(status) && status->valuestring[0] ? status->valuestring : "failed";
      snprintf(msg, sizeof(msg), "run %s with no result", status_name);
   }

   if (cJSON_IsObject(result))
   {
      cJSON *status = cJSON_GetObjectItemCaseSensitive(result, "status");
      if (!cJSON_IsString(status) || strcmp(status->valuestring, "error") != 0)
      {
         cJSON_DeleteItemFromObjectCaseSensitive(result, "status");
         cJSON_AddStringToObject(result, "status", "error");
      }
      if (!cJSON_GetObjectItemCaseSensitive(result, "message"))
         cJSON_AddStringToObject(result, "message", msg);
      return result;
   }

   cJSON *env = cJSON_CreateObject();
   cJSON *err = env ? cJSON_AddObjectToObject(env, "error") : NULL;
   if (err)
   {
      cJSON_AddStringToObject(err, "message", msg);
      cJSON_AddStringToObject(err, "type", "run_failed");
   }
   cJSON_Delete(result);
   return env;
}

/* cli_v1_run_and_poll: POST an async (rh_dispatch_op_async) route, then poll
 * GET /v1/runs/{id} until the run is terminal and return its `result` — the raw
 * dispatch payload, matching the synchronous dispatch response shape, so the
 * CLI's response handling is unchanged. Caller frees. */
static cJSON *cli_v1_run_and_poll(const char *remote, const char *bearer, const char *verb,
                                  const char *path, const char *body, int timeout_ms)
{
   int status = 0;
   cJSON *queued = cli_v1_send(remote, bearer, verb, path, body, 30000, &status);
   if (!queued)
      return NULL;
   cJSON *idj = cJSON_GetObjectItemCaseSensitive(queued, "id");
   if (!cJSON_IsString(idj) || !idj->valuestring || !idj->valuestring[0])
      return queued; /* not a run handle (e.g. a synchronous error body) — surface it */
   char runpath[160];
   /* run ids are oprun_<digits>_<digits>: URL-safe, no percent-encoding needed. */
   snprintf(runpath, sizeof(runpath), "/v1/runs/%s", idj->valuestring);
   cJSON_Delete(queued);

   /* Bound the WALL CLOCK, not the sum of the sleeps.
    *
    * `waited += step_ms` counted only the 500ms pause while each poll below can
    * itself block for its own timeout. A server that accepts the poll and then
    * goes quiet cost 15.5s of real time per iteration and credited 0.5s of it,
    * so a declared 300000ms budget could run for 600 iterations x 15.5s -- over
    * two and a half HOURS. A timeout that overruns by two orders of magnitude
    * is indistinguishable from a hang, and was read as one.
    *
    * Two parts, and both are needed: measure elapsed against a monotonic
    * deadline, and give each poll only the time that is actually LEFT, so a
    * single stalled request cannot overshoot the budget on its own. */
   const long long deadline_ms = timeout_ms > 0 ? util_now_ms() + timeout_ms : 0;
   const int step_ms = 500;
   /* A long remote run (delegate ensembles, kb.build, index.scan, …) polls for many
    * minutes. A single transient poll failure — a TLS/connection blip while the run
    * is still progressing server-side — must NOT abandon it; that surfaced as the
    * misleading "could not reach the /v1 endpoint" on long roundtables. Tolerate a
    * bounded run of CONSECUTIVE poll failures (reset on any success) before giving
    * up, so a momentary blip is ridden out instead of failing the whole run. */
   int consec_fail = 0;
   const int max_consec_fail = 20;
   for (;;)
   {
      /* Never hand a single poll more time than the whole call has left. */
      int poll_ms = 15000;
      if (deadline_ms)
      {
         long long left = deadline_ms - util_now_ms();
         if (left <= 0)
            return NULL;
         if (left < poll_ms)
            poll_ms = (int)left;
      }
      cJSON *snap = cli_v1_send(remote, bearer, "GET", runpath, NULL, poll_ms, &status);
      if (!snap)
      {
         if (++consec_fail > max_consec_fail)
            return NULL;
      }
      else
      {
         consec_fail = 0;
         cJSON *st = cJSON_GetObjectItemCaseSensitive(snap, "status");
         if (cJSON_IsString(st) &&
             (strcmp(st->valuestring, "completed") == 0 || strcmp(st->valuestring, "failed") == 0 ||
              strcmp(st->valuestring, "cancelled") == 0))
         {
            int ok = strcmp(st->valuestring, "completed") == 0;
            cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(snap, "result");
            if (ok && result)
            {
               cJSON_Delete(snap);
               return result;
            }
            /* A failed/cancelled run, or a completed one with no result, used to
             * become an empty object. Printers then rendered that as a zero-valued
             * SUCCESS — `aimee kb build` reported "files indexed: 0, chunks added: 0"
             * with exit status 0 when the run had actually failed, hiding the failure
             * from humans and scripts alike. Surface it as an error envelope instead,
             * carrying whatever the run recorded. */
            /* A failed run usually carries its reason inside `result` (e.g.
             * {"result":{"error":"rpc produced no response"}}), so look there first. */
            cJSON *env = cli_v1_failed_run_response(result, snap);
            cJSON_Delete(snap);
            return env;
         }
         cJSON_Delete(snap);
      }
      if (deadline_ms && util_now_ms() >= deadline_ms)
         return NULL;
      cli_v1_sleep_ms(step_ms);
   }
}

/* --- Thin-client workspace push (detached-workspace ingestion) ---------------
 *
 * When a thin client talks to a remote aimee-server over TCP, the server cannot
 * see this host's filesystem. So `aimee workspace add <path>` resolves the path
 * locally, registers it as a `detached` workspace (server stores it verbatim and
 * skips its own scan), then pushes the file contents itself to
 * POST /v1/index/ingest. `aimee index scan` re-pushes. The collector is POSIX
 * only, so these helpers are no-ops on Windows (the reverse-channel serve path
 * still works there; client-push indexing does not). */
#if defined(AIMEE_POSIX)

/* Keep the thin-client push path aligned with the local/canonical scanners.
 * Hidden-root projects are deliberately excluded from index reads and startup
 * cleanup so temporary .aimee/.claude worktrees cannot pollute the shared code
 * graph.  Accepting one here used to report a successful upload whose symbols
 * were immediately invisible to list/find/callers. */
static int cli_ws_root_has_hidden_component(const char *path)
{
   if (!path)
      return 0;
   const char *p = path;
   while (*p == '/')
      p++;
   const char *start = p;
   for (;;)
   {
      if (*p == '/' || *p == '\0')
      {
         if (p > start && start[0] == '.')
            return 1;
         if (*p == '\0')
            return 0;
         start = ++p;
      }
      else
      {
         p++;
      }
   }
}

static int cli_ws_reject_hidden_root(const char *abs_root)
{
   if (!cli_ws_root_has_hidden_component(abs_root))
      return 0;
   fprintf(stderr,
           "aimee: refusing to index hidden root: %s\n"
           "  Index the non-hidden canonical checkout instead. Temporary .aimee/.claude\n"
           "  worktrees are intentionally excluded from the shared code index.\n",
           abs_root);
   return 1;
}

/* Pull the human-readable message out of an {"error":...} envelope (object or
 * string form); returns NULL when there is no error. */
static const char *cli_ws_err_message(cJSON *resp)
{
   if (!resp)
      return NULL;
   cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
   if (cJSON_IsString(err))
      return err->valuestring;
   if (cJSON_IsObject(err))
   {
      cJSON *m = cJSON_GetObjectItemCaseSensitive(err, "message");
      if (cJSON_IsString(m))
         return m->valuestring;
      return "error";
   }
   /* Dispatch-error envelope: {"status":"error","message":"..."} (e.g. an op-run
    * that failed with PAYLOAD_TOO_LARGE). Without this the failure has no "error"
    * key and would be silently counted as success. */
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
   {
      cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
      return (cJSON_IsString(m) && m->valuestring) ? m->valuestring : "error";
   }
   return NULL;
}

/* Collect <abs_root>'s source files locally and push them to the server's
 * /v1/index/ingest in size-bounded batches, blocking on each op-run. The kb
 * code-scan upserts per project+path, so batches accumulate. Returns 0 on
 * success, nonzero if any batch failed. */

/* Per-batch content budget. Batching bounds client memory and keeps each
 * relayed request under the 1 MB body cap of pre-KB_HTTP_BODY_MAX aimee-kb
 * images (the server relays each batch on to kb); JSON escaping inflates the
 * wire body above the raw content total, so budget well under 1 MB. */
#define CLI_INGEST_BATCH_BYTES (600 * 1024)

/* POST one batch of files (adopted/freed) as project `name`/`root`, polling the
 * async op-run to completion. Returns 0 on success. */
static int cli_ws_ingest_batch(const char *remote, const char *bearer, const char *name,
                               const char *root, cJSON *batch)
{
   cJSON *body = cJSON_CreateObject();
   if (!body)
   {
      cJSON_Delete(batch);
      return 1;
   }
   cJSON_AddStringToObject(body, "name", name);
   cJSON_AddStringToObject(body, "root", root);
   cJSON_AddItemToObject(body, "files", batch); /* adopt */
   char *body_json = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!body_json)
      return 1;

   cJSON *resp = cli_v1_run_and_poll(remote, bearer, "POST", "/v1/index/ingest", body_json, 600000);
   free(body_json);
   if (!resp)
      return 1;
   const char *err = cli_ws_err_message(resp);
   int rc = err ? 1 : 0;
   if (err)
      fprintf(stderr, "aimee: index ingest batch failed: %s\n", err);
   cJSON_Delete(resp);
   return rc;
}

/* Streaming ingest state: code_collect_files_cb hands us one file at a time and
 * we accumulate them into byte-bounded batches, pushing (and freeing) each batch
 * the moment it fills. This keeps memory to ~one batch regardless of tree size,
 * so there is no file-count cap — an arbitrarily large workspace ingests fully. */
typedef struct
{
   const char *remote;
   const char *bearer;
   const char *base;
   const char *abs_root;
   cJSON *batch; /* current open batch, or NULL */
   size_t batch_bytes;
   int pushed;
   int failed;
   int batches;
   int oom; /* could not allocate a batch array */
} ws_ingest_ctx_t;

/* Push the current batch (cli_ws_ingest_batch adopts/frees it) and reset. */
static void ws_ingest_flush(ws_ingest_ctx_t *s)
{
   if (!s->batch)
      return;
   int have = cJSON_GetArraySize(s->batch);
   if (have == 0)
   {
      cJSON_Delete(s->batch);
   }
   else
   {
      int batch_no = s->batches + 1;
      int batch_ok = cli_ws_ingest_batch(s->remote, s->bearer, s->base, s->abs_root, s->batch) == 0;
      if (batch_ok)
         s->pushed += have;
      else
         s->failed += have;
      s->batches++;
      fprintf(stderr, "index upload: batch %d %s (%d file%s; %d uploaded total)\n", batch_no,
              batch_ok ? "complete" : "failed", have, have == 1 ? "" : "s", s->pushed);
   }
   s->batch = NULL;
   s->batch_bytes = 0;
}

static int ws_ingest_collect_cb(const char *rel_path, const char *content, void *ctx)
{
   ws_ingest_ctx_t *s = (ws_ingest_ctx_t *)ctx;
   size_t flen = strlen(content) + strlen(rel_path) + 64;

   if (s->batch && cJSON_GetArraySize(s->batch) > 0 &&
       s->batch_bytes + flen > CLI_INGEST_BATCH_BYTES)
      ws_ingest_flush(s);

   if (!s->batch)
   {
      s->batch = cJSON_CreateArray();
      if (!s->batch)
      {
         s->oom = 1;
         return 1; /* stop the walk */
      }
   }

   cJSON *entry = cJSON_CreateObject();
   if (!entry)
      return 0; /* skip this file, keep walking */
   cJSON_AddStringToObject(entry, "rel_path", rel_path);
   cJSON_AddStringToObject(entry, "content", content);
   cJSON_AddItemToArray(s->batch, entry);
   s->batch_bytes += flen;
   return 0;
}

static int cli_ws_ingest_root(const char *remote, const char *bearer, const char *abs_root)
{
   const char *base = strrchr(abs_root, '/');
   base = (base && base[1]) ? base + 1 : abs_root;

   ws_ingest_ctx_t s = {0};
   s.remote = remote;
   s.bearer = bearer;
   s.base = base;
   s.abs_root = abs_root;

   code_collect_files_cb(abs_root, ws_ingest_collect_cb, &s);
   ws_ingest_flush(&s); /* flush the trailing partial batch */

   if (s.batches == 0 && !s.oom)
   {
      printf("no indexable files found in %s\n", abs_root);
      return 0;
   }

   if (s.oom)
      fprintf(stderr,
              "aimee: warning: ran out of memory building an ingest batch; some files in "
              "'%s' were not pushed\n",
              abs_root);
   printf("ingested %d file(s) from %s (%d batch%s)%s\n", s.pushed, abs_root, s.batches,
          s.batches == 1 ? "" : "es", (s.failed || s.oom) ? " — some batches failed" : "");
   return (s.failed || s.oom) ? 1 : 0;
}

/* Ingest a workspace tree as ONE PROJECT PER GIT REPO: code_collect_discover_repos
 * finds each real checkout under abs_root (skipping linked worktrees and symlink
 * cycles) and we ingest each as its own project (cli_ws_ingest_root names a
 * project by its basename). Falls back to ingesting abs_root itself when it holds
 * no nested git repo — e.g. adding a single repo directly, or a plain directory. */
typedef struct
{
   const char *remote;
   const char *bearer;
   int rc;
   int count;
} ws_tree_ctx_t;

static void ws_tree_ingest_cb(const char *repo_abs, void *ctx)
{
   ws_tree_ctx_t *t = (ws_tree_ctx_t *)ctx;
   const char *base = strrchr(repo_abs, '/');
   base = (base && base[1]) ? base + 1 : repo_abs;
   printf("indexing project: %s\n", base);
   fflush(stdout);
   if (cli_ws_ingest_root(t->remote, t->bearer, repo_abs) != 0)
      t->rc = 1;
   t->count++;
}

static int cli_ws_ingest_tree(const char *remote, const char *bearer, const char *abs_root)
{
   if (cli_ws_reject_hidden_root(abs_root))
      return 1;
   ws_tree_ctx_t t = {remote, bearer, 0, 0};
   code_collect_discover_repos(abs_root, ws_tree_ingest_cb, &t);
   if (t.count == 0)
      return cli_ws_ingest_root(remote, bearer, abs_root); /* no nested repo: ingest root itself */
   return t.rc;
}

int cli_workspace_add_remote(const char *path)
{
   if (!path || !path[0])
   {
      fprintf(stderr, "usage: aimee workspace add <path>\n");
      return 1;
   }
   /* `--repo <url>` exists in the local (same-host) command but not here, and the
    * flag used to fall through to realpath() and come back as
    * "cannot resolve path '--repo' on this host" — which reads like a broken path
    * argument rather than an unsupported mode, and says nothing about what to do.
    * Cloning onto the server needs a browser login (the clone route is webchat-only),
    * so name that instead of letting the user debug a phantom path. */
   if (path[0] == '-')
   {
      fprintf(stderr,
              "aimee: `workspace add %s` is not available against a remote server.\n"
              "  Cloning a repo onto the server is a browser operation: open the web UI\n"
              "  and use the setup wizard's \"Workspaces & projects\" step.\n"
              "  From the CLI, `aimee workspace add <path>` registers a path on THIS host.\n",
              path);
      return 1;
   }
   char *abs = realpath(path, NULL);
   if (!abs)
   {
      fprintf(stderr, "aimee: workspace: cannot resolve path '%s' on this host\n", path);
      return 1;
   }
   if (cli_ws_reject_hidden_root(abs))
   {
      free(abs);
      return 1;
   }

   char *remote = cli_v1_client_endpoint();
   char *bearer = cli_v1_client_bearer();

   /* Register as detached so the server keeps the client path verbatim (no
    * server-side realpath) and skips its own filesystem scan. */
   cJSON *reg = cJSON_CreateObject();
   cJSON_AddStringToObject(reg, "root", abs);
   cJSON_AddStringToObject(reg, "provider", "detached");
   char *reg_json = cJSON_PrintUnformatted(reg);
   cJSON_Delete(reg);

   int status = 0;
   cJSON *rresp = cli_v1_send(remote, bearer, "POST", "/v1/workspaces", reg_json, 60000, &status);
   free(reg_json);

   const char *err = cli_ws_err_message(rresp);
   int already = (err && strstr(err, "already registered"));
   if ((!rresp || status < 200 || status >= 300) && !already)
   {
      fprintf(stderr, "aimee: workspace add failed (HTTP %d)%s%s\n", status, err ? ": " : "",
              err ? err : "");
      if (rresp)
         cJSON_Delete(rresp);
      free(abs);
      free(remote);
      free(bearer);
      return 1;
   }
   if (rresp)
      cJSON_Delete(rresp);
   printf("workspace registered: %s (detached)\n", abs);
   fflush(stdout);

   int rc = cli_ws_ingest_tree(remote, bearer, abs);
   free(abs);
   free(remote);
   free(bearer);
   return rc;
}

int cli_index_scan_remote(int argc, char **argv)
{
   /* Positional args mirror `index scan [name] [root]`; flags (e.g. --force) are
    * ignored for the push path (a push is always a full re-collect). */
   const char *pos[2] = {NULL, NULL};
   int npos = 0;
   for (int i = 0; i < argc; i++)
   {
      if (argv[i] && argv[i][0] == '-')
         continue;
      if (npos < 2)
         pos[npos] = argv[i];
      npos++;
   }
   const char *root_arg = (npos >= 2) ? pos[1] : (npos == 1 ? pos[0] : NULL);

   char *remote = cli_v1_client_endpoint();
   char *bearer = cli_v1_client_bearer();
   int rc = 0;

   if (root_arg)
   {
      char *abs = realpath(root_arg, NULL);
      if (!abs)
      {
         fprintf(stderr, "aimee: index scan: cannot resolve path '%s' on this host\n", root_arg);
         free(remote);
         free(bearer);
         return 1;
      }
      rc = cli_ws_ingest_tree(remote, bearer, abs);
      free(abs);
      free(remote);
      free(bearer);
      return rc;
   }

   /* No path: re-ingest every detached workspace the server knows about. */
   int status = 0;
   cJSON *list = cli_v1_send(remote, bearer, "GET", "/v1/workspaces", NULL, 60000, &status);
   if (!list)
   {
      fprintf(stderr, "aimee: index scan: could not list workspaces (HTTP %d)\n", status);
      free(remote);
      free(bearer);
      return 1;
   }
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(list, "workspaces");
   int any = 0;
   cJSON *w = NULL;
   cJSON_ArrayForEach(w, arr)
   {
      cJSON *prov = cJSON_GetObjectItemCaseSensitive(w, "provider");
      cJSON *p = cJSON_GetObjectItemCaseSensitive(w, "path");
      if (cJSON_IsString(prov) && strcmp(prov->valuestring, "detached") == 0 && cJSON_IsString(p) &&
          p->valuestring && p->valuestring[0])
      {
         any = 1;
         if (cli_ws_ingest_tree(remote, bearer, p->valuestring) != 0)
            rc = 1;
      }
   }
   cJSON_Delete(list);
   if (!any)
      printf("no detached workspaces to scan; add one with 'aimee workspace add <path>'\n");
   free(remote);
   free(bearer);
   return rc;
}

#else /* !AIMEE_POSIX */

int cli_workspace_add_remote(const char *path)
{
   (void)path;
   fprintf(stderr, "aimee: remote workspace add is not supported on this platform\n");
   return 1;
}

int cli_index_scan_remote(int argc, char **argv)
{
   (void)argc;
   (void)argv;
   fprintf(stderr, "aimee: remote index scan is not supported on this platform\n");
   return 1;
}

#endif /* AIMEE_POSIX */

/* Ship a workspace patch as a sequence of bounded requests.
 *
 * A working tree is not bounded by anything the transport can choose, so sending
 * the patch whole made the route unusable on any real checkout: one generated
 * file exceeds the request cap, and raising the cap only moves the number that
 * will be wrong next. The server reassembles by appending each chunk to the same
 * file, so the size that matters is the tree's, not a request's.
 *
 * `seq` 0 truncates server-side; only the final chunk carries the head, so a head
 * is never stored against a patch that is still arriving.
 *
 * An older server ignores both fields and writes every chunk whole, which would
 * leave the mirror holding the LAST chunk while looking complete. Two things
 * prevent that. The first response must acknowledge `chunked` before any later
 * chunk is sent; and chunk 0 deliberately carries NO patch data, so the worst an
 * old server can be left holding is an empty diff — a defined state (a clean
 * checkout at the recorded head), never a fragment of one. Patch data starts at
 * seq 1. */
static int cli_v1_mirror_sync_chunked(cJSON *req, int timeout, int json_output)
{
   cJSON *jdiff = cJSON_GetObjectItemCaseSensitive(req, "diff");
   const char *diff = cJSON_IsString(jdiff) ? jdiff->valuestring : "";
   size_t total = strlen(diff);

   /* Sized against the PER-METHOD limit, not the transport one. Requests are
    * capped twice: SHTTP_MAX_BODY (4MB) at the listener, and a per-method limit
    * that defaults to LIMIT_DEFAULT — 256KB — for any method without its own
    * entry, which mirror-sync does not have. The method limit is the binding one
    * and rejects with PAYLOAD_TOO_LARGE.
    *
    * 128KB of patch leaves room for JSON escaping (worst case ~2x on a binary
    * patch's escapes) and the envelope inside that 256KB. Chunking means no limit
    * anywhere has to move: the size of one request stops being a function of the
    * size of the tree. */
   const size_t chunk_max = 128u * 1024u;

   char *remote = cli_v1_client_endpoint();
   char *bearer = remote ? cli_v1_client_bearer() : NULL;

   cJSON *jhead = cJSON_GetObjectItemCaseSensitive(req, "head");
   /* Borrowed, not copied: `req` outlives this loop, and copying pulled
    * safe_strdup (util.o) into a TU whose test link line does not carry it. */
   const char *head = cJSON_IsString(jhead) ? jhead->valuestring : "";
   cJSON *jbranch = cJSON_GetObjectItemCaseSensitive(req, "branch");
   cJSON *jupstream = cJSON_GetObjectItemCaseSensitive(req, "upstream");
   const char *branch = cJSON_IsString(jbranch) ? jbranch->valuestring : "";
   const char *upstream = cJSON_IsString(jupstream) ? jupstream->valuestring : "";

   /* A transfer id keeps the server's cross-request assembly ownership explicit.
    * The server serializes transfers from their empty begin chunk through final
    * metadata publication; without an id it could not distinguish a delayed
    * continuation from a second client using the same workspace. */
   unsigned char transfer_random[16];
   char transfer[33];
   if (platform_random_bytes(transfer_random, sizeof(transfer_random)) != 0)
   {
      fprintf(stderr, "aimee: workspace mirror-sync: cannot create transfer id\n");
      free(remote);
      free(bearer);
      return 1;
   }
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < sizeof(transfer_random); i++)
   {
      transfer[i * 2] = hex[transfer_random[i] >> 4];
      transfer[i * 2 + 1] = hex[transfer_random[i] & 0x0f];
   }
   transfer[32] = '\0';

   size_t sent = 0;
   int seq = 0, rc = 0;
   int data_chunks = (int)((total + chunk_max - 1) / chunk_max);
   /* One empty begin chunk, then the data. A patch that fits in a single request
    * still uses the begin chunk: one extra tiny round trip buys the same
    * old-server safety on every sync rather than only on large ones. */
   int chunks = data_chunks + 1;

   for (;;)
   {
      int begin = (seq == 0);
      size_t n = begin ? 0 : (total - sent > chunk_max ? chunk_max : total - sent);
      int final = !begin && (sent + n >= total);
      if (begin && total == 0)
         final = 1; /* nothing to ship: the begin chunk is the whole sync */

      cJSON *body = cJSON_CreateObject();
      cJSON_AddStringToObject(body, "method", "workspace.mirror-sync");
      cJSON *args = cJSON_CreateArray();
      cJSON *src_args = cJSON_GetObjectItemCaseSensitive(req, "args");
      cJSON *a = NULL;
      cJSON_ArrayForEach(a, src_args) if (cJSON_IsString(a))
          cJSON_AddItemToArray(args, cJSON_CreateString(a->valuestring));
      cJSON_AddItemToObject(body, "args", args);
      char *slice = (char *)malloc(n + 1);
      if (!slice)
      {
         cJSON_Delete(body);
         rc = 1;
         break;
      }
      memcpy(slice, diff + sent, n);
      slice[n] = '\0';
      cJSON_AddStringToObject(body, "diff", slice);
      free(slice);
      cJSON_AddNumberToObject(body, "seq", seq);
      cJSON_AddBoolToObject(body, "final", final);
      cJSON_AddStringToObject(body, "transfer", transfer);
      if (final && head[0])
         cJSON_AddStringToObject(body, "head", head);
      if (final && branch[0])
         cJSON_AddStringToObject(body, "branch", branch);
      if (final && upstream[0])
         cJSON_AddStringToObject(body, "upstream", upstream);

      char *wire = cJSON_PrintUnformatted(body);
      cJSON_Delete(body);
      if (!wire)
      {
         rc = 1;
         break;
      }
      int status = 0;
      cJSON *resp =
          cli_v1_send(remote, bearer, "POST", "/v1/workspace/mirror-sync", wire, timeout, &status);
      free(wire);
      if (!resp)
      {
         fprintf(stderr, "aimee: workspace mirror-sync: chunk %d of %d was not answered\n", seq + 1,
                 chunks);
         rc = 1;
         break;
      }
      /* The refusal envelope is {"status":"error","message":...}; an "error" key
       * only appears on some paths. Reading the wrong one made a server that had
       * answered clearly ("PAYLOAD_TOO_LARGE ...") fall through to the
       * capability message below and get reported as too old — a plain refusal
       * dressed up as a version problem. */
      cJSON *st = cJSON_GetObjectItemCaseSensitive(resp, "status");
      cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
      int failed = (cJSON_IsString(st) && strcmp(st->valuestring, "error") == 0) || err != NULL;
      if (failed)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (!cJSON_IsString(m) && cJSON_IsObject(err))
            m = cJSON_GetObjectItemCaseSensitive(err, "message");
         if (!cJSON_IsString(m) && cJSON_IsString(err))
            m = err;
         fprintf(stderr, "aimee: workspace mirror-sync: chunk %d of %d refused: %s\n", seq + 1,
                 chunks, cJSON_IsString(m) ? m->valuestring : "rejected");
         cJSON_Delete(resp);
         rc = 1;
         break;
      }
      /* Refuse to keep going against a server that cannot reassemble: it would
       * accept every chunk and end up storing only the last one. */
      if (!final)
      {
         cJSON *ack = cJSON_GetObjectItemCaseSensitive(resp, "chunked");
         if (!cJSON_IsTrue(ack))
         {
            fprintf(stderr,
                    "aimee: workspace mirror-sync: this patch needs %d chunks but the server did "
                    "not acknowledge chunked sync, so the parts would overwrite each other and the "
                    "mirror would look complete while holding only the last one. Upgrade the "
                    "server, or sync a tree small enough for one request.\n",
                    chunks);
            cJSON_Delete(resp);
            rc = 1;
            break;
         }
      }
      cJSON_Delete(resp);

      sent += n;
      seq++;
      if (final)
         break;
   }

   free(remote);
   free(bearer);

   if (rc == 0 && !json_output)
      printf("workspace mirror-sync: shipped %zu byte(s) in %d chunk(s)\n", total, seq);
   return rc;
}

/* cli_v1_dispatch_local: dispatch a pre-marshalled {method, ...params} request to its
 * first-class /v1 route over the co-located transport — the local aimee-http.sock
 * on POSIX, or the configured remote on Windows (no UDS). Interactive
 * co-located callers (chat, launch, mcp serve, hooks, triggers, gateway,
 * workspace serve) reach the dispatch surface via dedicated routes (async
 * methods are polled to completion). Returns the parsed response (caller frees)
 * or NULL. */
cJSON *cli_v1_dispatch_local(cJSON *req, int timeout_ms)
{
   if (!req)
      return NULL;
   cJSON *mj = cJSON_GetObjectItemCaseSensitive(req, "method");
   const char *method = (cJSON_IsString(mj) && mj->valuestring) ? mj->valuestring : NULL;
   if (!method)
      return NULL;
   if (timeout_ms <= 0)
      timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;

   const char *async_verb = NULL;
   const char *async_path = cli_v1_async_route_for_method(method, &async_verb);
   const char *rest_verb = NULL;
   const char *rest_path = NULL;
   char pathid_buf[1100];
   if (!async_path)
      rest_path = cli_v1_pathid_build(method, req, pathid_buf, sizeof(pathid_buf), &rest_verb);
   if (!async_path && !rest_path)
      rest_path = cli_v1_route_for_method(method, &rest_verb);

   char *body = cJSON_PrintUnformatted(req);
   if (!body)
      return NULL;
   cJSON *resp = NULL;
   int status = 0;
   if (async_path)
      resp = cli_v1_run_and_poll(NULL, NULL, async_verb, async_path, body, timeout_ms);
   else if (rest_path)
      resp = cli_v1_send(NULL, NULL, rest_verb, rest_path, body, timeout_ms, &status);
   /* else: no first-class route (e.g. the fire-and-forget chat.graceful_cancel,
    * which has no dispatch handler) — return NULL; callers already tolerate it. */
   free(body);
   return resp;
}

/* cli_v1_dispatch: like cli_v1_dispatch_local, but routes to a configured remote
 * /v1 endpoint when one is set (cli_v1_client_endpoint), falling back to the
 * co-located local UDS otherwise. Flows that must run against the SAME server that
 * will STORE the result — e.g. `agent setup` (codex-auth.json + the server vault) —
 * use this, since on a thin-client deployment that server is the remote, not a
 * local one. */
cJSON *cli_v1_dispatch(cJSON *req, int timeout_ms)
{
   if (!req)
      return NULL;
   cJSON *mj = cJSON_GetObjectItemCaseSensitive(req, "method");
   const char *method = (cJSON_IsString(mj) && mj->valuestring) ? mj->valuestring : NULL;
   if (!method)
      return NULL;
   if (timeout_ms <= 0)
      timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;

   const char *async_verb = NULL;
   const char *async_path = cli_v1_async_route_for_method(method, &async_verb);
   const char *rest_verb = NULL;
   const char *rest_path = NULL;
   char pathid_buf[1100];
   if (!async_path)
      rest_path = cli_v1_pathid_build(method, req, pathid_buf, sizeof(pathid_buf), &rest_verb);
   if (!async_path && !rest_path)
      rest_path = cli_v1_route_for_method(method, &rest_verb);

   char *body = cJSON_PrintUnformatted(req);
   if (!body)
      return NULL;
   char *remote = cli_v1_client_endpoint();
   char *bearer = remote ? cli_v1_client_bearer() : NULL;
   cJSON *resp = NULL;
   int status = 0;
   if (async_path)
      resp = cli_v1_run_and_poll(remote, bearer, async_verb, async_path, body, timeout_ms);
   else if (rest_path)
      resp = cli_v1_send(remote, bearer, rest_verb, rest_path, body, timeout_ms, &status);
   free(bearer);
   free(remote);
   free(body);
   return resp;
}

/* Finish one parsed /v1 response. Keeping ownership, classification, rendering,
 * and exit-code selection in one helper gives the CLI a single user-visible
 * contract and lets regression tests exercise the exact production path. resp is
 * consumed on every return. */
static int cli_v1_finish_response(const cli_v1_route_t *route, cJSON *resp, int http_status,
                                  int effective_json_output, int fwd_argc, char **fwd_argv)
{
   if (!resp)
   {
      fprintf(stderr, "aimee: server returned an empty response\n");
      return -1;
   }

   /* Protocol version mismatch -> fall back. */
   cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
   if (cJSON_IsString(err) && strstr(err->valuestring, "protocol version"))
   {
      cJSON_Delete(resp);
      return -1;
   }

   /* JSON mode is a machine contract on failures too. Preserve the complete
    * server envelope before the text-mode branches extract and discard its
    * message. This is especially important for roundtable.review: a parked
    * degraded/deadline result carries participant_failures beside the error,
    * and `--json` previously threw those diagnostics away. */
   if (effective_json_output && cli_v1_response_is_error(resp, http_status))
   {
      cli_v1_print_json_response(resp);
      cJSON_Delete(resp);
      return 1;
   }

   /* Server-side error. */
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         fprintf(stderr, "aimee: %s\n  (via %s)\n", msg->valuestring,
                 aimee_client_transport_label());
      cJSON_Delete(resp);
      return 1;
   }

   /* REST error envelope: {"error":{"message":..,"type":..}}. The server emits
    * this on any non-2xx -- e.g. 403 permission_error when a write is attempted
    * over a read-only remote listener (aimee.api.remote_writes=off). It is NOT a
    * {"status":"error"} body, so without this the success pretty-printers below
    * would misreport a denied write as success (e.g. print "stored memory"). */
   if (cJSON_IsObject(err))
   {
      cJSON *emsg = cJSON_GetObjectItemCaseSensitive(err, "message");
      fprintf(stderr, "aimee: %s\n  (via %s)\n",
              (cJSON_IsString(emsg) && emsg->valuestring[0]) ? emsg->valuestring : "request failed",
              aimee_client_transport_label());
      cJSON_Delete(resp);
      return 1;
   }
   /* Any other non-2xx: never render it as success. Prefer a string error
    * envelope {"error":"<msg>"} -- the shape server_http.c's err_json() emits
    * (28+ routes: 502/503 backend-unavailable, "no such persona/agent", "attach
    * refused", ...) -- over the generic HTTP line, which drops the real reason.
    * Gated on http_status so a 2xx success that legitimately carries a top-level
    * "error" string (e.g. cron.run returns {status:ok,...,error:"<job stderr>"})
    * is NOT misreported as a failure. */
   if (http_status >= 400)
   {
      if (cJSON_IsString(err) && err->valuestring[0])
         fprintf(stderr, "aimee: %s\n  (via %s)\n", err->valuestring,
                 aimee_client_transport_label());
      else
         fprintf(stderr, "aimee: server returned HTTP %d for '%s'\n  (via %s)\n", http_status,
                 route->method, aimee_client_transport_label());
      cJSON_Delete(resp);
      return 1;
   }

   int exit_rc = 0;
   if (strcmp(route->method, "init.run") == 0)
   {
      cJSON *knowledge = cJSON_GetObjectItemCaseSensitive(resp, "knowledge_ready");
      if (!cJSON_IsTrue(knowledge))
         exit_rc = 1;
   }
   else if (strcmp(route->method, "git.verify") == 0 && git_verify_response_is_failure(resp))
   {
      exit_rc = 1;
   }
   else if (strcmp(route->method, "model.probe") == 0 && agent_probe_response_is_failure(resp))
   {
      exit_rc = 1;
   }
   else if (strcmp(route->method, "roundtable.review") == 0 &&
            roundtable_review_response_is_failure(resp))
   {
      exit_rc = 1;
   }

   if (!effective_json_output && strcmp(route->method, "delegate") == 0)
   {
      const char *output_path = delegate_output_path_from_args(fwd_argc, fwd_argv);
      if (output_path && output_path[0])
      {
         cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "response");
         if (!cJSON_IsString(r))
         {
            cJSON_Delete(resp);
            fprintf(stderr, "aimee: delegate returned no response to write\n");
            return 1;
         }
         if (write_delegate_output_file(output_path, r->valuestring) != 0)
         {
            cJSON_Delete(resp);
            fprintf(stderr, "aimee: cannot write to --output path: %s\n", output_path);
            return 1;
         }
         cJSON_Delete(resp);
         fprintf(stderr, "%s\n", output_path);
         return exit_rc;
      }
   }

   if (effective_json_output)
   {
      if (route->extract)
      {
         /* Extract named array from response. */
         cJSON *arr = cJSON_DetachItemFromObjectCaseSensitive(resp, route->extract);
         if (arr)
         {
            char *str = cJSON_PrintUnformatted(arr);
            if (str)
            {
               puts(str);
               free(str);
            }
            cJSON_Delete(arr);
         }
         cJSON_Delete(resp);
      }
      else
      {
         /* Successful JSON responses keep their payload but omit the transport
          * status marker. Error envelopes returned above are intentionally not
          * stripped: status/error/message are part of their machine contract. */
         cJSON_DeleteItemFromObjectCaseSensitive(resp, "status");
         cli_v1_print_json_response(resp);
         cJSON_Delete(resp);
      }
   }
   else
   {
      print_text_output(route->method, resp);
      cJSON_Delete(resp);
   }

   return exit_rc;
}

/* --- Main /v1 forward function --- */

/* Methods that must not be dispatched until the operator has said yes.
 *
 * A LIST RATHER THAN A COLUMN in rpc_routes: that table is built with positional
 * initializers and -Werror=missing-field-initializers, so a seventh field would mean
 * editing every one of its ~200 rows to add ", 0". The gate is worth more than the
 * adjacency.
 *
 * workspace.remove is here because it silently succeeded. Adding another destructive
 * method is one line, and cli_v1_confirm_or_refuse's caveat text is where anything
 * surprising about it belongs. */
static const char *const CLI_V1_CONFIRM_METHODS[] = {
    "workspace.remove",
    NULL,
};

/* Ask before doing something the operator cannot undo from the CLI.
 *
 * CLIENT-SIDE, because the server has no terminal to ask at: by the time a request
 * reaches /v1 the only options left are to do it or refuse it.
 *
 * Two ways through, mirroring `aimee kb reembed`, which is the existing precedent for
 * a gated mutation: --confirm (or --yes) for a script, or typing y at a prompt. With
 * neither a flag nor a terminal there is nobody to ask, so it refuses -- a cron job
 * that removes a workspace should have to say so.
 *
 * Returns 1 to proceed, 0 to abort. */
static int cli_v1_confirm_or_refuse(const char *method, int argc, char **argv)
{
   int gated = 0;
   for (size_t i = 0; CLI_V1_CONFIRM_METHODS[i] && !gated; i++)
      gated = strcmp(method, CLI_V1_CONFIRM_METHODS[i]) == 0;
   if (!gated)
      return 1;

   static const char *bool_flags[] = {"confirm", "yes", "json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   if (rpc_get(&opts, "confirm") || rpc_get(&opts, "yes"))
      return 1;

   /* The first positional is the thing being acted on, when there is one. */
   const char *target = NULL;
   for (int i = 0; i < argc; i++)
      if (argv[i] && argv[i][0] != '-')
      {
         target = argv[i];
         break;
      }

   /* Say what it does AND what it does not. `workspace remove` drops the
    * registration and leaves the indexed corpus searchable, which is the part an
    * operator is most likely to get wrong -- and did, in testing. */
   const char *caveat = strcmp(method, "workspace.remove") == 0
                            ? "  The indexed corpus is NOT deleted: documents stay in the "
                              "knowledge base and stay searchable.\n"
                            : "";

   if (!isatty(STDIN_FILENO))
   {
      fprintf(stderr,
              "aimee: %s needs confirmation and stdin is not a terminal.\n%s"
              "  Re-run with --confirm.\n",
              method, caveat);
      return 0;
   }

   fprintf(stderr, "%s", caveat);
   fprintf(stderr, "%s%s%s: proceed? [y/N] ", method, target ? " " : "", target ? target : "");
   fflush(stderr);
   char line[16] = "";
   if (!fgets(line, sizeof(line), stdin) || (line[0] != 'y' && line[0] != 'Y'))
   {
      fprintf(stderr, "Aborted.\n");
      return 0;
   }
   return 1;
}

int cli_v1_forward(const char *socket_path, const cli_v1_route_t *route, int json_output,
                   const char *json_fields, const char *response_profile, int argc, char **argv)
{
   (void)json_fields;
   (void)response_profile;

   if (!route || !route->method)
      return -1;
   (void)socket_path; /* strict /v1: local aimee-http.sock or a remote /v1 endpoint */

   /* Skip subcmd args consumed by the route match (1 for single, 2 for compound) */
   int fwd_argc = argc;
   char **fwd_argv = argv;
   for (int skip = route->skip_subcmd; skip > 0 && fwd_argc > 0; skip--)
   {
      fwd_argc--;
      fwd_argv++;
   }
   if (!cli_v1_confirm_or_refuse(route->method, fwd_argc, fwd_argv))
      return 2;

   int effective_json_output = json_output || cli_v1_args_request_json(fwd_argc, fwd_argv);

   cJSON *req = marshal_request(route->method, fwd_argc, fwd_argv);
   if (!req)
   {
      /* A marshalling failure means the arguments could not be turned into a request, so
       * NOTHING was sent. That used to exit 2 in total silence for every command in the CLI,
       * which reads as "it worked" to a script and as nothing at all to a person. A review
       * asked for a generic explanation and it belongs here rather than in each marshaller.
       *
       * Suppressed when the marshaller already printed something specific — a per-method
       * message is strictly better than this one, and two messages for one mistake is worse
       * than either alone. */
      if (!marshal_request_take_reported())
         fprintf(stderr,
                 "aimee: '%s' — arguments are missing or invalid, so no request was sent.\n"
                 "  Run 'aimee help' or the command with no arguments for its usage.\n",
                 route->method);
      return 2;
   }

   int timeout = route->timeout_ms > 0 ? route->timeout_ms : CLIENT_DEFAULT_TIMEOUT_MS;
   if (strcmp(route->method, "workspace.mirror-sync") == 0)
   {
      int rc = cli_v1_mirror_sync_chunked(req, timeout, effective_json_output);
      cJSON_Delete(req);
      return rc;
   }
   if (strcmp(route->method, "delegate") == 0)
   {
      int delegate_timeout = delegate_timeout_from_args(fwd_argc, fwd_argv);
      if (delegate_timeout > timeout)
         timeout = delegate_timeout < 2147480000 ? delegate_timeout + 5000 : delegate_timeout;
   }

   cJSON *resp = NULL;

   /* Resolve the method's first-class /v1 route. Strict /v1, no /v1/rpc bridge:
    * every dispatch method has a route (the generated sync map, the async set, the
    * {id}-path map, or the bespoke supplement), enforced by the cli-v1-routes +
    * v1-method-coverage gates. Async (rh_dispatch_op_async) methods return a run
    * handle the poller below drives to completion; sync routes echo the raw
    * dispatch response, so the post-processing is unchanged from the socket path. */
   /* Resolve by the method the request actually dispatches — the body's "method"
    * field, which marshalling may set to a server_method twin (e.g. `git verify`
    * dispatches as mcp.call) — not the CLI route name. */
   cJSON *bm = cJSON_GetObjectItemCaseSensitive(req, "method");
   const char *disp_method = (cJSON_IsString(bm) && bm->valuestring && bm->valuestring[0])
                                 ? bm->valuestring
                                 : route->method;
   const char *rest_verb = NULL;
   const char *rest_path = NULL;
   const char *async_verb = NULL;
   const char *async_path = cli_v1_async_route_for_method(disp_method, &async_verb);
   char pathid_buf[1100];
   if (!async_path)
      rest_path = cli_v1_pathid_build(disp_method, req, pathid_buf, sizeof(pathid_buf), &rest_verb);
   if (!async_path && !rest_path)
      rest_path = cli_v1_route_for_method(disp_method, &rest_verb);
   /* Whether the METHOD is a {id}-bearing route at all, independent of whether
    * this invocation supplied the id. Captured HERE, before the request tree is
    * freed: disp_method can point into `req` (bm->valuestring), so it dangles
    * once cJSON_Delete(req) runs and must not be dereferenced below. */
   const int is_pathid_route =
       cli_v1_pathid_route_for_method(disp_method, NULL, NULL, NULL) != NULL;

   char *http_body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   /* Transport target: a configured remote /v1 endpoint ("tcp:host:port" /
    * "unix:path", authed by its bearer) reaches an aimee-server on another host;
    * otherwise the co-located server over the local aimee-http.sock. Windows is
    * always remote (resolved from --server / AIMEE_SERVER_URL inside cli_v1_send). */
   char *remote = cli_v1_client_endpoint();
   char *bearer = remote ? cli_v1_client_bearer() : NULL;
   int http_status = 0; /* last HTTP status from the REST send (0 for the async path) */

   if (http_body && async_path)
   {
      resp = cli_v1_run_and_poll(remote, bearer, async_verb, async_path, http_body, timeout);
   }
   else if (http_body && rest_path)
   {
      /* Send the marshalled body on every first-class route, GET included: the
       * server reads it via Content-Length regardless of verb (rh_dispatch_op), so
       * param-bearing GET reads and {id}-in-URL routes that also carry fields keep
       * them. Retry on transient unavailability (a server restart drops the
       * connection; a full compute queue returns {status:error,"compute queue
       * full"}) — both clear fast. */
      const char *body = http_body;
      /* A body over the listener's cap is dropped before it is ever parsed, so
       * the send returns nothing and the failure reads as "could not reach the
       * endpoint (is the server running?)" — pointing at a server that is up and
       * answering every other request. Measured on a real workspace:
       * `workspace mirror-sync` shipped 16.8MB (a repo with 247 untracked files)
       * against the 4MB cap and reported the server as unreachable.
       *
       * Refuse here instead, naming the two numbers, so the size is the finding
       * rather than something the operator has to infer. The roundtable review
       * path has its own larger cap and is checked against that one. */
      size_t body_len = body ? strlen(body) : 0;
      size_t body_cap = rest_path && strcmp(rest_path, "/v1/roundtable/review") == 0
                            ? (size_t)CLI_V1_MAX_ROUNDTABLE_BODY
                            : (size_t)CLI_V1_MAX_BODY;
      if (body_len > body_cap)
      {
         fprintf(stderr,
                 "aimee: '%s' request body is %zu bytes, over the %zu-byte limit the server "
                 "accepts, so it would be dropped before it was read. Reduce what the request "
                 "carries (for a workspace sync, untracked files dominate it) and retry.\n",
                 route->method, body_len, body_cap);
         free(http_body);
         free(bearer);
         free(remote);
         return -1;
      }
      static const int retry_delays_ms[] = {200, 500, 1000};
      const int max_retries = (int)(sizeof(retry_delays_ms) / sizeof(retry_delays_ms[0]));
      for (int attempt = 0; attempt <= max_retries; attempt++)
      {
         if (resp)
         {
            cJSON_Delete(resp);
            resp = NULL;
         }
         http_status = 0;
         resp = cli_v1_send(remote, bearer, rest_verb, rest_path, body, timeout, &http_status);
         if (resp)
         {
            cJSON *st = cJSON_GetObjectItemCaseSensitive(resp, "status");
            cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
            int queue_full = http_status >= 200 && http_status < 300 && cJSON_IsString(st) &&
                             strcmp(st->valuestring, "error") == 0 && cJSON_IsString(msg) &&
                             strstr(msg->valuestring, "compute queue full");
            if (!queue_full || attempt == max_retries)
               break;
         }
         if (attempt < max_retries)
            cli_v1_sleep_ms(retry_delays_ms[attempt]);
      }
   }
   else if (http_body)
   {
      /* Two different failures land here. A path-id route ({id}-bearing) whose id
       * is missing produced no path, which is a MISSING ARGUMENT, not a routing
       * gap: `aimee workspace get` with no path answered "'workspace.get' has no
       * /v1 route" while `aimee workspace get <path>` worked fine, sending the
       * user to look for a route that is present and correct. Ask
       * cli_v1_pathid_route_for_method() which case this is -- it reports whether
       * the METHOD is a path-id route at all, independently of whether this
       * invocation supplied the id. */
      const int missing_arg = is_pathid_route;
      if (missing_arg)
         fprintf(stderr, "aimee: '%s' needs an argument (the id or path to act on)\n",
                 route->method);
      else
         /* Genuinely no first-class route — surface it rather than silently
          * dropping the command. This is a routing gap, not an unreachable
          * server, so return early instead of falling through to the misleading
          * "is the server running?" hint below. */
         fprintf(stderr, "aimee: '%s' has no /v1 route\n", route->method);
      free(http_body);
      free(bearer);
      free(remote);
      /* >= 0 means "handled, this is the exit code" and suppresses the caller's
       * "server /v1 request failed" line. A missing argument IS fully handled --
       * no request was ever attempted, so blaming the server on the next line is
       * doubly wrong. A real routing gap keeps returning -1 so that path is
       * unchanged. */
      return missing_arg ? 1 : -1;
   }

   free(http_body);
   free(bearer);
   free(remote);

   if (resp == NULL)
   {
      fprintf(stderr,
              "aimee: '%s' could not reach the aimee-server /v1 endpoint (is the server "
              "running? check aimee.api.client_endpoint / AIMEE_API_ENDPOINT for a remote "
              "target).\n",
              route->method);
      return -1;
   }

   return cli_v1_finish_response(route, resp, http_status, effective_json_output, fwd_argc,
                                 fwd_argv);
}
