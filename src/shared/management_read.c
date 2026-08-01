#include "management_read.h"
#include "cJSON.h"

#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   unsigned char bytes[2048];
   size_t n;
} transcript_t;

static int identifier(const char *s, size_t max)
{
   size_t n = s ? strnlen(s, max + 1) : 0;
   if (!n || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-'))
         return 0;
   }
   return 1;
}

static int model_identifier(const char *s)
{
   size_t n = s ? strnlen(s, 128) : 0;
   if (!n || n > 127)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == ':' || c == '/' || c == '+' || c == '-'))
         return 0;
   }
   return 1;
}

static int utf8_valid(const unsigned char *s, size_t n)
{
   for (size_t i = 0; i < n;)
   {
      unsigned c = s[i++];
      if (c < 0x80)
         continue;
      unsigned need = c >= 0xc2 && c <= 0xdf   ? 1
                      : c >= 0xe0 && c <= 0xef ? 2
                      : c >= 0xf0 && c <= 0xf4 ? 3
                                               : 99;
      if (need == 99 || need > n - i)
         return 0;
      if ((s[i] & 0xc0) != 0x80 || (need >= 2 && (s[i + 1] & 0xc0) != 0x80) ||
          (need == 3 && (s[i + 2] & 0xc0) != 0x80))
         return 0;
      if ((c == 0xe0 && s[i] < 0xa0) || (c == 0xed && s[i] >= 0xa0) || (c == 0xf0 && s[i] < 0x90) ||
          (c == 0xf4 && s[i] >= 0x90))
         return 0;
      i += need;
   }
   return 1;
}

static int canonical_text(const char *s, size_t max)
{
   size_t n = s ? strnlen(s, max + 1) : 0;
   if (!n || n > max || n > UINT16_MAX || !utf8_valid((const unsigned char *)s, n))
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c == 0 || c < 0x20 || c == 0x7f)
         return 0;
   }
   return 1;
}

static int lower_hex(const char *s, size_t max)
{
   if (!canonical_text(s, max))
      return 0;
   for (const char *p = s; *p; ++p)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
         return 0;
   return 1;
}

static int put(transcript_t *t, const void *p, size_t n)
{
   if (!t || !p || n > sizeof(t->bytes) - t->n)
      return 0;
   memcpy(t->bytes + t->n, p, n);
   t->n += n;
   return 1;
}

static int put_u16_text(transcript_t *t, const char *s)
{
   size_t n = strlen(s);
   unsigned char len[2] = {(unsigned char)(n >> 8), (unsigned char)n};
   return n <= UINT16_MAX && put(t, len, sizeof(len)) && put(t, s, n);
}

static int put_u64(transcript_t *t, uint64_t v)
{
   unsigned char b[8];
   for (size_t i = 0; i < sizeof(b); ++i)
      b[sizeof(b) - 1 - i] = (unsigned char)(v >> (i * 8));
   return put(t, b, sizeof(b));
}

const char *server_mgmt_read_selector_name(server_mgmt_read_selector_t selector)
{
   return selector == SERVER_MGMT_READ_SELECTOR_AGENTS   ? "agents"
          : selector == SERVER_MGMT_READ_SELECTOR_CONFIG ? "config"
                                                         : NULL;
}

const char *server_mgmt_read_selector_purpose(server_mgmt_read_selector_t selector)
{
   return selector == SERVER_MGMT_READ_SELECTOR_AGENTS   ? "management.read.v1"
          : selector == SERVER_MGMT_READ_SELECTOR_CONFIG ? "management.read.config.v1"
                                                         : NULL;
}

int server_mgmt_read_selector_path(server_mgmt_read_selector_t selector, const char *server_id,
                                   char *out, size_t cap)
{
   const char *name = server_mgmt_read_selector_name(selector);
   if (out && cap)
      out[0] = 0;
   if (!name || !out || !cap || !identifier(server_id, 127))
      return -1;
   int n = snprintf(out, cap, "/v1/servers/%s/%s", server_id, name);
   return n > 0 && (size_t)n < cap ? n : -1;
}

int server_mgmt_read_digest(const server_mgmt_read_digest_input_t *in, char out[65])
{
   static const unsigned char domain[] = "aimee-mgmt-read-v1";
   transcript_t t = {{0}, 0};
   char path[160];
   const char *selector = in ? server_mgmt_read_selector_name(in->selector) : NULL;
   if (out)
      out[0] = 0;
   if (!in || !out || !selector || !in->nonce || !identifier(in->server_id, 127) ||
       in->team_id <= 0 || !canonical_text(in->kb_issuer, 511) || !lower_hex(in->kb_serial, 79) ||
       !canonical_text(in->server_issuer, 511) || !lower_hex(in->server_serial, 79))
      return -1;
   if (server_mgmt_read_selector_path(in->selector, in->server_id, path, sizeof(path)) < 0 ||
       !put(&t, domain, sizeof(domain)) || /* includes the required NUL */
       !put_u16_text(&t, "GET") || !put_u16_text(&t, path) || !put_u16_text(&t, selector) ||
       !put_u16_text(&t, in->server_id) || !put_u64(&t, (uint64_t)in->team_id) ||
       !put(&t, in->nonce, 32) || !put_u16_text(&t, in->kb_issuer) ||
       !put_u16_text(&t, in->kb_serial) || !put_u16_text(&t, in->server_issuer) ||
       !put_u16_text(&t, in->server_serial) || !put_u64(&t, in->revocation_generation) ||
       !put_u64(&t, in->publication_generation))
      return -1;
   unsigned char digest[SHA256_DIGEST_LENGTH];
   if (!SHA256(t.bytes, t.n, digest))
      return -1;
   for (size_t i = 0; i < sizeof(digest); ++i)
      snprintf(out + i * 2, 3, "%02x", digest[i]);
   return 0;
}

static int agent_compare(const void *a, const void *b)
{
   const server_mgmt_read_agent_t *aa = a, *bb = b;
   return strcmp(aa->name, bb->name);
}

static int valid_agent(const server_mgmt_read_agent_t *a)
{
   return a && identifier(a->name, 63) && identifier(a->provider, 15) &&
          model_identifier(a->model) && (a->enabled == 0 || a->enabled == 1) &&
          (a->delegate_available == 0 || a->delegate_available == 1) &&
          (a->primary_only == 0 || a->primary_only == 1) && a->max_parallel >= 0 &&
          a->max_parallel <= 1024;
}

int server_mgmt_read_project(const char *server_id, int64_t team_id,
                             const server_mgmt_read_agent_t *agents, size_t count, char *out,
                             size_t cap)
{
   if (out && cap)
      out[0] = 0;
   if (!out || !cap || !identifier(server_id, 127) || team_id <= 0 ||
       count > SERVER_MGMT_READ_AGENT_MAX || (count && !agents))
      return -1;
   server_mgmt_read_agent_t sorted[SERVER_MGMT_READ_AGENT_MAX];
   memset(sorted, 0, sizeof(sorted));
   for (size_t i = 0; i < count; ++i)
   {
      if (!valid_agent(&agents[i]))
         return -1;
      sorted[i] = agents[i];
   }
   qsort(sorted, count, sizeof(sorted[0]), agent_compare);
   for (size_t i = 1; i < count; ++i)
      if (!strcmp(sorted[i - 1].name, sorted[i].name))
         return -1;

   cJSON *root = cJSON_CreateObject();
   cJSON *items = root ? cJSON_CreateArray() : NULL;
   char team[32];
   int team_n = snprintf(team, sizeof(team), "%lld", (long long)team_id);
   cJSON *team_raw = team_n > 0 && (size_t)team_n < sizeof(team) ? cJSON_CreateRaw(team) : NULL;
   if (!root || !items || !team_raw || !cJSON_AddStringToObject(root, "server_id", server_id))
      goto fail;
   cJSON_AddItemToObject(root, "team", team_raw);
   team_raw = NULL;
   cJSON_AddItemToObject(root, "agents", items);
   items = NULL;
   cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "agents");
   for (size_t i = 0; i < count; ++i)
   {
      cJSON *a = cJSON_CreateObject();
      if (!a || !cJSON_AddStringToObject(a, "name", sorted[i].name) ||
          !cJSON_AddStringToObject(a, "provider", sorted[i].provider) ||
          !cJSON_AddStringToObject(a, "model", sorted[i].model) ||
          !cJSON_AddBoolToObject(a, "enabled", sorted[i].enabled) ||
          !cJSON_AddBoolToObject(a, "delegate_available", sorted[i].delegate_available) ||
          !cJSON_AddBoolToObject(a, "primary_only", sorted[i].primary_only) ||
          !cJSON_AddNumberToObject(a, "max_parallel", sorted[i].max_parallel))
      {
         cJSON_Delete(a);
         goto fail;
      }
      cJSON_AddItemToArray(array, a);
   }
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!json)
      return -1;
   size_t n = strlen(json);
   if (n >= cap || n > SERVER_MGMT_READ_BODY_MAX)
   {
      free(json);
      return -1;
   }
   memcpy(out, json, n + 1);
   free(json);
   return (int)n;
fail:
   cJSON_Delete(team_raw);
   cJSON_Delete(items);
   cJSON_Delete(root);
   return -1;
}

int server_mgmt_read_project_config(const char *server_id, int64_t team_id,
                                    const server_mgmt_read_config_t *config, char *out, size_t cap)
{
   if (out && cap)
      out[0] = 0;
   if (!out || !cap || !identifier(server_id, 127) || team_id <= 0 || !config || config->mtls < 0 ||
       config->mtls > 2 || config->remote_writes < 0 || config->remote_writes > 2 ||
       (config->cli_session_forwarding != 0 && config->cli_session_forwarding != 1) ||
       (config->require_aimee_git != 0 && config->require_aimee_git != 1) ||
       strnlen(config->client_transport, sizeof(config->client_transport)) >=
           sizeof(config->client_transport))
      return -1;
   const char *mtls[] = {"off", "optional", "required"};
   const char *writes[] = {"off", "data", "full"};
   const char *transport = config->client_transport;
   if (!transport[0] || !strcmp(transport, "socket"))
      transport = "socket";
   else if (strcmp(transport, "http") && strcmp(transport, "auto"))
      return -1;

   cJSON *root = cJSON_CreateObject();
   cJSON *projected = root ? cJSON_CreateObject() : NULL;
   char team[32];
   int team_n = snprintf(team, sizeof(team), "%lld", (long long)team_id);
   cJSON *team_raw = team_n > 0 && (size_t)team_n < sizeof(team) ? cJSON_CreateRaw(team) : NULL;
   if (!root || !projected || !team_raw || !cJSON_AddStringToObject(root, "server_id", server_id))
      goto config_fail;
   cJSON_AddItemToObject(root, "team", team_raw);
   team_raw = NULL;
   cJSON_AddItemToObject(root, "config", projected);
   projected = NULL;
   cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "config");
   if (!cJSON_AddStringToObject(object, "mtls", mtls[config->mtls]) ||
       !cJSON_AddStringToObject(object, "remote_writes", writes[config->remote_writes]) ||
       !cJSON_AddStringToObject(object, "client_transport", transport) ||
       !cJSON_AddBoolToObject(object, "cli_session_forwarding", config->cli_session_forwarding) ||
       !cJSON_AddBoolToObject(object, "require_aimee_git", config->require_aimee_git))
      goto config_fail;
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!json)
      return -1;
   size_t n = strlen(json);
   if (n >= cap || n > SERVER_MGMT_READ_BODY_MAX)
   {
      free(json);
      return -1;
   }
   memcpy(out, json, n + 1);
   free(json);
   return (int)n;
config_fail:
   cJSON_Delete(team_raw);
   cJSON_Delete(projected);
   cJSON_Delete(root);
   return -1;
}
