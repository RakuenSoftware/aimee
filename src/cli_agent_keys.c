/* cli_agent_keys.c: see cli_agent_keys.h. */
#include "cli_agent_keys.h"
#include "cli_client.h" /* cli_http_request, cli_rpc_* */
#include "codex_auth.h" /* codex_local_auth */
#include "aimee_home.h" /* aimee_home */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define AGENT_KEYS_FILE   "agent-keys.json"
#define CRED_SESSION_FILE "cred-session.id"
#define CRED_PUSH_MARKER  ".creds-pushed"
/* Don't re-push within this window (one-shot CLI invocations share the keyring
 * via the marker file; a server restart re-syncs after at most this long). */
#define CRED_PUSH_MIN_INTERVAL_SECS 120

static int aimee_file_path(const char *name, char *out, size_t out_len)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   snprintf(out, out_len, "%s/%s", home, name);
   return 0;
}

static char *read_text(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > (1 << 20))
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
   size_t got = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[got] = '\0';
   return buf;
}

/* Load the local keyring object {name:key}; always returns an object. */
static cJSON *load_keys(void)
{
   char path[1024];
   if (aimee_file_path(AGENT_KEYS_FILE, path, sizeof(path)) != 0)
      return cJSON_CreateObject();
   char *buf = read_text(path);
   cJSON *root = buf ? cJSON_Parse(buf) : NULL;
   free(buf);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      root = cJSON_CreateObject();
   }
   return root;
}

cJSON *cli_agent_keys_load(void)
{
   return load_keys();
}

int cli_agent_key_set(const char *agent_name, const char *api_key)
{
   if (!agent_name || !agent_name[0])
      return -1;
   char path[1024];
   if (aimee_file_path(AGENT_KEYS_FILE, path, sizeof(path)) != 0)
      return -1;

   cJSON *root = load_keys();
   cJSON_DeleteItemFromObjectCaseSensitive(root, agent_name);
   if (api_key && api_key[0])
      cJSON_AddStringToObject(root, agent_name, api_key);

   char *json = cJSON_Print(root);
   cJSON_Delete(root);
   if (!json)
      return -1;
   FILE *f = fopen(path, "wb");
   if (!f)
   {
      free(json);
      return -1;
   }
   fputs(json, f);
   fclose(f);
   chmod(path, 0600);
   free(json);
   return 0;
}

int cli_agent_add_localize_key(int *argc, char **argv)
{
   if (!argc || !argv || *argc < 2)
      return 0;
   const char *name = argv[1]; /* argv[0] == "add" */
   if (!name || !name[0])
      return 0;
   int ki = -1;
   for (int i = 2; i + 1 < *argc; i++)
      if (strcmp(argv[i], "--key") == 0)
      {
         ki = i;
         break;
      }
   if (ki < 0)
      return 0;
   if (cli_agent_key_set(name, argv[ki + 1]) != 0)
      return 0;
   /* Drop argv[ki] and argv[ki+1] by shifting the tail left. */
   for (int j = ki; j + 2 < *argc; j++)
      argv[j] = argv[j + 2];
   *argc -= 2;
   return 1;
}

int cli_cred_session_id(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   out[0] = '\0';
   char path[1024];
   if (aimee_file_path(CRED_SESSION_FILE, path, sizeof(path)) != 0)
      return -1;

   char *existing = read_text(path);
   if (existing)
   {
      size_t n = strcspn(existing, "\r\n");
      if (n > 0 && n < out_len)
      {
         memcpy(out, existing, n);
         out[n] = '\0';
      }
      free(existing);
      if (out[0])
         return 0;
   }

   /* Generate a random id from /dev/urandom (hex). */
   unsigned char rnd[16] = {0};
   FILE *ur = fopen("/dev/urandom", "rb");
   if (ur)
   {
      if (fread(rnd, 1, sizeof(rnd), ur) != sizeof(rnd))
         rnd[0] |= 1;
      fclose(ur);
   }
   char id[64];
   int p = snprintf(id, sizeof(id), "cs-");
   for (size_t i = 0; i < sizeof(rnd) && p < (int)sizeof(id) - 2; i++)
      p += snprintf(id + p, sizeof(id) - (size_t)p, "%02x", rnd[i]);
   FILE *f = fopen(path, "wb");
   if (f)
   {
      fputs(id, f);
      fclose(f);
      chmod(path, 0600);
   }
   snprintf(out, out_len, "%s", id);
   return 0;
}

/* True if we pushed within CRED_PUSH_MIN_INTERVAL_SECS (marker mtime). */
static int recently_pushed(void)
{
   char path[1024];
   if (aimee_file_path(CRED_PUSH_MARKER, path, sizeof(path)) != 0)
      return 0;
   struct stat st;
   if (stat(path, &st) != 0)
      return 0;
   return (time(NULL) - st.st_mtime) < CRED_PUSH_MIN_INTERVAL_SECS;
}

static void touch_push_marker(void)
{
   char path[1024];
   if (aimee_file_path(CRED_PUSH_MARKER, path, sizeof(path)) != 0)
      return;
   FILE *f = fopen(path, "wb");
   if (f)
      fclose(f);
}

void cli_session_creds_prime(cJSON *req)
{
   if (!req || !cli_rpc_remote_endpoint_is_tcp())
      return;
   char sid[128];
   if (cli_cred_session_id(sid, sizeof(sid)) != 0 || !sid[0])
      return;
   cJSON_AddStringToObject(req, "cred_session_id", sid);

   if (recently_pushed())
      return; /* server already has this session's keyring (deduped) */

   /* Build the push body from the local keyring + Codex creds. */
   cJSON *agents = load_keys();
   char token[8192];
   char account[256];
   int have_codex = (codex_local_auth(token, sizeof(token), account, sizeof(account)) == 0);
   if (cJSON_GetArraySize(agents) == 0 && !have_codex)
   {
      cJSON_Delete(agents);
      return; /* nothing to push */
   }

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "session_id", sid);
   cJSON_AddItemToObject(body, "agents", agents); /* adopt */
   if (have_codex)
   {
      cJSON_AddStringToObject(body, "codex_oauth_token", token);
      if (account[0])
         cJSON_AddStringToObject(body, "codex_account_id", account);
   }
   char *body_json = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!body_json)
      return;

   char *endpoint = cli_rpc_client_endpoint();
   char *bearer = cli_rpc_client_bearer();
   int status = 0;
   cJSON *resp = cli_http_request(endpoint, "POST", "/v1/session/credentials", body_json, bearer,
                                  30000, &status);
   free(body_json);
   free(endpoint);
   free(bearer);
   if (resp && status >= 200 && status < 300)
      touch_push_marker();
   if (resp)
      cJSON_Delete(resp);
}

/* `aimee agent key import [--scrub]` (P3): migrate client-held agent-keys.json
 * entries into the server vault under the server principal (vault.set_server).
 * Reports per agent; --scrub removes an entry only after a confirmed vault store.
 * Idempotent (re-runnable). Over a plaintext-TCP remote the server refuses
 * (P2a/D2b) and the entry is left intact — provision over an attested
 * (UDS/webchat) connection holding the vault:write:server capability. */
int cli_agent_key_import(int argc, char **argv, int json_output)
{
   (void)json_output;
   int scrub = 0, dry = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--scrub") == 0)
         scrub = 1;
      else if (strcmp(argv[i], "--dry-run") == 0)
         dry = 1;
   }

   cJSON *keys = cli_agent_keys_load();
   int total = 0, vaulted = 0, refused = 0, errors = 0;
   cJSON *entry = NULL;
   cJSON_ArrayForEach(entry, keys)
   {
      const char *agent = entry->string;
      if (!agent || !agent[0] || !cJSON_IsString(entry) || !entry->valuestring[0])
         continue;
      total++;
      if (dry)
      {
         /* Preview only — never send the secret or mutate the keyring. */
         printf("  %-16s would vault (dry-run)\n", agent);
         continue;
      }
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "method", "vault.set_server");
      cJSON_AddStringToObject(req, "agent", agent);
      cJSON_AddStringToObject(req, "cred", "api_key");
      cJSON_AddStringToObject(req, "secret", entry->valuestring);
      cJSON *resp = cli_v1_dispatch(req, 30000);
      cJSON_Delete(req);

      cJSON *st = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
      int ok = st && cJSON_IsString(st) && strcmp(st->valuestring, "ok") == 0;
      if (ok)
      {
         vaulted++;
         printf("  %-16s vaulted\n", agent);
         if (scrub)
            cli_agent_key_set(agent, NULL);
      }
      else
      {
         cJSON *msg = resp ? cJSON_GetObjectItemCaseSensitive(resp, "message") : NULL;
         const char *m = (msg && cJSON_IsString(msg)) ? msg->valuestring : "no server response";
         if (strstr(m, "attested") || strstr(m, "capability"))
            refused++;
         else
            errors++;
         printf("  %-16s SKIPPED (%s)\n", agent, m);
      }
      cJSON_Delete(resp);
   }
   cJSON_Delete(keys);

   if (dry)
   {
      printf("agent key import (dry-run): %d entr%s would be sent; nothing changed\n", total,
             total == 1 ? "y" : "ies");
      return 0;
   }
   printf("agent key import: %d vaulted, %d refused, %d error (of %d)%s\n", vaulted, refused,
          errors, total, scrub ? "; migrated entries scrubbed from the local keyring" : "");
   if (refused > 0)
      printf("note: server-principal writes need an attested (UDS/webchat) connection holding the "
             "vault:write:server capability — run this on the server host over UDS, or grant the "
             "capability.\n");
   return errors > 0 ? 1 : 0;
}
