/* cli_agent_setup.c: thin-client `aimee agent setup <provider>` wizard.
 *
 * Supports exactly four providers:
 *   openai / anthropic         -> prompt for name/URL/model/key, create the agent
 *                                 via the agent.add RPC (server saves + vaults key).
 *   codex-oauth / claude-oauth -> install the vendor CLI on aimee-server and run
 *                                 its OAuth login there via the agent.cli_oauth_*
 *                                 RPCs. */
#include "cli_agent_setup.h"
#include "cli_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Read one interactive line from stdin into buf (newline/CR/trailing-space
 * stripped). Returns 1 if a non-empty value was read, else 0. */
static int setup_prompt_line(const char *label, char *buf, size_t n)
{
   fprintf(stderr, "%s", label);
   fflush(stderr);
   if (!fgets(buf, (int)n, stdin))
   {
      buf[0] = '\0';
      return 0;
   }
   size_t l = strlen(buf);
   while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r' || buf[l - 1] == ' '))
      buf[--l] = '\0';
   return buf[0] != '\0';
}

/* Resolve a server socket for `method` when no remote is configured. Returns 0 on
 * success (sets *sock, NULL in remote mode), 1 if no server is available. */
static int setup_ensure_server(const char *method, const char **sock)
{
   *sock = NULL;
   if (!cli_rpc_has_remote_endpoint())
   {
      *sock = cli_ensure_server_for_method(method);
      if (!*sock)
      {
         fprintf(stderr, "aimee agent setup: no aimee-server is available; start one or "
                         "configure a remote with `aimee remote set`\n");
         return 1;
      }
   }
   return 0;
}

/* openai / anthropic: prompt for the connection details, then create the agent
 * through the agent.add RPC so the server stores it and vaults the key. */
static int setup_api_provider_cmd(const char *provider, int json_output)
{
   const char *default_url = strcmp(provider, "anthropic") == 0 ? "https://api.anthropic.com/v1"
                                                                : "https://api.openai.com/v1";

   char name[128], url[512], model[256], key[4096];
   fprintf(stderr, "\n=== %s agent setup ===\n", provider);
   if (!setup_prompt_line("Agent name: ", name, sizeof(name)))
   {
      fprintf(stderr, "aimee agent setup: agent name is required\n");
      return 1;
   }
   char url_prompt[640];
   snprintf(url_prompt, sizeof(url_prompt), "Endpoint URL [%s]: ", default_url);
   if (!setup_prompt_line(url_prompt, url, sizeof(url)))
      snprintf(url, sizeof(url), "%s", default_url);
   if (!setup_prompt_line("Model name: ", model, sizeof(model)))
   {
      fprintf(stderr, "aimee agent setup: model name is required\n");
      return 1;
   }
   int have_key = setup_prompt_line("API key (leave blank for none): ", key, sizeof(key));

   /* Build an `agent add` invocation and forward it through the normal RPC route
    * (the server vaults --key over an attested connection). */
   char *sub[10];
   int n = 0;
   sub[n++] = "add";
   sub[n++] = name;
   sub[n++] = url;
   sub[n++] = model;
   sub[n++] = "--provider";
   sub[n++] = (char *)provider;
   if (have_key)
   {
      sub[n++] = "--key";
      sub[n++] = key;
   }

   cli_rpc_route_t route;
   if (!cli_rpc_lookup("agent", n, sub, &route))
   {
      fprintf(stderr, "aimee agent setup: no agent.add route available\n");
      return 1;
   }
   const char *server_method = route.server_method ? route.server_method : route.method;
   const char *sock = NULL;
   if (setup_ensure_server(server_method, &sock) != 0)
      return 1;
   int rc = cli_rpc_forward(sock, &route, json_output, NULL, NULL, n, sub);
   if (rc < 0)
   {
      fprintf(stderr, "aimee agent setup: server RPC request failed\n");
      return 1;
   }
   if (rc == 0 && !json_output)
      fprintf(stderr, "\nTest with: aimee agent test %s\n", name);
   return rc;
}

/* Dispatch one cli_oauth RPC step. Returns the parsed response (caller deletes)
 * or NULL; on a non-ok status prints the server message and returns NULL. */
static cJSON *setup_oauth_rpc(const char *method, const char *vendor, const char *session,
                              const char *code, int timeout_ms)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddStringToObject(req, "vendor", vendor);
   if (session)
      cJSON_AddStringToObject(req, "session", session);
   if (code)
      cJSON_AddStringToObject(req, "code", code);
   cJSON *resp = cli_v1_dispatch(req, timeout_ms);
   cJSON_Delete(req);
   if (!resp)
      return NULL;
   cJSON *st = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(st) || strcmp(st->valuestring, "ok") != 0)
   {
      cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
      fprintf(stderr, "aimee agent setup: %s\n",
              cJSON_IsString(m) ? m->valuestring : "server error");
      cJSON_Delete(resp);
      return NULL;
   }
   return resp;
}

/* codex-oauth / claude-oauth: install the vendor CLI on aimee-server and drive
 * its OAuth login (start -> optional code-back -> poll until authenticated). */
static int setup_oauth_cli_cmd(const char *vendor, int json_output)
{
   const char *sock = NULL;
   if (setup_ensure_server("agent.cli_oauth_start", &sock) != 0)
      return 1;

   fprintf(stderr, "\n=== %s — server-hosted OAuth setup ===\n", vendor);
   fprintf(stderr, "Installing the %s CLI on aimee-server and starting its login...\n", vendor);

   cJSON *started = setup_oauth_rpc("agent.cli_oauth_start", vendor, NULL, NULL, 120000);
   if (!started)
   {
      fprintf(stderr,
              "Could not start %s setup (is the server reachable and "
              "server_cli_oauth_enabled=true?).\n",
              vendor);
      return 1;
   }
   char session[160] = "";
   cJSON *j_session = cJSON_GetObjectItemCaseSensitive(started, "session");
   cJSON *j_url = cJSON_GetObjectItemCaseSensitive(started, "url");
   cJSON *j_code = cJSON_GetObjectItemCaseSensitive(started, "code");
   int needs_code_back = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(started, "needs_code_back"));
   if (cJSON_IsString(j_session))
      snprintf(session, sizeof(session), "%s", j_session->valuestring);

   fprintf(stderr, "\n1. Open this URL in your browser and authorize:\n   %s\n",
           cJSON_IsString(j_url) ? j_url->valuestring : "(none)");
   if (cJSON_IsString(j_code) && j_code->valuestring[0])
      fprintf(stderr, "2. Enter this one-time code on that page: %s\n", j_code->valuestring);
   cJSON_Delete(started);

   if (needs_code_back)
   {
      char code[600];
      if (setup_prompt_line("\nAfter authorizing, paste the code shown back here: ", code,
                            sizeof(code)))
      {
         cJSON *r = setup_oauth_rpc("agent.cli_oauth_code", vendor, session, code, 60000);
         if (!r)
            return 1;
         cJSON_Delete(r);
      }
   }

   fprintf(stderr, "\nWaiting for authentication");
   for (int i = 0; i < 100; i++)
   {
      fprintf(stderr, ".");
      fflush(stderr);
      sleep(3);
      cJSON *pr = setup_oauth_rpc("agent.cli_oauth_poll", vendor, session, NULL, 60000);
      if (!pr)
         continue;
      cJSON *j_state = cJSON_GetObjectItemCaseSensitive(pr, "state");
      const char *state = cJSON_IsString(j_state) ? j_state->valuestring : "pending";
      if (strcmp(state, "authenticated") == 0)
      {
         cJSON *j_agent = cJSON_GetObjectItemCaseSensitive(pr, "agent");
         const char *agent = cJSON_IsString(j_agent) ? j_agent->valuestring : vendor;
         if (json_output)
         {
            char *s = cJSON_PrintUnformatted(pr);
            if (s)
            {
               puts(s);
               free(s);
            }
         }
         else
         {
            fprintf(stderr,
                    "\n\xE2\x9C\x93 %s authenticated and registered as server-side agent '%s'.\n",
                    vendor, agent);
            fprintf(stderr, "Test with: aimee delegate review --via %s \"...\"\n", agent);
         }
         cJSON_Delete(pr);
         return 0;
      }
      if (strcmp(state, "failed") == 0)
      {
         cJSON *j_err = cJSON_GetObjectItemCaseSensitive(pr, "error");
         fprintf(stderr, "\naimee agent setup: %s authentication failed%s%s\n", vendor,
                 cJSON_IsString(j_err) ? ": " : "",
                 cJSON_IsString(j_err) ? j_err->valuestring : "");
         cJSON_Delete(pr);
         return 1;
      }
      cJSON_Delete(pr);
   }
   fprintf(stderr, "\nTimed out waiting for %s authentication.\n", vendor);
   return 1;
}

int handle_agent_setup_cmd(int argc, char **argv, int json_output)
{
   const char *provider = (argc >= 1) ? argv[0] : NULL;
   if (!provider || provider[0] == '\0')
   {
      fprintf(stderr, "aimee agent setup: provider required "
                      "(openai, anthropic, codex-oauth, claude-oauth)\n");
      return 1;
   }
   if (strcmp(provider, "openai") == 0 || strcmp(provider, "anthropic") == 0)
      return setup_api_provider_cmd(provider, json_output);
   if (strcmp(provider, "codex-oauth") == 0)
      return setup_oauth_cli_cmd("codex", json_output);
   if (strcmp(provider, "claude-oauth") == 0)
      return setup_oauth_cli_cmd("claude", json_output);
   fprintf(stderr,
           "aimee agent setup: unsupported provider '%s' "
           "(expected: openai, anthropic, codex-oauth, claude-oauth)\n",
           provider);
   return 1;
}
