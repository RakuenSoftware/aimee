/* shadow_mirror.c -- see shadow_mirror.h. */
#include "aimee.h"

#include "shadow_mirror.h"

#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
#include "agent_exec.h" /* agent_http_post */
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Bound concurrent delivery threads: each blocks for a subscriber's whole turn, so
 * without a cap a burst across many subscribers would spawn an unbounded pile. At
 * the cap we DROP (and count) -- shadow is best-effort, dropping is always safe. */
#define SHADOW_MIRROR_MAX_INFLIGHT 32
/* Only bounds a hung subscriber; the reply is discarded, so this is not a deadline. */
#define SHADOW_MIRROR_TIMEOUT_MS 300000
/* Cap the subscriber table (test VMs, not a crowd). */
#define SHADOW_MIRROR_MAX_SUBS 16
/* Prune a subscriber after this many consecutive failed deliveries: it went away.
 * A periodic re-subscribe (idempotent) rescues one pruned after a transient blip. */
#define SHADOW_MIRROR_FAIL_LIMIT 10

typedef struct
{
   char base_url[512];
   char bearer[256];
   int fails; /* consecutive delivery failures */
   int used;
} shadow_sub_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static shadow_sub_t g_subs[SHADOW_MIRROR_MAX_SUBS];

/* The ONE thing that arms shadow publishing. In-memory only, initialised to 0, so
 * the process ALWAYS boots disarmed: enabling it via POST /v1/shadow/enable can
 * never survive a restart. There is deliberately no config/env knob that could
 * re-arm it at boot -- that is the whole safety property (an operator can't enable
 * tapping of live prompt/response content, reboot, and have it silently persist). */
static volatile int g_armed;

static volatile long g_inflight;
static volatile long g_sent;
static volatile long g_dropped;
static volatile long g_pruned;

int shadow_mirror_publish_enabled(void)
{
   return __atomic_load_n(&g_armed, __ATOMIC_RELAXED) ? 1 : 0;
}

void shadow_mirror_set_armed(int on)
{
   __atomic_store_n(&g_armed, on ? 1 : 0, __ATOMIC_RELAXED);
   if (!on)
   {
      /* Disarming is a hard stop: forget every subscriber so a later re-arm starts
       * clean and no tap targets linger while disarmed. */
      pthread_mutex_lock(&g_lock);
      memset(g_subs, 0, sizeof(g_subs));
      pthread_mutex_unlock(&g_lock);
   }
   LOG_INFO("shadow.mirror", "shadow publishing %s", on ? "ARMED (runtime)" : "disarmed");
}

int shadow_mirror_is_mirrorable_path(const char *path)
{
   if (!path)
      return 0;
   return strcmp(path, "/v1/messages") == 0 || strcmp(path, "/v1/chat/completions") == 0 ||
          strcmp(path, "/v1/completions") == 0 || strcmp(path, "/v1/responses") == 0;
}

/* caller holds g_lock */
static shadow_sub_t *find_sub_locked(const char *base_url)
{
   for (int i = 0; i < SHADOW_MIRROR_MAX_SUBS; i++)
      if (g_subs[i].used && strcmp(g_subs[i].base_url, base_url) == 0)
         return &g_subs[i];
   return NULL;
}

int shadow_mirror_subscribe(const char *base_url, const char *bearer)
{
   if (!base_url || !base_url[0] || strlen(base_url) >= sizeof(g_subs[0].base_url))
      return -1;
   if (bearer && strlen(bearer) >= sizeof(g_subs[0].bearer))
      return -1;

   pthread_mutex_lock(&g_lock);
   shadow_sub_t *s = find_sub_locked(base_url);
   if (!s)
   {
      for (int i = 0; i < SHADOW_MIRROR_MAX_SUBS; i++)
         if (!g_subs[i].used)
         {
            s = &g_subs[i];
            break;
         }
   }
   if (!s)
   {
      pthread_mutex_unlock(&g_lock);
      return -1; /* table full */
   }
   s->used = 1;
   snprintf(s->base_url, sizeof(s->base_url), "%s", base_url);
   snprintf(s->bearer, sizeof(s->bearer), "%s", bearer ? bearer : "");
   s->fails = 0; /* refresh: a re-subscribe clears prior failures */
   pthread_mutex_unlock(&g_lock);
   LOG_INFO("shadow.mirror", "subscriber registered: %s", base_url);
   return 0;
}

int shadow_mirror_unsubscribe(const char *base_url)
{
   if (!base_url)
      return -1;
   pthread_mutex_lock(&g_lock);
   shadow_sub_t *s = find_sub_locked(base_url);
   if (s)
      memset(s, 0, sizeof(*s));
   pthread_mutex_unlock(&g_lock);
   return s ? 0 : -1;
}

int shadow_mirror_subscriber_count(void)
{
   int n = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < SHADOW_MIRROR_MAX_SUBS; i++)
      if (g_subs[i].used)
         n++;
   pthread_mutex_unlock(&g_lock);
   return n;
}

/* Record the outcome of a delivery: reset failures on success; on failure count up
 * and prune the subscriber once it has clearly gone away. */
static void record_delivery(const char *base_url, int ok)
{
   pthread_mutex_lock(&g_lock);
   shadow_sub_t *s = find_sub_locked(base_url);
   if (s)
   {
      if (ok)
         s->fails = 0;
      else if (++s->fails >= SHADOW_MIRROR_FAIL_LIMIT)
      {
         LOG_INFO("shadow.mirror", "pruning unreachable subscriber: %s", base_url);
         memset(s, 0, sizeof(*s));
         __atomic_fetch_add(&g_pruned, 1, __ATOMIC_RELAXED);
      }
   }
   pthread_mutex_unlock(&g_lock);
}

typedef struct
{
   char *url;      /* base_url + path, owned */
   char *auth;     /* "Bearer <token>" or NULL, owned */
   char *body;     /* request body copy, owned */
   char *base_url; /* for failure accounting, owned */
} mirror_job_t;

static void mirror_job_free(mirror_job_t *j)
{
   if (!j)
      return;
   free(j->url);
   free(j->auth);
   free(j->body);
   free(j->base_url);
   free(j);
}

static void *mirror_send_thread(void *arg)
{
   mirror_job_t *j = (mirror_job_t *)arg;
   char *resp = NULL;
   /* X-Aimee-Shadow marks this as mirrored traffic: the subscriber processes it
    * normally but must NOT re-mirror (loop guard on the receiving side). */
   int rc = agent_http_post(j->url, j->auth, j->body, &resp, SHADOW_MIRROR_TIMEOUT_MS,
                            "X-Aimee-Shadow: 1\r\n");
   free(resp); /* the subscriber's reply is deliberately discarded */
   record_delivery(j->base_url, rc >= 200 && rc < 300);
   if (rc < 200 || rc >= 300)
      LOG_DEBUG("shadow.mirror", "delivery to %s failed (rc=%d)", j->base_url, rc);
   mirror_job_free(j);
   __atomic_fetch_sub(&g_inflight, 1, __ATOMIC_RELAXED);
   return NULL;
}

/* Spawn one detached delivery to `sub` for (path, body); bounded by the in-flight
 * cap. On any resource failure the slot is backed out and the mirror dropped. */
static void deliver_one(const shadow_sub_t *sub, const char *path, const char *body, int body_len)
{
   long now = __atomic_add_fetch(&g_inflight, 1, __ATOMIC_RELAXED);
   if (now > SHADOW_MIRROR_MAX_INFLIGHT)
   {
      __atomic_fetch_sub(&g_inflight, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
      return;
   }

   mirror_job_t *j = calloc(1, sizeof(*j));
   if (!j)
   {
      __atomic_fetch_sub(&g_inflight, 1, __ATOMIC_RELAXED);
      return;
   }

   size_t urllen = strlen(sub->base_url) + strlen(path) + 1;
   j->url = malloc(urllen);
   if (j->url)
      snprintf(j->url, urllen, "%s%s", sub->base_url, path);
   j->base_url = strdup(sub->base_url);
   if (sub->bearer[0])
   {
      size_t alen = strlen(sub->bearer) + 8;
      j->auth = malloc(alen);
      if (j->auth)
         snprintf(j->auth, alen, "Bearer %s", sub->bearer);
   }
   j->body = malloc((size_t)body_len + 1);
   if (j->body)
   {
      memcpy(j->body, body, (size_t)body_len);
      j->body[body_len] = '\0';
   }
   if (!j->url || !j->body || !j->base_url || (sub->bearer[0] && !j->auth))
   {
      mirror_job_free(j);
      __atomic_fetch_sub(&g_inflight, 1, __ATOMIC_RELAXED);
      return;
   }

   pthread_t t;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int perr = pthread_create(&t, &attr, mirror_send_thread, j);
   pthread_attr_destroy(&attr);
   if (perr != 0)
   {
      mirror_job_free(j);
      __atomic_fetch_sub(&g_inflight, 1, __ATOMIC_RELAXED);
      return;
   }
   __atomic_fetch_add(&g_sent, 1, __ATOMIC_RELAXED);
}

typedef struct
{
   const char *path; /* one of the constant endpoint names below */
   int captured;
   const aimee_request_t *owner; /* borrowed identity only, never dereferenced after capture */
   aimee_request_t request;
} mirror_scope_t;
static _Thread_local mirror_scope_t g_scope;

void shadow_mirror_end_request(void)
{
   aimee_request_free(&g_scope.request);
   memset(&g_scope, 0, sizeof g_scope);
}
void shadow_mirror_begin_request(const char *path, int is_shadow_inbound)
{
   shadow_mirror_end_request();
   if (!path || is_shadow_inbound || !shadow_mirror_publish_enabled() ||
       !shadow_mirror_subscriber_count())
      return;
   static const char *const paths[] = {"/v1/messages", "/v1/chat/completions", "/v1/completions",
                                       "/v1/responses"};
   for (size_t i = 0; i < sizeof paths / sizeof *paths; i++)
      if (!strcmp(path, paths[i]))
         g_scope.path = paths[i];
}
void shadow_mirror_capture_request(const aimee_request_t *request)
{
   if (!g_scope.path || g_scope.captured || !shadow_mirror_publish_enabled() ||
       !shadow_mirror_subscriber_count())
      return;
   /* A nested producer cannot replace the actual ingress snapshot. */
   g_scope.captured = 1;
   g_scope.owner = request;
   if (!request || !request->n_messages || aimee_ir_request_admit(request) ||
       aimee_ir_request_clone(request, &g_scope.request))
   {
      g_scope.path = NULL;
      __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
   }
}

static char *mirror_text_request(const aimee_request_t *request);
/* Unknown top-level native controls are not shadow execution authority. Native
 * block/tool companions remain owned by the translation codec and raw sidecar;
 * semantic replay comparison, not copying an incoming body, renders them. */
static int mirror_fields_supported(const aimee_request_t *request, int legacy)
{
   static const char *const chat[] = {"model",
                                      "messages",
                                      "max_tokens",
                                      "temperature",
                                      "top_p",
                                      "top_k",
                                      "stream",
                                      "stop",
                                      "tools",
                                      "tool_choice",
                                      "parallel_tool_calls",
                                      "metadata",
                                      "reasoning_effort",
                                      "response_format",
                                      "prompt_cache_key",
                                      "prompt_cache_retention",
                                      "prompt_cache_options",
                                      "min_p",
                                      "repeat_penalty"};
   static const char *const anthropic[] = {
       "model",       "messages", "system",   "max_tokens",     "temperature",
       "top_p",       "top_k",    "stream",   "stop_sequences", "tools",
       "tool_choice", "metadata", "thinking", "service_tier",   "output_config"};
   static const char *const responses[] = {"model",
                                           "input",
                                           "instructions",
                                           "max_output_tokens",
                                           "temperature",
                                           "top_p",
                                           "stream",
                                           "tools",
                                           "tool_choice",
                                           "parallel_tool_calls",
                                           "metadata",
                                           "reasoning",
                                           "text",
                                           "prompt_cache_key",
                                           "prompt_cache_retention",
                                           "prompt_cache_options",
                                           "store",
                                           "include"};
   static const char *const text[] = {"model",    "prompt", "max_tokens", "temperature",
                                      "top_p",    "top_k",  "stream",     "stop",
                                      "metadata", "n",      "best_of",    "echo"};
   const char *const *keys = text;
   size_t count = sizeof text / sizeof *text;
   if (!legacy && request->frontend == AIMEE_WIRE_ANTHROPIC)
   {
      keys = anthropic;
      count = sizeof anthropic / sizeof *anthropic;
   }
   else if (!legacy && request->frontend == AIMEE_WIRE_RESPONSES)
   {
      keys = responses;
      count = sizeof responses / sizeof *responses;
   }
   else if (!legacy)
   {
      keys = chat;
      count = sizeof chat / sizeof *chat;
   }
   if (!request->raw)
      return 1;
   if (!cJSON_IsObject(request->raw) || !aimee_ir_json_unique(request->raw))
      return 0;
   const cJSON *entry;
   cJSON_ArrayForEach(entry, request->raw)
   {
      int known = 0;
      for (size_t i = 0; entry->string && i < count; i++)
         known |= !strcmp(entry->string, keys[i]);
      if (!known)
         return 0;
   }
   char *legacy_body = legacy ? mirror_text_request(request) : NULL;
   cJSON *canonical = legacy                                      ? cJSON_Parse(legacy_body)
                      : request->frontend == AIMEE_WIRE_ANTHROPIC ? anthropic_backend_build(request)
                      : request->frontend == AIMEE_WIRE_RESPONSES ? responses_backend_build(request)
                                                                  : openai_backend_build(request);
   free(legacy_body);
   if (!canonical)
      return 0;
   int faithful = 1;
   cJSON_ArrayForEach(entry, request->raw)
   {
      /* Content is normalized by its codec; the IR owns its block structure and
       * native companions. Scalars/options must not be silently ignored/coerced. */
      if (!strcmp(entry->string, "messages") || !strcmp(entry->string, "tools") ||
          !strcmp(entry->string, "input") || !strcmp(entry->string, "instructions") ||
          !strcmp(entry->string, "system") || !strcmp(entry->string, "prompt"))
         continue;
      if (legacy && ((!strcmp(entry->string, "n") || !strcmp(entry->string, "best_of")) &&
                     cJSON_IsNumber(entry) && entry->valuedouble == 1))
         continue;
      if (legacy && !strcmp(entry->string, "echo") && cJSON_IsFalse(entry))
         continue;
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(canonical, entry->string);
      if (cJSON_IsNull(entry) && (!value || cJSON_IsNull(value) ||
                                  (!strcmp(entry->string, "stream") && cJSON_IsFalse(value))))
         continue;
      if (!strcmp(entry->string, "stop") && cJSON_IsString(entry) && cJSON_IsArray(value) &&
          cJSON_GetArraySize(value) == 1 && cJSON_Compare(entry, cJSON_GetArrayItem(value, 0), 1))
         continue;
      if (!value && !strcmp(entry->string, "stream") && cJSON_IsFalse(entry))
         continue;
      if (!value || !cJSON_Compare(entry, value, 1))
         faithful = 0;
   }
   cJSON_Delete(canonical);
   return faithful;
}
static int mirror_add(cJSON *object, const char *key, cJSON *value)
{
   if (value && cJSON_AddItemToObject(object, key, value))
      return 1;
   cJSON_Delete(value);
   return 0;
}
static char *mirror_text_request(const aimee_request_t *request)
{
   if (aimee_ir_request_needs_model_projection(request))
      return NULL; /* A legacy text endpoint cannot faithfully expose hidden context. */
   if (request->n_system || request->n_messages != 1 || request->n_tools ||
       strcmp(request->messages[0].role, "user") || request->messages[0].n_blocks != 1 ||
       request->messages[0].blocks[0].type != AIMEE_BLK_TEXT)
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out || !cJSON_AddStringToObject(out, "prompt", request->messages[0].blocks[0].text) ||
       (request->model && !cJSON_AddStringToObject(out, "model", request->model)) ||
       (request->has_max_tokens &&
        !cJSON_AddNumberToObject(out, "max_tokens", request->max_tokens)) ||
       (request->has_temperature &&
        !cJSON_AddNumberToObject(out, "temperature", request->temperature)) ||
       (request->has_top_p && !cJSON_AddNumberToObject(out, "top_p", request->top_p)) ||
       (request->has_top_k && !cJSON_AddNumberToObject(out, "top_k", request->top_k)) ||
       !cJSON_AddBoolToObject(out, "stream", request->stream) ||
       (request->metadata && !mirror_add(out, "metadata", cJSON_Duplicate(request->metadata, 1))))
      goto failed;
   if (request->n_stop)
   {
      cJSON *stop = cJSON_CreateArray();
      if (!mirror_add(out, "stop", stop))
         goto failed;
      for (int i = 0; i < request->n_stop; i++)
      {
         cJSON *item = cJSON_CreateString(request->stop_sequences[i]);
         if (!item || !cJSON_AddItemToArray(stop, item))
         {
            cJSON_Delete(item);
            goto failed;
         }
      }
   }
   char *body = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   return body;
failed:
   cJSON_Delete(out);
   return NULL;
}

void shadow_mirror_dispatch_admitted(const aimee_request_t *owner)
{
   if (!g_scope.path || !g_scope.captured || owner != g_scope.owner ||
       aimee_ir_request_admit(owner))
      return;
   const char *path = g_scope.path;
   g_scope.path = NULL; /* once, before any nested producer or delivery */
   aimee_request_t *request = &g_scope.request;
   int legacy = !strcmp(path, "/v1/completions");
   char *body = NULL;
   if (shadow_mirror_publish_enabled() && !aimee_ir_request_admit(request) &&
       mirror_fields_supported(request, legacy))
   {
      aimee_wire_t wire = !strcmp(path, "/v1/messages")    ? AIMEE_WIRE_ANTHROPIC
                          : !strcmp(path, "/v1/responses") ? AIMEE_WIRE_RESPONSES
                                                           : AIMEE_WIRE_OPENAI_CHAT;
      aimee_wire_projection_report_t projection = {0};
      if (!legacy)
         aimee_backend_projection_report(request, wire, &projection);
      if (!projection.omitted_reasoning_blocks && !projection.omitted_opaque_state_blocks &&
          !projection.omitted_cache_controls)
         body = legacy ? mirror_text_request(request) : aimee_backend_render_wire(request, wire);
   }
   if (!body)
   {
      __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
      aimee_request_free(request);
      return;
   }
   shadow_sub_t snap[SHADOW_MIRROR_MAX_SUBS];
   int n = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < SHADOW_MIRROR_MAX_SUBS; i++)
      if (g_subs[i].used)
         snap[n++] = g_subs[i];
   pthread_mutex_unlock(&g_lock);
   for (int i = 0; i < n; i++)
      deliver_one(&snap[i], path, body, (int)strlen(body));
   free(body);
   aimee_request_free(request);
}

long shadow_mirror_sent_count(void)
{
   return __atomic_load_n(&g_sent, __ATOMIC_RELAXED);
}
long shadow_mirror_dropped_count(void)
{
   return __atomic_load_n(&g_dropped, __ATOMIC_RELAXED);
}
long shadow_mirror_pruned_count(void)
{
   return __atomic_load_n(&g_pruned, __ATOMIC_RELAXED);
}

void shadow_mirror_reset(void)
{
   shadow_mirror_end_request();
   __atomic_store_n(&g_armed, 0, __ATOMIC_RELAXED);
   pthread_mutex_lock(&g_lock);
   memset(g_subs, 0, sizeof(g_subs));
   pthread_mutex_unlock(&g_lock);
   __atomic_store_n(&g_sent, 0, __ATOMIC_RELAXED);
   __atomic_store_n(&g_dropped, 0, __ATOMIC_RELAXED);
   __atomic_store_n(&g_pruned, 0, __ATOMIC_RELAXED);
}
