#include "kb_bedrock_egress.h"

#include "cJSON.h"
#include "modules/aws/bedrock_policy.h"

#include <limits.h>
#include <math.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>

#define KB_BEDROCK_REQUEST_MAGIC 0xbed60c01U
#define KB_JSON_NODE_MAX         16384U
#define KB_JSON_DEPTH_MAX        64U

_Static_assert(AIMEE_STREAM_MAX_TOOLS == 64, "Bedrock stream index cap drift");
_Static_assert(AWS_ES_MAX_MESSAGE == KB_BEDROCK_BODY_MAX, "eventstream cap drift");

struct kb_bedrock_stream
{
   unsigned char *buffer;
   size_t length, offset, capacity;
   kb_bedrock_stream_callback_t callback;
   void *context;
   converse_stream_state_t ir;
   unsigned char open[64], used[64], kind[64];
   int message_started, message_stopped, metadata, busy, finished, poisoned;
   kb_bedrock_result_t fatal;
};

static int ascii_safe(const char *s, size_t max, int empty)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max);
   if (n == max || (!empty && n == 0))
      return 0;
   for (size_t i = 0; i < n; i++)
      if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f)
         return 0;
   return 1;
}

static int credential_safe(const kb_bedrock_credentials_t *c)
{
   if (!c || !ascii_safe(c->access_key_id, 129, 0) || !ascii_safe(c->secret_access_key, 256, 0) ||
       (c->session_token && !ascii_safe(c->session_token, AWS_SIGV4_TOKEN_MAX, 1)) ||
       !c->amz_date || !c->date || strnlen(c->amz_date, 17) != 16 || strnlen(c->date, 9) != 8 ||
       c->amz_date[8] != 'T' || c->amz_date[15] != 'Z' || memcmp(c->amz_date, c->date, 8) != 0)
      return 0;
   for (int i = 0; i < 16; i++)
      if (i != 8 && i != 15 && (c->amz_date[i] < '0' || c->amz_date[i] > '9'))
         return 0;
   for (int i = 0; i < 8; i++)
      if (c->date[i] < '0' || c->date[i] > '9')
         return 0;
   for (const unsigned char *p = (const unsigned char *)c->access_key_id; *p; p++)
      if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
         return 0;
   return 1;
}

static int node_budget_add(size_t *nodes, size_t add)
{
   if (add > KB_JSON_NODE_MAX - *nodes)
      return -1;
   *nodes += add;
   return 0;
}

static int bedrock_identifier(const char *s)
{
   size_t n = s ? strnlen(s, 65) : 0;
   if (!n || n > 64)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
         return 0;
   return 1;
}

static int strict_base64(const char *s)
{
   size_t n = s ? strnlen(s, 1024U * 1024U + 1U) : 0;
   if (!n || n > 1024U * 1024U || n % 4)
      return 0;
   size_t padding = s[n - 1] == '=' ? 1U : 0U;
   if (n > 1 && s[n - 2] == '=')
      padding++;
   for (size_t i = 0; i < n - padding; i++)
      if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '/'))
         return 0;
   for (size_t i = n - padding; i < n; i++)
      if (s[i] != '=')
         return 0;
   return padding <= 2;
}

static int string_budget(const char *s, size_t *total)
{
   if (!s)
      return -1;
   size_t n = strnlen(s, 1024U * 1024U + 1U);
   if (n > 1024U * 1024U)
      return -1;
   if (*total > KB_BEDROCK_BODY_MAX - n)
      return -1;
   *total += n;
   return 0;
}

static int json_budget(const cJSON *j, unsigned depth, size_t *nodes, size_t *strings)
{
   if (!j || depth > KB_JSON_DEPTH_MAX || ++*nodes > KB_JSON_NODE_MAX)
      return -1;
   if ((j->string && string_budget(j->string, strings) != 0) ||
       (cJSON_IsString(j) && string_budget(j->valuestring, strings) != 0))
      return -1;
   if (cJSON_IsObject(j))
      for (const cJSON *a = j->child; a; a = a->next)
         for (const cJSON *b = a->next; b; b = b->next)
            if (a->string && b->string && strcmp(a->string, b->string) == 0)
               return -1;
   for (const cJSON *c = j->child; c; c = c->next)
      if (json_budget(c, depth + 1, nodes, strings) != 0)
         return -1;
   return 0;
}

static int image_format_ok(const char *media_type)
{
   return media_type &&
          (strcmp(media_type, "image/png") == 0 || strcmp(media_type, "image/jpeg") == 0 ||
           strcmp(media_type, "image/jpg") == 0 || strcmp(media_type, "image/gif") == 0 ||
           strcmp(media_type, "image/webp") == 0);
}

static int block_ok(const aimee_block_t *b, size_t *strings, size_t *nodes, int system)
{
   if (!b || b->cache_control)
      return 0;
   if (system)
      return b->type == AIMEE_BLK_TEXT && b->text && b->text[0] && node_budget_add(nodes, 2) == 0 &&
             string_budget(b->text, strings) == 0;
   switch (b->type)
   {
   case AIMEE_BLK_TEXT:
      return b->text && b->text[0] && node_budget_add(nodes, 2) == 0 &&
             string_budget(b->text, strings) == 0;
   case AIMEE_BLK_IMAGE:
      return image_format_ok(b->media_type) && strict_base64(b->media_ref) &&
             node_budget_add(nodes, 5) == 0 && string_budget(b->media_type, strings) == 0 &&
             string_budget(b->media_ref, strings) == 0;
   case AIMEE_BLK_TOOL_USE:
      return bedrock_identifier(b->tool_id) && bedrock_identifier(b->tool_name) && b->tool_input &&
             cJSON_IsObject(b->tool_input) && node_budget_add(nodes, 4) == 0 &&
             string_budget(b->tool_id, strings) == 0 && string_budget(b->tool_name, strings) == 0 &&
             json_budget(b->tool_input, 1, nodes, strings) == 0;
   case AIMEE_BLK_TOOL_RESULT:
      return bedrock_identifier(b->tool_id) && b->tool_result && node_budget_add(nodes, 7) == 0 &&
             (cJSON_IsString(b->tool_result) || cJSON_IsObject(b->tool_result) ||
              cJSON_IsArray(b->tool_result)) &&
             string_budget(b->tool_id, strings) == 0 &&
             json_budget(b->tool_result, 1, nodes, strings) == 0;
   case AIMEE_BLK_THINKING:
      return b->text && b->text[0] && node_budget_add(nodes, b->thinking_signature ? 5 : 4) == 0 &&
             string_budget(b->text, strings) == 0 &&
             (!b->thinking_signature || string_budget(b->thinking_signature, strings) == 0);
   default:
      return 0;
   }
}

static int request_preflight(const aimee_request_t *ir)
{
   if (!ir || ir->n_system < 0 || ir->n_system > 4096 || ir->n_messages < 0 ||
       ir->n_messages > 1024 || ir->n_tools < 0 || ir->n_tools > 256 || ir->n_stop < 0 ||
       ir->n_stop > 64 || ir->has_top_k || ir->metadata || ir->service_tier || ir->thinking ||
       (ir->n_system && !ir->system) || (ir->n_messages && !ir->messages) ||
       (ir->n_tools && !ir->tools) || (ir->n_stop && !ir->stop_sequences))
      return 0;
   if ((ir->has_max_tokens && ir->max_tokens <= 0) ||
       (ir->has_temperature &&
        (!isfinite(ir->temperature) || ir->temperature < 0 || ir->temperature > 1)) ||
       (ir->has_top_p && (!isfinite(ir->top_p) || ir->top_p < 0 || ir->top_p > 1)))
      return 0;
   size_t strings = 0, blocks = (size_t)ir->n_system, nodes = 2;
   if (ir->n_system && node_budget_add(&nodes, 1) != 0)
      return 0;
   for (int i = 0; i < ir->n_system; i++)
      if (!block_ok(&ir->system[i], &strings, &nodes, 1))
         return 0;
   for (int i = 0; i < ir->n_messages; i++)
   {
      const aimee_message_t *m = &ir->messages[i];
      if (!m->role || (strcmp(m->role, "user") != 0 && strcmp(m->role, "assistant") != 0) ||
          m->n_blocks < 0 || (m->n_blocks && !m->blocks) || (size_t)m->n_blocks > 4096U - blocks ||
          node_budget_add(&nodes, 3) != 0 || string_budget(m->role, &strings) != 0)
         return 0;
      blocks += (size_t)m->n_blocks;
      for (int j = 0; j < m->n_blocks; j++)
         if (!block_ok(&m->blocks[j], &strings, &nodes, 0))
            return 0;
   }
   for (int i = 0; i < ir->n_tools; i++)
      if (!bedrock_identifier(ir->tools[i].name) || !ir->tools[i].schema ||
          !cJSON_IsObject(ir->tools[i].schema) || ir->tools[i].cache_control ||
          node_budget_add(&nodes, ir->tools[i].description ? 6 : 5) != 0 ||
          string_budget(ir->tools[i].name, &strings) != 0 ||
          (ir->tools[i].description && string_budget(ir->tools[i].description, &strings) != 0) ||
          json_budget(ir->tools[i].schema, 1, &nodes, &strings) != 0)
         return 0;
   for (int i = 0; i < ir->n_stop; i++)
      if (!ir->stop_sequences[i] || !ir->stop_sequences[i][0] || node_budget_add(&nodes, 1) != 0 ||
          string_budget(ir->stop_sequences[i], &strings) != 0)
         return 0;
   if ((ir->has_max_tokens || ir->has_temperature || ir->has_top_p || ir->n_stop) &&
       node_budget_add(&nodes, 1U + !!ir->has_max_tokens + !!ir->has_temperature + !!ir->has_top_p +
                                   !!ir->n_stop) != 0)
      return 0;
   if (ir->n_tools && node_budget_add(&nodes, 2) != 0)
      return 0;
   if (ir->tool_choice)
   {
      if (!cJSON_IsObject(ir->tool_choice))
         return 0;
      cJSON *type = cJSON_GetObjectItemCaseSensitive(ir->tool_choice, "type");
      int tool = cJSON_IsString(type) && strcmp(type->valuestring, "tool") == 0;
      if (!cJSON_IsString(type) ||
          (strcmp(type->valuestring, "auto") != 0 && strcmp(type->valuestring, "any") != 0 &&
           strcmp(type->valuestring, "tool") != 0) ||
          cJSON_GetArraySize(ir->tool_choice) != (tool ? 2 : 1) ||
          node_budget_add(&nodes, (tool ? 3U : 2U) + !ir->n_tools) != 0 ||
          json_budget(ir->tool_choice, 1, &nodes, &strings) != 0)
         return 0;
      if (tool)
      {
         cJSON *name = cJSON_GetObjectItemCaseSensitive(ir->tool_choice, "name");
         if (!cJSON_IsString(name) || !bedrock_identifier(name->valuestring))
            return 0;
         int found = 0;
         for (int i = 0; i < ir->n_tools; i++)
            found |= strcmp(ir->tools[i].name, name->valuestring) == 0;
         if (!found)
            return 0;
      }
   }
   return 1;
}

static int family_supported(const char *family)
{
   static const char *const families[] = {"anthropic", "amazon-nova", "amazon-titan", "meta-llama",
                                          "mistral",   "cohere",      "ai21"};
   for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++)
      if (family && strcmp(family, families[i]) == 0)
         return 1;
   return 0;
}

static const char *policy_id(const db2_bedrock_target_t *t, bedrock_target_type_t type,
                             int *was_arn)
{
   *was_arn = 0;
   if (strncmp(t->model_id, "arn:", 4) != 0)
      return t->model_id;
   const char *resource = NULL;
   const char *account = t->account;
   switch (type)
   {
   case BEDROCK_TARGET_FOUNDATION:
      resource = "foundation-model";
      account = "";
      break;
   case BEDROCK_TARGET_PROVISIONED:
      resource = "provisioned-model";
      break;
   case BEDROCK_TARGET_CUSTOM:
      resource = "custom-model";
      break;
   case BEDROCK_TARGET_APP_INFERENCE_PROFILE:
      resource = "application-inference-profile";
      break;
   case BEDROCK_TARGET_CROSS_REGION_INFERENCE_PROFILE:
      resource = "inference-profile";
      break;
   default:
      return NULL;
   }
   char prefix[256];
   int n = snprintf(prefix, sizeof(prefix), "arn:%s:bedrock:%s:%s:%s/", t->partition,
                    t->invoke_region, account, resource);
   if (n < 0 || (size_t)n >= sizeof(prefix) || strncmp(t->model_id, prefix, (size_t)n) != 0 ||
       !t->model_id[n])
      return NULL;
   *was_arn = 1;
   return t->model_id + n;
}

static kb_bedrock_result_t target_policy_check(const db2_bedrock_target_t *t, int streaming)
{
   if (!t || t->endpoint[0] || !ascii_safe(t->bedrock_api, sizeof(t->bedrock_api), 0) ||
       !ascii_safe(t->model_family, sizeof(t->model_family), 0) ||
       !ascii_safe(t->target_type, sizeof(t->target_type), 0) ||
       !ascii_safe(t->partition, sizeof(t->partition), 0) ||
       !ascii_safe(t->account, sizeof(t->account), 1) ||
       !ascii_safe(t->invoke_region, sizeof(t->invoke_region), 0) ||
       strcmp(t->bedrock_api, "converse") != 0 || !family_supported(t->model_family) ||
       !ascii_safe(t->model_id, 201, 0) || t->n_regions == 0 || t->n_regions > 64 ||
       t->n_underlying > 64)
      return KB_BEDROCK_INVALID_TARGET;
   bedrock_target_type_t type;
   if (strcmp(t->target_type, "foundation") == 0)
      type = BEDROCK_TARGET_FOUNDATION;
   else if (strcmp(t->target_type, "provisioned") == 0)
      type = BEDROCK_TARGET_PROVISIONED;
   else if (strcmp(t->target_type, "custom") == 0)
      type = BEDROCK_TARGET_CUSTOM;
   else if (strcmp(t->target_type, "application-inference-profile") == 0)
      type = BEDROCK_TARGET_APP_INFERENCE_PROFILE;
   else if (strcmp(t->target_type, "cross-region-inference-profile") == 0)
      type = BEDROCK_TARGET_CROSS_REGION_INFERENCE_PROFILE;
   else
      return KB_BEDROCK_INVALID_TARGET;
   int model_was_arn = 0;
   const char *id = policy_id(t, type, &model_was_arn);
   if (!id)
      return KB_BEDROCK_INVALID_TARGET;
   const char *regions[64], *arns[64];
   for (size_t i = 0; i < t->n_regions; i++)
   {
      if (!ascii_safe(t->regions[i], sizeof(t->regions[i]), 0))
         return KB_BEDROCK_INVALID_TARGET;
      regions[i] = t->regions[i];
   }
   for (size_t i = 0; i < t->n_underlying; i++)
   {
      if (!ascii_safe(t->underlying_fm_arns[i], sizeof(t->underlying_fm_arns[i]), 0))
         return KB_BEDROCK_INVALID_TARGET;
      arns[i] = t->underlying_fm_arns[i];
   }
   bedrock_target_t p = {.type = type,
                         .partition = t->partition,
                         .invoke_region = t->invoke_region,
                         .region_set = regions,
                         .n_regions = t->n_regions,
                         .account = t->account,
                         .id = id,
                         .underlying_fm_arns = arns,
                         .n_underlying = t->n_underlying};
   size_t cap = 65U * DB2_BEDROCK_ARN_CAP + 8192U;
   char *policy = malloc(cap);
   if (!policy)
      return KB_BEDROCK_INTERNAL_ERROR;
   int ok = bedrock_session_policy(&p, !!streaming, policy, cap) == 0;
   if (ok && model_was_arn && !strstr(policy, t->model_id))
      ok = 0;
   OPENSSL_cleanse(policy, cap);
   free(policy);
   return ok ? KB_BEDROCK_OK : KB_BEDROCK_INVALID_TARGET;
}

void kb_bedrock_wire_request_init(kb_bedrock_wire_request_t *r)
{
   if (r)
   {
      memset(r, 0, sizeof(*r));
      r->initialized = KB_BEDROCK_REQUEST_MAGIC;
   }
}

void kb_bedrock_wire_request_clear(kb_bedrock_wire_request_t *r)
{
   if (!r || r->initialized != KB_BEDROCK_REQUEST_MAGIC)
      return;
   if (r->body)
   {
      OPENSSL_cleanse(r->body, KB_BEDROCK_BODY_MAX + 5U);
      free(r->body);
   }
   OPENSSL_cleanse(r, sizeof(*r));
   r->initialized = KB_BEDROCK_REQUEST_MAGIC;
}

static int model_path_ok(const char *id)
{
   if (!id || !id[0] || strchr(id, '?') || strchr(id, '#'))
      return 0;
   const char *p = id;
   while (*p)
   {
      const char *slash = strchr(p, '/');
      size_t n = slash ? (size_t)(slash - p) : strlen(p);
      if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
         return 0;
      if (!slash)
         break;
      p = slash + 1;
   }
   return 1;
}

kb_bedrock_result_t kb_bedrock_wire_request_build(const db2_bedrock_target_t *t,
                                                  const aimee_request_t *ir, int streaming,
                                                  const kb_bedrock_credentials_t *c,
                                                  kb_bedrock_wire_request_t *out)
{
   if (!out || out->initialized != KB_BEDROCK_REQUEST_MAGIC || !ir || !c)
      return KB_BEDROCK_INVALID_ARGUMENT;
   kb_bedrock_result_t target_result = target_policy_check(t, streaming);
   if (target_result != KB_BEDROCK_OK)
      return target_result;
   if (!model_path_ok(t->model_id))
      return KB_BEDROCK_INVALID_TARGET;
   if (!request_preflight(ir) || !credential_safe(c))
      return KB_BEDROCK_INVALID_ARGUMENT;
   kb_bedrock_wire_request_t q;
   kb_bedrock_wire_request_init(&q);
   const char *suffix = streaming ? "/converse-stream" : "/converse";
   int n = snprintf(q.raw_path, sizeof(q.raw_path), "/model/%s%s", t->model_id, suffix);
   if (n < 0 || (size_t)n >= sizeof(q.raw_path) ||
       aws_uri_encode(q.raw_path, 0, q.encoded_path, sizeof(q.encoded_path)) != 0)
      return KB_BEDROCK_INVALID_TARGET;
   const char *domain = strcmp(t->partition, "aws-cn") == 0 ? "amazonaws.com.cn" : "amazonaws.com";
   n = snprintf(q.host, sizeof(q.host), "bedrock-runtime.%s.%s", t->invoke_region, domain);
   if (n < 0 || (size_t)n >= sizeof(q.host))
      return KB_BEDROCK_INVALID_TARGET;
   cJSON *json = bedrock_converse_build(ir);
   if (!json)
      return KB_BEDROCK_INTERNAL_ERROR;
   q.body = malloc(KB_BEDROCK_BODY_MAX + 5U);
   if (!q.body)
   {
      cJSON_Delete(json);
      return KB_BEDROCK_INTERNAL_ERROR;
   }
   if (!cJSON_PrintPreallocated(json, q.body, (int)KB_BEDROCK_BODY_MAX + 5, 0))
   {
      cJSON_Delete(json);
      kb_bedrock_wire_request_clear(&q);
      return KB_BEDROCK_TOO_LARGE;
   }
   cJSON_Delete(json);
   q.body_len = strnlen(q.body, KB_BEDROCK_BODY_MAX + 1U);
   if (q.body_len > KB_BEDROCK_BODY_MAX)
   {
      kb_bedrock_wire_request_clear(&q);
      return KB_BEDROCK_TOO_LARGE;
   }
   aws_sha256_hex((const unsigned char *)q.body, q.body_len, q.payload_hash);
   aws_sigv4_kv_t headers[] = {{"content-type", "application/json"},
                               {"host", q.host},
                               {"x-amz-content-sha256", q.payload_hash},
                               {"x-amz-date", c->amz_date}};
   aws_sigv4_request_t sign = {.method = "POST",
                               .raw_path = q.raw_path,
                               .headers = headers,
                               .n_headers = 4,
                               .payload_hash = q.payload_hash,
                               .amz_date = c->amz_date,
                               .date = c->date,
                               .region = t->invoke_region,
                               .service = "bedrock",
                               .access_key_id = c->access_key_id,
                               .secret_access_key = c->secret_access_key,
                               .session_token = c->session_token};
   if (aws_sigv4_sign(&sign, &q.sig) != 0)
   {
      kb_bedrock_wire_request_clear(&q);
      return KB_BEDROCK_SIGNING_ERROR;
   }
   q.streaming = !!streaming;
   kb_bedrock_wire_request_clear(out);
   *out = q;
   return KB_BEDROCK_OK;
}

kb_bedrock_result_t kb_bedrock_wire_request_headers(const kb_bedrock_wire_request_t *r,
                                                    kb_bedrock_header_t *h, size_t cap,
                                                    size_t *count)
{
   if (count)
      *count = 0;
   if (!r || r->initialized != KB_BEDROCK_REQUEST_MAGIC || !r->body || !h || !count)
      return KB_BEDROCK_INVALID_ARGUMENT;
   size_t n = r->sig.has_security_token ? 6 : 5;
   if (cap < n)
      return KB_BEDROCK_TOO_LARGE;
   h[0] = (kb_bedrock_header_t){"host", r->host};
   h[1] = (kb_bedrock_header_t){"content-type", "application/json"};
   h[2] = (kb_bedrock_header_t){"x-amz-date", r->sig.amz_date};
   h[3] = (kb_bedrock_header_t){"x-amz-content-sha256", r->payload_hash};
   h[4] = (kb_bedrock_header_t){"authorization", r->sig.authorization};
   if (n == 6)
      h[5] = (kb_bedrock_header_t){"x-amz-security-token", r->sig.security_token};
   *count = n;
   return KB_BEDROCK_OK;
}

void kb_bedrock_response_init(aimee_response_t *r)
{
   if (r)
      memset(r, 0, sizeof(*r));
}

static int json_boundary(const cJSON *j, unsigned depth, size_t *nodes)
{
   if (!j || depth > KB_JSON_DEPTH_MAX || ++*nodes > KB_JSON_NODE_MAX)
      return -1;
   if ((cJSON_IsString(j) && !ascii_safe(j->valuestring, 1024U * 1024U + 1U, 1)) ||
       (j->string && !ascii_safe(j->string, 1024U * 1024U + 1U, 1)))
      return -1;
   if (cJSON_IsObject(j))
      for (const cJSON *a = j->child; a; a = a->next)
         for (const cJSON *b = a->next; b; b = b->next)
            if (a->string && b->string && strcmp(a->string, b->string) == 0)
               return -1;
   for (const cJSON *c = j->child; c; c = c->next)
      if (json_boundary(c, depth + 1, nodes) != 0)
         return -1;
   return 0;
}

static int nonnegative_long(const cJSON *o, const char *key)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, key);
   return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0 &&
          v->valuedouble <= 9007199254740991.0 && v->valuedouble <= LONG_MAX &&
          floor(v->valuedouble) == v->valuedouble;
}

static int optional_nonnegative_long(const cJSON *o, const char *key)
{
   return !cJSON_GetObjectItemCaseSensitive((cJSON *)o, key) || nonnegative_long(o, key);
}

static int converse_response_strict(cJSON *j)
{
   cJSON *output = cJSON_GetObjectItemCaseSensitive(j, "output");
   cJSON *message = output ? cJSON_GetObjectItemCaseSensitive(output, "message") : NULL;
   cJSON *role = message ? cJSON_GetObjectItemCaseSensitive(message, "role") : NULL;
   cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
   cJSON *stop = cJSON_GetObjectItemCaseSensitive(j, "stopReason");
   cJSON *usage = cJSON_GetObjectItemCaseSensitive(j, "usage");
   if (!cJSON_IsObject(output) || !cJSON_IsObject(message) || !cJSON_IsString(role) ||
       strcmp(role->valuestring, "assistant") != 0 || !cJSON_IsArray(content) ||
       !cJSON_IsString(stop) || !cJSON_IsObject(usage) || !nonnegative_long(usage, "inputTokens") ||
       !nonnegative_long(usage, "outputTokens") ||
       !optional_nonnegative_long(usage, "cacheReadInputTokens") ||
       !optional_nonnegative_long(usage, "cacheWriteInputTokens"))
      return 0;
   cJSON *el = NULL;
   cJSON_ArrayForEach(el, content)
   {
      if (!cJSON_IsObject(el))
         return 0;
      int variants = !!cJSON_GetObjectItemCaseSensitive(el, "text") +
                     !!cJSON_GetObjectItemCaseSensitive(el, "toolUse") +
                     !!cJSON_GetObjectItemCaseSensitive(el, "reasoningContent");
      if (variants != 1)
         return 0;
      cJSON *text = cJSON_GetObjectItemCaseSensitive(el, "text");
      cJSON *tool = cJSON_GetObjectItemCaseSensitive(el, "toolUse");
      cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(el, "reasoningContent");
      if (text && !cJSON_IsString(text))
         return 0;
      if (tool)
      {
         cJSON *id = cJSON_GetObjectItemCaseSensitive(tool, "toolUseId");
         cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
         cJSON *input = cJSON_GetObjectItemCaseSensitive(tool, "input");
         if (!cJSON_IsObject(tool) || !cJSON_IsString(id) || !cJSON_IsString(name) ||
             !cJSON_IsObject(input))
            return 0;
      }
      if (reasoning)
      {
         cJSON *rt = cJSON_GetObjectItemCaseSensitive(reasoning, "reasoningText");
         cJSON *text_value = rt ? cJSON_GetObjectItemCaseSensitive(rt, "text") : NULL;
         cJSON *sig = rt ? cJSON_GetObjectItemCaseSensitive(rt, "signature") : NULL;
         if (!cJSON_IsObject(reasoning) || !cJSON_IsObject(rt) || !cJSON_IsString(text_value) ||
             (sig && !cJSON_IsString(sig)))
            return 0;
      }
   }
   return 1;
}

static cJSON *parse_json_exact(const unsigned char *body, size_t len, size_t cap, int *oom)
{
   if (oom)
      *oom = 0;
   int nul_escape = 0;
   for (size_t i = 0; body && i + 6 <= len; i++)
      if (memcmp(body + i, "\\u0000", 6) == 0)
         nul_escape = 1;
   if (!body || !len || len > cap || memchr(body, 0, len) || nul_escape)
      return NULL;
   char *copy = malloc(len + 1);
   if (!copy)
   {
      if (oom)
         *oom = 1;
      return NULL;
   }
   memcpy(copy, body, len);
   copy[len] = 0;
   const char *end = NULL;
   cJSON *j = cJSON_ParseWithLengthOpts(copy, len + 1, &end, 0);
   if (j)
   {
      while (end < copy + len && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
         end++;
      size_t nodes = 0;
      if (end != copy + len || json_boundary(j, 1, &nodes) != 0)
      {
         cJSON_Delete(j);
         j = NULL;
      }
   }
   OPENSSL_cleanse(copy, len + 1);
   free(copy);
   return j;
}

kb_bedrock_result_t kb_bedrock_nonstream_parse(const unsigned char *body, size_t len,
                                               aimee_response_t *out)
{
   if (!out)
      return KB_BEDROCK_INVALID_ARGUMENT;
   int oom = 0;
   cJSON *j = parse_json_exact(body, len, KB_BEDROCK_BODY_MAX, &oom);
   if (!j || !converse_response_strict(j))
   {
      cJSON_Delete(j);
      aimee_response_free(out);
      memset(out, 0, sizeof(*out));
      return oom ? KB_BEDROCK_INTERNAL_ERROR
                 : (len > KB_BEDROCK_BODY_MAX ? KB_BEDROCK_TOO_LARGE
                                              : KB_BEDROCK_MALFORMED_RESPONSE);
   }
   aimee_response_t tmp;
   memset(&tmp, 0, sizeof(tmp));
   char err[64];
   if (bedrock_converse_parse(j, &tmp, err, sizeof(err)) != 0 || !tmp.role || !tmp.raw ||
       !tmp.raw_stop_reason)
   {
      cJSON_Delete(j);
      aimee_response_free(&tmp);
      aimee_response_free(out);
      memset(out, 0, sizeof(*out));
      return KB_BEDROCK_INTERNAL_ERROR;
   }
   cJSON_Delete(j);
   aimee_response_free(out);
   *out = tmp;
   return KB_BEDROCK_OK;
}

static int view_eq(aws_es_view_t v, const char *s)
{
   return v.len == strlen(s) && memcmp(v.ptr, s, v.len) == 0;
}

static int header_count(const aws_es_message_t *m, const char *name)
{
   int n = 0;
   for (size_t i = 0; i < m->n_headers; i++)
      if (view_eq(m->headers[i].name, name))
         n++;
   return n;
}

static int view_copy(aws_es_view_t v, char *out, size_t cap)
{
   if (!v.ptr || !v.len || v.len >= cap)
      return -1;
   for (size_t i = 0; i < v.len; i++)
      if (v.ptr[i] < 0x20 || v.ptr[i] == 0x7f)
         return -1;
   memcpy(out, v.ptr, v.len);
   out[v.len] = 0;
   return 0;
}

static int known_exception(const char *type)
{
   return strcmp(type, "internalServerException") == 0 ||
          strcmp(type, "modelStreamErrorException") == 0 ||
          strcmp(type, "validationException") == 0 || strcmp(type, "throttlingException") == 0 ||
          strcmp(type, "serviceUnavailableException") == 0;
}

static kb_bedrock_result_t stream_poison(kb_bedrock_stream_t *s, kb_bedrock_result_t r)
{
   s->poisoned = 1;
   s->fatal = r;
   return r;
}

static kb_bedrock_result_t emit_delta(kb_bedrock_stream_t *s, const aimee_delta_t *d)
{
   if (s->callback && s->callback(d, s->context) != 0)
      return KB_BEDROCK_CALLBACK_ABORT;
   return KB_BEDROCK_OK;
}

static int strict_index(cJSON *payload, int *idx)
{
   cJSON *v = payload ? cJSON_GetObjectItemCaseSensitive(payload, "contentBlockIndex") : NULL;
   if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || floor(v->valuedouble) != v->valuedouble ||
       v->valuedouble < 0 || v->valuedouble >= 64)
      return -1;
   *idx = (int)v->valuedouble;
   return 0;
}

static kb_bedrock_result_t process_event(kb_bedrock_stream_t *s, const char *event, cJSON *j)
{
   int idx = -1;
   /* Metadata is terminal, and after messageStop only metadata may follow.  Keep
    * this guard ahead of the individual known-event branches so content events
    * cannot accidentally reopen a stopped turn. */
   if (s->metadata || (s->message_stopped && strcmp(event, "metadata") != 0))
      return KB_BEDROCK_MALFORMED_STREAM;
   if (strcmp(event, "messageStart") == 0)
   {
      if (s->message_started || s->message_stopped)
         return KB_BEDROCK_MALFORMED_STREAM;
      s->message_started = 1;
   }
   else if (!s->message_started)
      return KB_BEDROCK_MALFORMED_STREAM;
   if (strcmp(event, "contentBlockStart") == 0)
   {
      cJSON *start = cJSON_GetObjectItemCaseSensitive(j, "start");
      cJSON *tu = start ? cJSON_GetObjectItemCaseSensitive(start, "toolUse") : NULL;
      if (strict_index(j, &idx) || s->used[idx] || !cJSON_IsObject(tu))
         return KB_BEDROCK_MALFORMED_STREAM;
      s->used[idx] = s->open[idx] = 1;
      s->kind[idx] = AIMEE_BLK_TOOL_USE;
   }
   else if (strcmp(event, "contentBlockDelta") == 0)
   {
      cJSON *delta = cJSON_GetObjectItemCaseSensitive(j, "delta");
      if (strict_index(j, &idx) || !cJSON_IsObject(delta))
         return KB_BEDROCK_MALFORMED_STREAM;
      int text = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(delta, "text"));
      int tool = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(delta, "toolUse"));
      int reason = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(delta, "reasoningContent"));
      if (text + tool + reason != 1)
         return KB_BEDROCK_MALFORMED_STREAM;
      int kind = text ? AIMEE_BLK_TEXT : (tool ? AIMEE_BLK_TOOL_USE : AIMEE_BLK_THINKING);
      if (!s->open[idx])
      {
         if (tool || s->used[idx])
            return KB_BEDROCK_MALFORMED_STREAM;
         s->used[idx] = s->open[idx] = 1;
         s->kind[idx] = (unsigned char)kind;
      }
      else if (s->kind[idx] != kind)
         return KB_BEDROCK_MALFORMED_STREAM;
   }
   else if (strcmp(event, "contentBlockStop") == 0)
   {
      if (strict_index(j, &idx) || !s->open[idx])
         return KB_BEDROCK_MALFORMED_STREAM;
      s->open[idx] = 0;
   }
   else if (strcmp(event, "messageStop") == 0)
   {
      if (s->message_stopped)
         return KB_BEDROCK_MALFORMED_STREAM;
      for (int i = 0; i < 64; i++)
         if (s->open[i])
            return KB_BEDROCK_MALFORMED_STREAM;
      s->message_stopped = 1;
   }
   else if (strcmp(event, "metadata") == 0)
   {
      if (!s->message_stopped || s->metadata)
         return KB_BEDROCK_MALFORMED_STREAM;
      cJSON *usage = cJSON_GetObjectItemCaseSensitive(j, "usage");
      if (!cJSON_IsObject(usage) || !nonnegative_long(usage, "inputTokens") ||
          !nonnegative_long(usage, "outputTokens"))
         return KB_BEDROCK_MALFORMED_STREAM;
      s->metadata = 1;
   }
   else if (s->message_stopped)
      return KB_BEDROCK_MALFORMED_STREAM;
   aimee_delta_t d[2];
   int n = bedrock_converse_stream_to_deltas(event, j, &s->ir, d, 2);
   if (n < 0)
      return KB_BEDROCK_MALFORMED_STREAM;
   for (int i = 0; i < n; i++)
   {
      kb_bedrock_result_t r = emit_delta(s, &d[i]);
      if (r != KB_BEDROCK_OK)
         return r;
   }
   return KB_BEDROCK_OK;
}

static kb_bedrock_result_t process_frame(kb_bedrock_stream_t *s, const aws_es_message_t *m)
{
   if (m->headers_truncated || header_count(m, ":message-type") != 1 || !m->message_type ||
       m->message_type->value_type != AWS_ES_HDR_STRING)
      return KB_BEDROCK_MALFORMED_STREAM;
   if (m->msg_type == AWS_ES_MSG_ERROR)
   {
      if (header_count(m, ":error-code") != 1 || header_count(m, ":error-message") != 1 ||
          header_count(m, ":event-type") || header_count(m, ":exception-type") ||
          header_count(m, ":content-type") || m->payload.len || m->content_type || m->event_type ||
          m->exception_type || !view_eq(m->message_type->value, "error"))
         return KB_BEDROCK_MALFORMED_STREAM;
      char code[128], message[128];
      if (!m->error_code || m->error_code->value_type != AWS_ES_HDR_STRING ||
          view_copy(m->error_code->value, code, sizeof(code)) || !m->error_message ||
          m->error_message->value_type != AWS_ES_HDR_STRING ||
          view_copy(m->error_message->value, message, sizeof(message)))
         return KB_BEDROCK_MALFORMED_STREAM;
      aimee_delta_t d = {.type = AIMEE_DELTA_ERROR, .error_message = message};
      kb_bedrock_result_t r = emit_delta(s, &d);
      return r == KB_BEDROCK_OK ? KB_BEDROCK_PROVIDER_ERROR : r;
   }
   if ((m->msg_type != AWS_ES_MSG_EVENT && m->msg_type != AWS_ES_MSG_EXCEPTION) ||
       header_count(m, ":content-type") != 1 || !m->content_type ||
       m->content_type->value_type != AWS_ES_HDR_STRING ||
       !view_eq(m->content_type->value, "application/json") ||
       m->payload.len > KB_BEDROCK_EVENT_JSON_MAX)
      return KB_BEDROCK_MALFORMED_STREAM;
   if (m->msg_type == AWS_ES_MSG_EVENT)
   {
      if (!view_eq(m->message_type->value, "event") || header_count(m, ":exception-type") ||
          header_count(m, ":error-code") || header_count(m, ":error-message"))
         return KB_BEDROCK_MALFORMED_STREAM;
   }
   else if (!view_eq(m->message_type->value, "exception") || header_count(m, ":event-type") ||
            header_count(m, ":error-code") || header_count(m, ":error-message"))
      return KB_BEDROCK_MALFORMED_STREAM;
   const aws_es_header_t *type =
       m->msg_type == AWS_ES_MSG_EVENT ? m->event_type : m->exception_type;
   const char *header_name = m->msg_type == AWS_ES_MSG_EVENT ? ":event-type" : ":exception-type";
   if (!type || type->value_type != AWS_ES_HDR_STRING || header_count(m, header_name) != 1)
      return KB_BEDROCK_MALFORMED_STREAM;
   char event[128];
   if (view_copy(type->value, event, sizeof(event)))
      return KB_BEDROCK_MALFORMED_STREAM;
   if (m->msg_type == AWS_ES_MSG_EXCEPTION && !known_exception(event))
      return KB_BEDROCK_MALFORMED_STREAM;
   int oom = 0;
   cJSON *j = parse_json_exact(m->payload.ptr, m->payload.len, KB_BEDROCK_EVENT_JSON_MAX, &oom);
   if (!j || !cJSON_IsObject(j))
   {
      cJSON_Delete(j);
      return oom ? KB_BEDROCK_INTERNAL_ERROR : KB_BEDROCK_MALFORMED_STREAM;
   }
   kb_bedrock_result_t r;
   if (m->msg_type == AWS_ES_MSG_EXCEPTION)
   {
      const char *message = NULL;
      cJSON *mv = cJSON_GetObjectItemCaseSensitive(j, "message");
      if (!cJSON_IsString(mv))
         r = KB_BEDROCK_MALFORMED_STREAM;
      else
      {
         message = mv->valuestring;
         aimee_delta_t d = {.type = AIMEE_DELTA_ERROR, .error_message = message};
         r = emit_delta(s, &d);
         if (r == KB_BEDROCK_OK)
            r = KB_BEDROCK_PROVIDER_ERROR;
      }
   }
   else
      r = process_event(s, event, j);
   cJSON_Delete(j);
   return r;
}

kb_bedrock_result_t kb_bedrock_stream_init(kb_bedrock_stream_t **out,
                                           kb_bedrock_stream_callback_t cb, void *ctx)
{
   if (!out || *out)
      return KB_BEDROCK_INVALID_ARGUMENT;
   kb_bedrock_stream_t *s = calloc(1, sizeof(*s));
   if (!s)
      return KB_BEDROCK_INTERNAL_ERROR;
   s->buffer = malloc(AWS_ES_MAX_MESSAGE);
   if (!s->buffer)
   {
      free(s);
      return KB_BEDROCK_INTERNAL_ERROR;
   }
   s->capacity = AWS_ES_MAX_MESSAGE;
   s->callback = cb;
   s->context = ctx;
   converse_stream_state_init(&s->ir);
   *out = s;
   return KB_BEDROCK_OK;
}

kb_bedrock_result_t kb_bedrock_stream_feed(kb_bedrock_stream_t *s, const unsigned char *bytes,
                                           size_t length)
{
   if (!s || (!bytes && length))
      return KB_BEDROCK_INVALID_ARGUMENT;
   if (s->busy)
      return KB_BEDROCK_BUSY;
   if (s->poisoned)
      return KB_BEDROCK_POISONED;
   if (s->finished)
      return KB_BEDROCK_INVALID_ARGUMENT;
   s->busy = 1;
   while (length || s->offset < s->length)
   {
      if (s->offset == s->length)
         s->offset = s->length = 0;
      if (length && s->length < s->capacity)
      {
         size_t take = s->capacity - s->length;
         if (take > length)
            take = length;
         memcpy(s->buffer + s->length, bytes, take);
         s->length += take;
         bytes += take;
         length -= take;
      }
      aws_es_message_t m;
      size_t consumed = 0;
      aws_es_status_t es =
          aws_es_decode(s->buffer + s->offset, s->length - s->offset, &m, &consumed);
      if (es == AWS_ES_OK)
      {
         kb_bedrock_result_t r = process_frame(s, &m);
         s->offset += consumed;
         if (r != KB_BEDROCK_OK)
         {
            s->busy = 0;
            return stream_poison(s, r);
         }
         continue;
      }
      if (es == AWS_ES_ERROR || s->length - s->offset == s->capacity)
      {
         s->busy = 0;
         return stream_poison(s, KB_BEDROCK_MALFORMED_STREAM);
      }
      if (s->offset && length)
      {
         memmove(s->buffer, s->buffer + s->offset, s->length - s->offset);
         s->length -= s->offset;
         s->offset = 0;
         continue;
      }
      break;
   }
   s->busy = 0;
   return KB_BEDROCK_OK;
}

kb_bedrock_result_t kb_bedrock_stream_finish(kb_bedrock_stream_t *s)
{
   if (!s)
      return KB_BEDROCK_INVALID_ARGUMENT;
   if (s->busy)
      return KB_BEDROCK_BUSY;
   if (s->poisoned)
      return KB_BEDROCK_POISONED;
   if (s->finished)
      return KB_BEDROCK_INVALID_ARGUMENT;
   if (s->offset != s->length || !s->message_started || !s->message_stopped || !s->metadata)
      return stream_poison(s, KB_BEDROCK_INCOMPLETE_STREAM);
   OPENSSL_cleanse(s->buffer, s->capacity);
   s->length = s->offset = 0;
   s->finished = 1;
   return KB_BEDROCK_OK;
}

kb_bedrock_result_t kb_bedrock_stream_clear(kb_bedrock_stream_t **sp)
{
   if (!sp || !*sp)
      return KB_BEDROCK_INVALID_ARGUMENT;
   kb_bedrock_stream_t *s = *sp;
   if (s->busy)
      return KB_BEDROCK_BUSY;
   OPENSSL_cleanse(s->buffer, s->capacity);
   free(s->buffer);
   OPENSSL_cleanse(s, sizeof(*s));
   free(s);
   *sp = NULL;
   return KB_BEDROCK_OK;
}

int kb_bedrock_dispatch_https(const kb_bedrock_target_t *t, const aimee_request_t *ir, int stream,
                              const char *ak, const char *sk, const char *tok, const char *amz,
                              const char *date, const char *ca, const char *cc, char *out,
                              size_t cap, int *status)
{
   (void)t;
   (void)ir;
   (void)stream;
   (void)ak;
   (void)sk;
   (void)tok;
   (void)amz;
   (void)date;
   (void)ca;
   (void)cc;
   if (out && cap)
      out[0] = 0;
   if (status)
      *status = 0;
   return -1;
}
