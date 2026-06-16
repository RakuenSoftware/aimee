/* cli_agent_setup.c: thin-client `aimee agent setup <provider>` wizard.
 *
 * Supports exactly four providers:
 *   openai / anthropic         -> prompt for name/URL/model/key, create the agent
 *                                 via the agent.add route (server saves + vaults
 *                                 the key over its /v1 surface).
 *   codex-oauth / claude-oauth -> install the vendor CLI on aimee-server and run
 *                                 its OAuth login there via the agent.cli_oauth_*
 *                                 /v1 routes. */
#include "cli_agent_setup.h"
#include "cli_client.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <termios.h>
#endif

/* Poll loop bounds for the server-hosted OAuth login. */
#define OAUTH_POLL_INTERVAL_S 3
#define OAUTH_POLL_DEADLINE_S 600 /* hard wall-clock cap regardless of progress */
#define OAUTH_MAX_CONSEC_FAIL 5   /* bail if the server is unreachable this many polls in a row */

/* Best-effort scrub of a sensitive buffer (not optimized away). */
static void secure_zero(void *p, size_t n)
{
   volatile unsigned char *v = (volatile unsigned char *)p;
   while (n--)
      *v++ = 0;
}

/* Read one interactive line from stdin into buf (all trailing whitespace
 * stripped). When `secret` is set and stdin is a tty, terminal echo is
 * suppressed. Returns 1 if a non-empty value was read, 0 on empty/EOF; on an
 * over-long line it drains the rest, prints an error, and returns 0 so a
 * truncated value is never used. */
static int setup_prompt_ex(const char *label, char *buf, size_t n, int secret)
{
   fprintf(stderr, "%s", label);
   fflush(stderr);

#ifndef _WIN32
   struct termios old_t, new_t;
   int echo_off = 0;
   if (secret && isatty(fileno(stdin)) && tcgetattr(fileno(stdin), &old_t) == 0)
   {
      new_t = old_t;
      new_t.c_lflag &= ~(tcflag_t)ECHO;
      if (tcsetattr(fileno(stdin), TCSAFLUSH, &new_t) == 0)
         echo_off = 1;
   }
#else
   (void)secret;
#endif

   char *got = fgets(buf, (int)n, stdin);

#ifndef _WIN32
   if (echo_off)
   {
      if (tcsetattr(fileno(stdin), TCSAFLUSH, &old_t) != 0)
         fprintf(stderr, "\n(warning: could not restore terminal echo; run `stty echo` if later "
                         "prompts are invisible)");
      fprintf(stderr, "\n"); /* echo the newline the user could not see */
   }
#endif

   if (!got)
   {
      buf[0] = '\0';
      return 0;
   }
   size_t l = strlen(buf);
   if (l > 0 && buf[l - 1] != '\n' && !feof(stdin))
   {
      /* fgets filled the buffer before storing a newline. Peek the next byte: if
       * it is just the line terminator (the value fit exactly) consume it and
       * accept; otherwise the line is genuinely too long — drain and reject so a
       * truncated value is never used. */
      int c = getchar();
      if (c != '\n' && c != EOF)
      {
         while (c != '\n' && c != EOF)
            c = getchar();
         fprintf(stderr, "aimee agent setup: value too long (max %zu characters)\n", n - 1);
         secure_zero(buf, n);
         return 0;
      }
   }
   while (l > 0 && isspace((unsigned char)buf[l - 1]))
      buf[--l] = '\0';
   return buf[0] != '\0';
}

static int setup_prompt_line(const char *label, char *buf, size_t n)
{
   return setup_prompt_ex(label, buf, n, 0);
}

/* Resolve a server socket for `method` when no remote is configured. Returns 0 on
 * success (sets *sock, NULL in remote mode), 1 if no server is available. */
static int setup_ensure_server(const char *method, const char **sock)
{
   *sock = NULL;
   if (!cli_v1_has_remote_endpoint())
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
 * through the agent.add /v1 route so the server stores it and vaults the key. */
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
   /* A value beginning with '-' could be misread as an option by the server's
    * agent.add argument parser, so reject it up front. */
   if (name[0] == '-' || url[0] == '-' || model[0] == '-')
   {
      fprintf(stderr, "aimee agent setup: name, URL, and model cannot start with '-'\n");
      return 1;
   }
   int have_key = setup_prompt_ex("API key (leave blank for none): ", key, sizeof(key), 1);

   /* Build an `agent add` invocation and forward it through the normal /v1 route
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

   int rc;
   cli_v1_route_t route;
   if (!cli_v1_lookup("agent", n, sub, &route))
   {
      fprintf(stderr, "aimee agent setup: no agent.add route available\n");
      rc = 1;
      goto done;
   }
   const char *server_method = route.server_method ? route.server_method : route.method;
   const char *sock = NULL;
   if (setup_ensure_server(server_method, &sock) != 0)
   {
      rc = 1;
      goto done;
   }
   rc = cli_v1_forward(sock, &route, json_output, NULL, NULL, n, sub);
   if (rc < 0)
   {
      fprintf(stderr, "aimee agent setup: server request failed\n");
      rc = 1;
      goto done;
   }
   if (rc == 0 && !json_output)
      fprintf(stderr, "\nTest with: aimee agent test %s\n", name);

done:
   secure_zero(key, sizeof(key));
   return rc;
}

/* Dispatch one cli_oauth /v1 call. Returns the parsed response (caller deletes)
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

/* True for a non-terminal poll state (keep waiting). Anything else — including a
 * missing/unknown state — is treated as terminal so genuine failures are not
 * masked as "still pending". */
static int oauth_state_is_pending(const char *state)
{
   return state && (strcmp(state, "pending") == 0 || strcmp(state, "in_progress") == 0 ||
                    strcmp(state, "authorizing") == 0 || strcmp(state, "waiting") == 0);
}

/* codex-oauth / claude-oauth: install the vendor CLI on aimee-server and drive
 * its OAuth login (start -> optional code-back -> poll until authenticated). */
static int setup_oauth_cli_cmd(const char *vendor, int json_output)
{
   const char *sock = NULL;
   if (setup_ensure_server("agent.cli_oauth_start", &sock) != 0)
      return 1;

   fprintf(stderr, "\n=== %s server-hosted OAuth setup ===\n", vendor);
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
   {
      /* Fail loudly rather than silently truncate a session the server won't
       * then recognize on poll. */
      if (strlen(j_session->valuestring) >= sizeof(session))
      {
         fprintf(stderr, "aimee agent setup: server returned an oversized session id\n");
         cJSON_Delete(started);
         return 1;
      }
      snprintf(session, sizeof(session), "%s", j_session->valuestring);
   }
   int have_url = cJSON_IsString(j_url) && j_url->valuestring[0] != '\0';

   /* A successful-but-incomplete start would otherwise stall in the poll loop;
    * fail fast instead. A URL is required unless the flow asks for a code first. */
   if (session[0] == '\0' || (!needs_code_back && !have_url))
   {
      fprintf(stderr, "aimee agent setup: %s server-side setup returned an incomplete response\n",
              vendor);
      cJSON_Delete(started);
      return 1;
   }

   fprintf(stderr, "\n1. Open this URL in your browser and authorize:\n   %s\n",
           have_url ? j_url->valuestring : "(shown after you enter the code below)");
   if (cJSON_IsString(j_code) && j_code->valuestring[0])
      fprintf(stderr, "2. Enter this one-time code on that page: %s\n", j_code->valuestring);
   cJSON_Delete(started);

   /* From here on `session` (and any pasted code) are sensitive; route every exit
    * through `done:` so the session buffer is scrubbed. */
   int rc = 1;
   if (needs_code_back)
   {
      char code[600];
      if (!setup_prompt_ex("\nAfter authorizing, paste the code shown back here: ", code,
                           sizeof(code), 1))
      {
         secure_zero(code, sizeof(code));
         fprintf(stderr, "aimee agent setup: an authorization code is required\n");
         goto done;
      }
      cJSON *r = setup_oauth_rpc("agent.cli_oauth_code", vendor, session, code, 60000);
      secure_zero(code, sizeof(code));
      if (!r)
         goto done;
      cJSON_Delete(r);
   }

   fprintf(stderr, "\nWaiting for authentication");
   time_t deadline = time(NULL) + OAUTH_POLL_DEADLINE_S;
   int consec_fail = 0;
   while (time(NULL) < deadline)
   {
      fprintf(stderr, ".");
      fflush(stderr);
      sleep(OAUTH_POLL_INTERVAL_S);
      cJSON *pr = setup_oauth_rpc("agent.cli_oauth_poll", vendor, session, NULL, 60000);
      if (!pr)
      {
         /* Transient failure or unreachable server: bound the retries so a server
          * that died mid-login does not spin for the whole deadline. */
         if (++consec_fail >= OAUTH_MAX_CONSEC_FAIL)
         {
            fprintf(stderr,
                    "\naimee agent setup: lost contact with the server during %s "
                    "authentication\n",
                    vendor);
            goto done;
         }
         continue;
      }
      consec_fail = 0;
      cJSON *j_state = cJSON_GetObjectItemCaseSensitive(pr, "state");
      const char *state = cJSON_IsString(j_state) ? j_state->valuestring : NULL;
      if (state && strcmp(state, "authenticated") == 0)
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
            fprintf(stderr, "\n%s authenticated and registered as server-side agent '%s'.\n",
                    vendor, agent);
            fprintf(stderr, "Test with: aimee delegate review --via %s \"...\"\n", agent);
         }
         cJSON_Delete(pr);
         rc = 0;
         goto done;
      }
      if (!oauth_state_is_pending(state))
      {
         /* "failed", "expired", "denied", or any unknown/missing terminal state. */
         cJSON *j_err = cJSON_GetObjectItemCaseSensitive(pr, "error");
         fprintf(stderr, "\naimee agent setup: %s authentication %s%s%s\n", vendor,
                 state ? state : "failed", cJSON_IsString(j_err) ? ": " : "",
                 cJSON_IsString(j_err) ? j_err->valuestring : "");
         cJSON_Delete(pr);
         goto done;
      }
      cJSON_Delete(pr);
   }
   fprintf(stderr, "\nTimed out waiting for %s authentication.\n", vendor);

done:
   secure_zero(session, sizeof(session));
   return rc;
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
