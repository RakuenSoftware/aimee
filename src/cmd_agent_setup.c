/* cmd_agent_setup.c: provider setup wizards and token management */
#include "aimee.h"
#include "agent.h"
#include "agent_adapter.h"
#include "log.h"
#include "platform_path.h"
#include "agent_tunnel.h"
#include "commands.h"
#include "cJSON.h"
#include "aimee_client.h" /* aimee_client_request: drive server-side OAuth-CLI setup */
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- agent subcommand handlers --- */

/* OAuth token-refresh endpoints (used by `aimee agent token`). */
#define CODEX_OAUTH_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_TOKEN_URL       "https://auth.openai.com/oauth/token"
#define GOOGLE_TOKEN_URL      "https://oauth2.googleapis.com/token"

/* Agent config reference (defined in cmd_agent.c, loaded before dispatch) */
extern agent_config_t s_agent_cfg;

static void write_key_file(const char *path, const char *content)
{
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", config_default_dir());
   platform_mkdir_p(dir, 0700);

   FILE *f = fopen(path, "w");
   if (!f)
      fatal("cannot write key file: %s", path);
   fputs(content, f);
   fputc('\n', f);
   fclose(f);
   chmod(path, 0600);
}

/* Open /dev/tty for interactive input so setup wizards work even when stdin
 * is /dev/null (e.g. forwarded through the aimee daemon). Falls back to
 * stdin if /dev/tty cannot be opened (e.g. non-interactive test runs). */
static FILE *tty_input(void)
{
   static FILE *tty = NULL;
   static int tried = 0;
   if (!tried)
   {
      tried = 1;
      tty = fopen("/dev/tty", "r");
   }
   return tty ? tty : stdin;
}

static char *read_file_contents(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 64 * 1024)
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t nr = fread(buf, 1, (size_t)sz, f);
   buf[nr] = '\0';
   fclose(f);
   return buf;
}

/* --- agent setup: direct HTTP API providers (openai / anthropic) --- */

static void read_line(const char *prompt, char *buf, size_t buf_len, int required)
{
   fprintf(stderr, "%s", prompt);
   if (!fgets(buf, (int)buf_len, tty_input()))
   {
      if (required)
         fatal("failed to read input");
      buf[0] = '\0';
      return;
   }
   size_t len = strlen(buf);
   while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
      buf[--len] = '\0';
   if (required && len == 0)
      fatal("value cannot be empty");
}

/* Shared name + URL + token wizard for direct HTTP API providers. `provider` is
 * the wire adapter ("openai" or "anthropic"); `intro`, `endpoint_prompt`, and
 * `model_prompt` tailor the prompts. Adding another first-class provider later is
 * a new thin wrapper plus a setup_providers[] row. */
static void setup_api_provider(agent_config_t *cfg, const char *provider, const char *intro,
                               const char *endpoint_prompt, const char *model_prompt)
{
   fprintf(stderr, "%s", intro);

   char name_buf[MAX_AGENT_NAME];
   read_line("Agent name: ", name_buf, sizeof(name_buf), 1);

   char endpoint_buf[MAX_ENDPOINT_LEN];
   read_line(endpoint_prompt, endpoint_buf, sizeof(endpoint_buf), 1);

   char model_buf[MAX_MODEL_LEN];
   read_line(model_prompt, model_buf, sizeof(model_buf), 1);

   char key_buf[MAX_API_KEY_LEN];
   read_line("API key (leave blank for no auth): ", key_buf, sizeof(key_buf), 0);

   char fallback_buf[MAX_MODEL_LEN];
   read_line("Fallback model (leave blank to skip): ", fallback_buf, sizeof(fallback_buf), 0);

   char roles_buf[256];
   read_line(
       "Roles (comma-separated, default: summarize,format,draft,review,explain,code,execute): ",
       roles_buf, sizeof(roles_buf), 0);

   char tier_buf[16];
   read_line("Cost tier (0=free, 1=cheap, 2=expensive, default: 1): ", tier_buf, sizeof(tier_buf),
             0);

   /* Save key if provided */
   char auth_cmd[MAX_AUTH_CMD_LEN] = {0};
   int has_key = (key_buf[0] != '\0');
   if (has_key)
   {
      char key_path[MAX_PATH_LEN];
      snprintf(key_path, sizeof(key_path), "%s/%s.key", config_default_dir(), name_buf);
      write_key_file(key_path, key_buf);
      snprintf(auth_cmd, sizeof(auth_cmd), "cat %s/%s.key", config_default_dir(), name_buf);
   }

   agent_t *ag = agent_find(cfg, name_buf);
   int is_new = 0;
   if (!ag)
   {
      if (cfg->agent_count >= MAX_AGENTS)
         fatal("maximum number of agents reached");
      ag = &cfg->agents[cfg->agent_count];
      memset(ag, 0, sizeof(*ag));
      cfg->agent_count++;
      is_new = 1;
   }

   snprintf(ag->name, MAX_AGENT_NAME, "%s", name_buf);
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "%s", endpoint_buf);
   snprintf(ag->model, MAX_MODEL_LEN, "%s", model_buf);
   if (fallback_buf[0])
      snprintf(ag->fallback_model, MAX_MODEL_LEN, "%s", fallback_buf);
   else
      ag->fallback_model[0] = '\0';

   if (has_key)
   {
      snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", auth_cmd);
      snprintf(ag->auth_type, sizeof(ag->auth_type), "oauth");
   }
   else
   {
      ag->auth_cmd[0] = '\0';
      snprintf(ag->auth_type, sizeof(ag->auth_type), "none");
   }

   snprintf(ag->provider, sizeof(ag->provider), "%s", provider);
   ag->api_key[0] = '\0';
   ag->api_key_disk[0] = '\0';
   ag->extra_headers[0] = '\0';
   ag->cost_tier = (tier_buf[0] >= '0' && tier_buf[0] <= '9') ? atoi(tier_buf) : 1;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = 60000;
   ag->enabled = 1;
   ag->tools_enabled = 1;
   ag->max_turns = -1; /* inherit from global config max_iterations_delegate */
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

   /* Parse roles */
   ag->role_count = 0;
   if (roles_buf[0])
   {
      char roles_copy[256];
      snprintf(roles_copy, sizeof(roles_copy), "%s", roles_buf);
      char *tok = strtok(roles_copy, ",");
      while (tok && ag->role_count < MAX_AGENT_ROLES)
      {
         while (*tok == ' ')
            tok++;
         snprintf(ag->roles[ag->role_count++], 32, "%s", tok);
         tok = strtok(NULL, ",");
      }
   }
   else
   {
      const char *defaults[] = {"summarize", "format", "draft",  "review",
                                "explain",   "code",   "execute"};
      for (int i = 0; i < 7; i++)
         snprintf(ag->roles[ag->role_count++], 32, "%s", defaults[i]);
   }

   if (cfg->agent_count == 1 || !cfg->default_agent[0])
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "%s", name_buf);

   agent_save_config(cfg);
   fprintf(stderr, "\nAgent '%s' %s.\n", name_buf, is_new ? "created" : "updated");
   if (has_key)
      fprintf(stderr, "Key saved to: %s/%s.key\n", config_default_dir(), name_buf);
   fprintf(stderr, "\nTest with: aimee agent test %s\n", name_buf);
}

static void setup_openai(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;
   setup_api_provider(
       cfg, "openai",
       "\n=== OpenAI-Compatible Agent Setup ===\n\n"
       "Configure any provider with an OpenAI-compatible base URL.\n"
       "Examples: https://api.openai.com/v1, https://api.groq.com/openai/v1,\n"
       "          http://192.168.1.122:8080/v1\n\n",
       "Endpoint URL (e.g. https://api.openai.com/v1): ", "Model name (e.g. gpt-5.5): ");
}

static void setup_anthropic(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;
   setup_api_provider(cfg, "anthropic",
                      "\n=== Anthropic API Agent Setup ===\n\n"
                      "Configure an Anthropic (Claude) provider with an API key.\n"
                      "Examples: https://api.anthropic.com/v1 (default Anthropic endpoint)\n\n",
                      "Endpoint URL (e.g. https://api.anthropic.com/v1): ",
                      "Model name (e.g. claude-sonnet-4-6): ");
}

/* --- server-hosted OAuth CLI agents (claude-oauth / codex-oauth) --- */

/* POST a {vendor,...} body to a /v1/agent/cli_oauth_* route; returns the parsed
 * JSON response (caller cJSON_Delete) or NULL on transport/HTTP error. */
static cJSON *cli_oauth_post(const char *path, cJSON *body)
{
   char *bs = cJSON_PrintUnformatted(body);
   if (!bs)
      return NULL;
   int st = 0;
   char *resp = aimee_client_request("POST", path, bs, &st);
   free(bs);
   if (!resp || st < 200 || st >= 300)
   {
      cJSON *e = (resp && resp[0]) ? cJSON_Parse(resp) : NULL;
      const char *msg = NULL;
      if (e)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "error");
         if (cJSON_IsString(m))
            msg = m->valuestring;
      }
      fprintf(stderr, "server error (%d)%s%s\n", st, msg ? ": " : "", msg ? msg : "");
      cJSON_Delete(e);
      free(resp);
      return NULL;
   }
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   return j;
}

static const char *json_str(cJSON *o, const char *key)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
   return cJSON_IsString(v) ? v->valuestring : "";
}

/* Drive the server-side install + OAuth login for `vendor` to completion. */
static void setup_server_oauth_cli(const char *vendor)
{
   fprintf(stderr, "\n=== %s — server-hosted OAuth setup ===\n", vendor);
   fprintf(stderr, "Installing the %s CLI on aimee-server and starting its login...\n", vendor);

   cJSON *start_body = cJSON_CreateObject();
   cJSON_AddStringToObject(start_body, "vendor", vendor);
   cJSON *started = cli_oauth_post("/v1/agent/cli_oauth_start", start_body);
   cJSON_Delete(start_body);
   if (!started)
   {
      fprintf(stderr, "Could not start %s setup (is the server reachable?).\n", vendor);
      return;
   }
   char session[128];
   snprintf(session, sizeof(session), "%s", json_str(started, "session"));
   const char *url = json_str(started, "url");
   const char *code = json_str(started, "code");
   int needs_code_back = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(started, "needs_code_back"));

   fprintf(stderr, "\n1. Open this URL in your browser and authorize:\n   %s\n", url);
   if (code[0])
      fprintf(stderr, "2. Enter this one-time code on that page: %s\n", code);
   cJSON_Delete(started);

   if (needs_code_back)
   {
      fprintf(stderr, "\nAfter authorizing, paste the code shown back here: ");
      fflush(stderr);
      char line[600];
      if (fgets(line, sizeof(line), stdin))
      {
         line[strcspn(line, "\r\n")] = '\0';
         cJSON *cb = cJSON_CreateObject();
         cJSON_AddStringToObject(cb, "vendor", vendor);
         cJSON_AddStringToObject(cb, "session", session);
         cJSON_AddStringToObject(cb, "code", line);
         cJSON *r = cli_oauth_post("/v1/agent/cli_oauth_code", cb);
         cJSON_Delete(cb);
         if (!r)
         {
            fprintf(stderr, "Failed to submit the code.\n");
            return;
         }
         cJSON_Delete(r);
      }
   }

   fprintf(stderr, "\nWaiting for authentication");
   for (int i = 0; i < 100; i++)
   {
      fprintf(stderr, ".");
      fflush(stderr);
      sleep(3);
      cJSON *pb = cJSON_CreateObject();
      cJSON_AddStringToObject(pb, "vendor", vendor);
      cJSON_AddStringToObject(pb, "session", session);
      cJSON *pr = cli_oauth_post("/v1/agent/cli_oauth_poll", pb);
      cJSON_Delete(pb);
      if (!pr)
         continue;
      const char *state = json_str(pr, "state");
      if (strcmp(state, "authenticated") == 0)
      {
         fprintf(stderr, "\n✓ %s authenticated and registered as a server-side agent '%s'.\n",
                 vendor, json_str(pr, "agent"));
         fprintf(stderr, "Test with: aimee delegate review --persona reviewer --via %s \"...\"\n",
                 json_str(pr, "agent"));
         cJSON_Delete(pr);
         return;
      }
      if (strcmp(state, "failed") == 0)
      {
         fprintf(stderr, "\n%s login failed: %s\n", vendor, json_str(pr, "error"));
         cJSON_Delete(pr);
         return;
      }
      cJSON_Delete(pr);
   }
   fprintf(stderr, "\nTimed out waiting for %s authentication.\n", vendor);
}

static void setup_claude_oauth(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;
   (void)cfg;
   setup_server_oauth_cli("claude");
}
static void setup_codex_oauth_server(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;
   (void)cfg;
   setup_server_oauth_cli("codex");
}

/* --- agent setup dispatch --- */

typedef struct
{
   const char *name;
   void (*fn)(app_ctx_t *ctx, agent_config_t *cfg);
} setup_provider_t;

/* The supported provider set. `openai`/`anthropic` are direct HTTP API providers
 * (prompt for name + URL + token); `codex-oauth`/`claude-oauth` install the CLI
 * on aimee-server and run the OAuth flow there. More providers can be added later
 * as a new wrapper + a row here. */
static const setup_provider_t setup_providers[] = {
    {"openai", setup_openai},
    {"anthropic", setup_anthropic},
    {"codex-oauth", setup_codex_oauth_server},
    {"claude-oauth", setup_claude_oauth},
    {NULL, NULL},
};

void ag_setup(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee agent setup <provider>\n");
      fprintf(stderr, "Providers: openai, anthropic, codex-oauth, claude-oauth\n");
      exit(1);
   }
   const char *provider = argv[0];
   for (int i = 0; setup_providers[i].name; i++)
   {
      if (strcmp(provider, setup_providers[i].name) == 0)
      {
         setup_providers[i].fn(ctx, &s_agent_cfg);
         return;
      }
   }
   fatal("unknown setup provider: %s "
         "(available: openai, anthropic, codex-oauth, claude-oauth)",
         provider);
}

/* --- agent token (print/refresh OAuth token) --- */

void ag_token(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1)
      fatal("usage: aimee agent token <name>");

   const char *name = argv[0];

   int is_gemini = (strcmp(name, "gemini") == 0);
   int is_codex = (strcmp(name, "codex") == 0);

   if (!is_codex && !is_gemini)
      fatal("token refresh supported for 'codex' and 'gemini'");

   char auth_path[MAX_PATH_LEN];
   if (is_gemini)
      snprintf(auth_path, sizeof(auth_path), "%s/gemini-auth.json", config_default_dir());
   else
      snprintf(auth_path, sizeof(auth_path), "%s/codex-auth.json", config_default_dir());

   char *data = read_file_contents(auth_path);
   if (!data)
      fatal("no auth file found at %s (run: aimee agent setup %s)", auth_path,
            is_gemini ? "gemini-oauth" : "codex-oauth");

   cJSON *root = cJSON_Parse(data);
   free(data);
   if (!root)
      fatal("failed to parse %s", auth_path);

   cJSON *j_access = cJSON_GetObjectItem(root, "access_token");
   cJSON *j_refresh = cJSON_GetObjectItem(root, "refresh_token");
   cJSON *j_expires_at = cJSON_GetObjectItem(root, "expires_at");
   cJSON *j_client_id = cJSON_GetObjectItem(root, "client_id");

   if (!j_access || !cJSON_IsString(j_access))
   {
      cJSON_Delete(root);
      fatal("auth file missing access_token");
   }

   time_t now = time(NULL);
   double expires_at =
       (j_expires_at && cJSON_IsNumber(j_expires_at)) ? j_expires_at->valuedouble : 0;

   /* If token is still fresh (more than 5 minutes remaining), print it */
   if ((double)now + 300 < expires_at)
   {
      printf("%s", j_access->valuestring);
      cJSON_Delete(root);
      return;
   }

   /* Token expired or expiring, try refresh */
   if (!j_refresh || !cJSON_IsString(j_refresh))
   {
      /* No refresh token, print what we have */
      printf("%s", j_access->valuestring);
      cJSON_Delete(root);
      return;
   }

   const char *client_id = (j_client_id && cJSON_IsString(j_client_id)) ? j_client_id->valuestring
                                                                        : CODEX_OAUTH_CLIENT_ID;
   const char *token_url = is_gemini ? GOOGLE_TOKEN_URL : CODEX_TOKEN_URL;

   agent_http_init();

   char post_body[MAX_API_KEY_LEN + 256];
   snprintf(post_body, sizeof(post_body), "grant_type=refresh_token&client_id=%s&refresh_token=%s",
            client_id, j_refresh->valuestring);

   char *resp_body = NULL;
   int status = agent_http_post_form(token_url, post_body, &resp_body, 15000);

   if (status != 200 || !resp_body)
   {
      agent_http_cleanup();
      /* Fall back to current token */
      printf("%s", j_access->valuestring);
      cJSON_Delete(root);
      free(resp_body);
      return;
   }

   cJSON *tok = cJSON_Parse(resp_body);
   free(resp_body);

   if (!tok)
   {
      agent_http_cleanup();
      printf("%s", j_access->valuestring);
      cJSON_Delete(root);
      return;
   }

   cJSON *new_id_token = cJSON_GetObjectItem(tok, "id_token");
   cJSON *new_access = cJSON_GetObjectItem(tok, "access_token");
   cJSON *new_refresh = cJSON_GetObjectItem(tok, "refresh_token");
   cJSON *new_tok_expires = cJSON_GetObjectItem(tok, "expires_in");

   /* Try to exchange id_token for API key (Codex-specific, best-effort) */
   const char *final_token = NULL;
   int new_exp_secs = 3600;
   cJSON *exch = NULL;

   if (!is_gemini && new_id_token && cJSON_IsString(new_id_token))
   {
      char exchange_body[4096];
      snprintf(exchange_body, sizeof(exchange_body),
               "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Atoken-exchange"
               "&client_id=%s"
               "&requested_token=openai-api-key"
               "&subject_token=%s"
               "&subject_token_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Atoken-type%%3Aid_token",
               client_id, new_id_token->valuestring);

      char *exch_resp = NULL;
      int exch_status = agent_http_post_form(CODEX_TOKEN_URL, exchange_body, &exch_resp, 15000);

      if (exch_status == 200 && exch_resp)
      {
         exch = cJSON_Parse(exch_resp);
         if (exch)
         {
            cJSON *new_api_key = cJSON_GetObjectItem(exch, "access_token");
            cJSON *new_expires = cJSON_GetObjectItem(exch, "expires_in");
            if (new_api_key && cJSON_IsString(new_api_key))
            {
               final_token = new_api_key->valuestring;
               if (new_expires && cJSON_IsNumber(new_expires))
                  new_exp_secs = new_expires->valueint;
            }
         }
      }
      if (!final_token)
      {
         LOG_WARN("agent_setup", "API key exchange failed (HTTP %d)%s%s", exch_status,
                  exch_resp ? ": " : "", exch_resp ? exch_resp : "");
      }
      free(exch_resp);
   }

   agent_http_cleanup();

   /* Fall back to refreshed access_token, then to original token */
   if (!final_token && new_access && cJSON_IsString(new_access))
   {
      final_token = new_access->valuestring;
      if (new_tok_expires && cJSON_IsNumber(new_tok_expires))
         new_exp_secs = new_tok_expires->valueint;
   }
   if (!final_token)
   {
      printf("%s", j_access->valuestring);
      cJSON_Delete(exch);
      cJSON_Delete(tok);
      cJSON_Delete(root);
      return;
   }

   /* Update the auth file */
   cJSON *updated = cJSON_CreateObject();
   cJSON_AddStringToObject(updated, "access_token", final_token);
   if (new_refresh && cJSON_IsString(new_refresh))
      cJSON_AddStringToObject(updated, "refresh_token", new_refresh->valuestring);
   else if (j_refresh && cJSON_IsString(j_refresh))
      cJSON_AddStringToObject(updated, "refresh_token", j_refresh->valuestring);
   if (new_id_token && cJSON_IsString(new_id_token))
      cJSON_AddStringToObject(updated, "id_token", new_id_token->valuestring);
   cJSON_AddNumberToObject(updated, "expires_at", (double)(now + new_exp_secs));
   cJSON_AddStringToObject(updated, "client_id", client_id);

   /* Print token before freeing the cJSON objects that own the string */
   printf("%s", final_token);

   char *updated_str = cJSON_Print(updated);
   cJSON_Delete(updated);
   cJSON_Delete(exch);
   cJSON_Delete(tok);
   cJSON_Delete(root);

   if (updated_str)
   {
      write_key_file(auth_path, updated_str);
      free(updated_str);
   }
}

/* --- tunnel status --- */

void ag_tunnel(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   agent_tunnel_print_status(&s_agent_cfg.tunnel_mgr, ctx->json_output);
}
