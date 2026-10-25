#include "cli_ir.h"
/* First-party canonical chat subscriber; ACP alone projects callbacks to its wire. */
#include "aimee_home.h"
#include "cli_client.h"
#include "cli_chat_stream.h"
#include "cJSON.h"
#include <aimee/ir/session_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct part
{
   char *message, *text, *tool, *name, *key;
   size_t length, capacity;
   int index, kind, begun, final, announced, finished;
   struct part *next;
} part_t;
typedef struct
{
   const char *session;
   char *turn, *actor, *reply;
   size_t length, capacity, parts;
   uint64_t sequence;
   int http_status;
   int started, completed, failed;
   part_t *blocks;
   void (*text)(const char *, void *);
   cli_chat_tool_fn tool;
   void *context;
} subscriber_t;
static const char *string(const cJSON *o, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
}
static int append(char **out, size_t *length, size_t *capacity, const char *text)
{
   size_t n = strlen(text);
   if (n > CLIENT_MAX_RESPONSE_SIZE - *length - 1)
      return -1;
   if (*length + n + 1 > *capacity)
   {
      size_t cap = *capacity ? *capacity : 256;
      while (cap < *length + n + 1)
         cap *= 2;
      char *copy = realloc(*out, cap);
      if (!copy)
         return -1;
      *out = copy;
      *capacity = cap;
   }
   memcpy(*out + *length, text, n + 1);
   *length += n;
   return 0;
}
static part_t *part(subscriber_t *s, const char *message, int index, int kind)
{
   if (!message || !message[0] || strlen(message) > 1024 || index < 0 ||
       index >= AIMEE_IR_MAX_BLOCKS)
      return NULL;
   for (part_t *p = s->blocks; p; p = p->next)
      if (p->index == index && !strcmp(p->message, message))
         return p->kind == kind ? p : NULL;
   if (s->parts >= AIMEE_IR_MAX_BLOCKS)
      return NULL;
   part_t *p = calloc(1, sizeof *p);
   if (!p || !(p->message = strdup(message)))
   {
      free(p);
      return NULL;
   }
   p->index = index;
   p->kind = kind;
   p->next = s->blocks;
   s->blocks = p;
   s->parts++;
   return p;
}
static int text(subscriber_t *s, part_t *p, const char *value, int snapshot)
{
   if (!p || !value)
      return -1;
   if (snapshot)
   {
      if ((p->final && strlen(value) != p->length) || strlen(value) < p->length ||
          (p->length && memcmp(value, p->text, p->length)))
         return -1;
      value += p->length;
   }
   if (!value[0])
      return 0;
   if (append(&p->text, &p->length, &p->capacity, value) ||
       append(&s->reply, &s->length, &s->capacity, value))
      return -1;
   if (s->text)
      s->text(value, s->context);
   return 0;
}
static int tool(subscriber_t *s, part_t *p, const aimee_block_t *b)
{
   if (!p || !b->tool_id || !b->tool_name || strlen(b->tool_id) > 1024 ||
       strlen(b->tool_name) > 1024)
      return -1;
   if (p->announced)
      return strcmp(p->tool, b->tool_id) || strcmp(p->name, b->tool_name) ? -1 : 0;
   for (part_t *other = s->blocks; other; other = other->next)
      if (other != p && other->tool && !other->finished && !strcmp(other->tool, b->tool_id))
         return -1;
   size_t n = strlen(s->turn) + strlen(p->message) + 32;
   p->tool = strdup(b->tool_id);
   p->name = strdup(b->tool_name);
   p->key = malloc(n);
   if (!p->tool || !p->name || !p->key)
      return -1;
   snprintf(p->key, n, "%s:%s:%d", s->turn, p->message, p->index);
   p->announced = 1;
   if (s->tool)
      s->tool("started", p->key, p->name, s->context);
   return 0;
}
static int message(subscriber_t *s, const cJSON *event)
{
   aimee_message_t m = {0};
   if (aimee_ir_message_parse(cJSON_GetObjectItemCaseSensitive(event, "payload"), &m))
      return -1;
   const char *id = string(event, "message_id");
   const cJSON *partial = cJSON_GetObjectItemCaseSensitive(event, "partial");
   aimee_request_t admitted = {.messages = &m, .n_messages = 1};
   const char *role = string(event, "role");
   int rc = (partial && !cJSON_IsBool(partial)) || (role && strcmp(role, m.role)) ||
                    aimee_ir_request_admit(&admitted)
                ? -1
                : 0;
   for (int i = 0; !rc && i < m.n_blocks; i++)
   {
      aimee_block_t *b = &m.blocks[i];
      if (!strcmp(m.role, "assistant") && b->type == AIMEE_BLK_TEXT)
      {
         part_t *p = part(s, id, (int)i, AIMEE_BLK_TEXT);
         rc = text(s, p, b->text, 1);
         if (p)
         {
            p->begun = 1;
            p->final = !cJSON_IsTrue(partial);
         }
      }
      else if (!strcmp(m.role, "assistant") && b->type == AIMEE_BLK_TOOL_USE)
         rc = tool(s, part(s, id, (int)i, AIMEE_BLK_TOOL_USE), b);
      else if (b->type == AIMEE_BLK_TOOL_RESULT)
      {
         if (strcmp(m.role, "tool") || cJSON_IsTrue(partial))
         {
            rc = -1;
            break;
         }
         part_t *p = s->blocks;
         while (p && (!p->tool || p->finished || strcmp(p->tool, b->tool_id)))
            p = p->next;
         if (!p || p->finished)
            rc = -1;
         else
         {
            p->finished = 1;
            if (s->tool)
               s->tool(b->tool_is_error ? "failed" : "completed", p->key, p->name, s->context);
         }
      }
   }
   aimee_message_free(&m);
   return rc;
}
static int consume(cJSON *envelope, void *context)
{
   subscriber_t *s = context;
   const char *sid = string(envelope, "session_id"), *turn = string(envelope, "turn_id"),
              *actor = string(envelope, "actor"), *seq = string(envelope, "sequence"),
              *id = string(envelope, "event_id");
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(envelope, "version"),
               *event = cJSON_GetObjectItemCaseSensitive(envelope, "event"),
               *payload = cJSON_GetObjectItemCaseSensitive(event, "payload");
   const char *kind = string(event, "kind");
   if (s->failed || s->http_status != 200 || !aimee_ir_json_unique(envelope) ||
       !cJSON_IsNumber(version) || version->valuedouble != 1 || !sid || strcmp(sid, s->session) ||
       !turn || !turn[0] || strlen(turn) > 127 || !actor || !actor[0] || strlen(actor) > 255 ||
       !id || !id[0] || strlen(id) > 1024 || !seq || seq[0] < '1' || seq[0] > '9' ||
       strlen(seq) > 20 || !kind)
      goto fail;
   uint64_t sequence = 0;
   for (const char *p = seq; *p; p++)
   {
      if (*p < '0' || *p > '9' || sequence > (UINT64_MAX - (unsigned)(*p - '0')) / 10)
         goto fail;
      sequence = sequence * 10 + (unsigned)(*p - '0');
   }
   if ((s->sequence && (s->sequence == UINT64_MAX || sequence != s->sequence + 1)) ||
       (s->actor && strcmp(actor, s->actor)))
      goto fail;
   s->sequence = sequence;
   if (!s->actor && !(s->actor = strdup(actor)))
      goto fail;
   if (!strcmp(kind, "execution"))
   {
      const char *status = string(payload, "status");
      if (!status)
         goto fail;
      if (!strcmp(status, "running") && !s->started && !s->completed)
      {
         if (!(s->turn = strdup(turn)))
            goto fail;
         s->started = 1;
         return 0;
      }
      if (!s->started || s->completed || strcmp(turn, s->turn) || strcmp(status, "completed"))
         goto fail;
      for (part_t *p = s->blocks; p; p = p->next)
         if ((p->kind == AIMEE_BLK_TEXT && !p->final) || (p->announced && !p->finished))
            goto fail;
      s->completed = 1;
      return 1; /* POSIX closes after the durable host terminal; Windows may keep draining. */
   }
   if (!strcmp(kind, "history") || !strcmp(kind, "projection") ||
       !strcmp(kind, "child_execution") || !strcmp(kind, "replay_proof") ||
       !strcmp(kind, "native_checkpoint") || !strcmp(kind, "native_compaction") ||
       !strcmp(kind, "tool_interruption"))
      return 0;
   if (!s->started || s->completed || strcmp(turn, s->turn) || !string(event, "message_id") ||
       !string(event, "message_id")[0])
      goto fail;
   if (!strcmp(kind, "message"))
   {
      if (message(s, event))
         goto fail;
      return 0;
   }
   if (!strcmp(kind, "delta"))
   {
      const char *type = string(payload, "type"), *role = string(event, "role"),
                 *block_kind = string(payload, "kind");
      if (!type || !strcmp(type, "ERROR"))
         goto fail;
      if (!strcmp(type, "TURN_START") || !strcmp(type, "TURN_STOP") || !strcmp(type, "BLOCK_STOP"))
         return 0;
      if (strcmp(type, "BLOCK_START") && strcmp(type, "BLOCK_DELTA"))
         goto fail;
      const cJSON *index = cJSON_GetObjectItemCaseSensitive(payload, "block_id");
      if (!role || strcmp(role, "assistant") || !block_kind || !cJSON_IsNumber(index) ||
          index->valuedouble != index->valueint || index->valueint < 0 ||
          index->valueint >= AIMEE_IR_MAX_BLOCKS)
         goto fail;
      if (!strcmp(block_kind, "text"))
      {
         const char *chunk = string(payload, "text_delta");
         part_t *p = part(s, string(event, "message_id"), index->valueint, AIMEE_BLK_TEXT);
         int start = !strcmp(type, "BLOCK_START");
         if (!p || p->final || (start ? p->begun : !p->begun) || (!chunk && !start) ||
             (chunk && text(s, p, chunk, 0)))
            goto fail;
         p->begun = 1;
      }
      else if (strcmp(block_kind, "thinking") && strcmp(block_kind, "tool_use"))
         goto fail;
      return 0;
   }
fail:
   s->failed = 1;
   return -1;
}
char *cli_chat_request_stream(const char *endpoint, const char *bearer, const cJSON *request,
                              int timeout, void (*text_cb)(const char *, void *),
                              cli_chat_tool_fn tool_cb, void *context)
{
   subscriber_t s = {.session = string(request, "aimee_session_id"),
                     .text = text_cb,
                     .tool = tool_cb,
                     .context = context};
   cJSON *copy = cJSON_IsObject(request) && aimee_ir_json_unique(request)
                     ? cJSON_Duplicate(request, 1)
                     : NULL;
   cJSON *version = cJSON_CreateNumber(1), *controls = cJSON_CreateFalse();
   int ok = copy && s.session && s.session[0] && version && controls;
   if (ok)
      ok = cJSON_GetObjectItemCaseSensitive(copy, "ir_version")
               ? cJSON_ReplaceItemInObjectCaseSensitive(copy, "ir_version", version)
               : cJSON_AddItemToObject(copy, "ir_version", version);
   if (ok)
      version = NULL;
   if (ok)
      ok = cJSON_GetObjectItemCaseSensitive(copy, "control_replies")
               ? cJSON_ReplaceItemInObjectCaseSensitive(copy, "control_replies", controls)
               : cJSON_AddItemToObject(copy, "control_replies", controls);
   if (ok)
      controls = NULL;
   char *body = ok ? cJSON_PrintUnformatted(copy) : NULL;
   cJSON_Delete(version);
   cJSON_Delete(controls);
   cJSON_Delete(copy);
   int delivery = body && strlen(body) < CLIENT_MAX_RESPONSE_SIZE
                      ? cli_http_request_stream_ndjson(endpoint, "POST", "/v1/chat/stream", body,
                                                       bearer, timeout, &s.http_status, consume, &s)
                      : -1;
   free(body);
   for (part_t *p = s.blocks; p;)
   {
      part_t *next = p->next;
      free(p->message);
      free(p->text);
      free(p->tool);
      free(p->name);
      free(p->key);
      free(p);
      p = next;
   }
   free(s.turn);
   free(s.actor);
   if (delivery || s.http_status != 200 || s.failed || !s.completed)
   {
      free(s.reply);
      fprintf(stderr, "aimee chat: canonical turn failed or stream ended before completion\n");
      return NULL;
   }
   return s.reply ? s.reply : strdup("");
}
char *cli_chat_stream(const char *sock, const char *session, const char *message,
                      void (*text_cb)(const char *, void *), cli_chat_tool_fn tool_cb,
                      void *context)
{
   (void)sock;
   if (!message || !message[0])
      return NULL;
   char *endpoint = cli_v1_has_remote_endpoint() ? cli_v1_client_endpoint() : NULL;
   char local[4096];
   if (!endpoint)
   {
      const char *home = aimee_home();
      if (!home ||
          snprintf(local, sizeof local, "unix:%s/aimee-http.sock", home) >= (int)sizeof local)
         return NULL;
   }
   char *bearer = cli_v1_has_remote_endpoint() ? cli_v1_client_bearer() : NULL;
   cJSON *request = cJSON_CreateObject();
   cJSON *canonical = cli_ir_user_message(message, NULL, NULL);
   if (!canonical || !cJSON_AddItemToObject(request, "message", canonical))
   {
      cJSON_Delete(canonical);
      cJSON_Delete(request);
      free(endpoint);
      free(bearer);
      return NULL;
   }
   cJSON_AddStringToObject(request, "aimee_session_id", session ? session : "");
   char cwd[4096];
   if (getcwd(cwd, sizeof cwd))
      cJSON_AddStringToObject(request, "cwd", cwd);
   const char *mode = getenv("AIMEE_MODE"), *attach = getenv("AIMEE_ATTACH_ID"),
              *client_type = getenv("AIMEE_CLIENT_TYPE");
   if (mode && mode[0])
      cJSON_AddStringToObject(request, "mode", mode);
   if (attach && attach[0])
      cJSON_AddStringToObject(request, "attach_id", attach);
   if (client_type && client_type[0])
      cJSON_AddStringToObject(request, "client_type", client_type);
   char *reply = cli_chat_request_stream(endpoint ? endpoint : local, bearer, request, -1, text_cb,
                                         tool_cb, context);
   cJSON_Delete(request);
   free(endpoint);
   free(bearer);
   return reply;
}
