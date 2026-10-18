/* aimee_ir.c -- lifecycle + accessors for the canonical IR. See aimee_ir.h.
 * Pure: depends only on cJSON + libc, so it unit-tests standalone. */
#include <aimee/ir/aimee_ir.h>

#include "cJSON.h"

#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void aimee_ir_block_set_context(aimee_block_t *block, aimee_context_origin_t origin,
                                aimee_context_authority_t authority, aimee_context_trust_t trust,
                                aimee_context_sensitivity_t sensitivity, int model_visible,
                                const char *revision_domain, const char *revision_scope,
                                unsigned long long revision_epoch)
{
   if (!block)
      return;
   memset(&block->context, 0, sizeof block->context);
   block->context.origin = origin;
   block->context.authority = authority;
   block->context.trust = trust;
   block->context.sensitivity = sensitivity;
   block->context.model_visible = model_visible ? 1 : 0;
   snprintf(block->context.revision_domain, sizeof block->context.revision_domain, "%s",
            revision_domain ? revision_domain : "");
   snprintf(block->context.revision_scope, sizeof block->context.revision_scope, "%s",
            revision_scope ? revision_scope : "");
   block->context.revision_epoch = revision_epoch;
}

int aimee_ir_context_promotion_allowed(aimee_context_origin_t origin,
                                       aimee_context_authority_t from, aimee_context_authority_t to)
{
   if (to <= from)
      return 1;
   if (to == AIMEE_CTX_AUTH_POLICY)
      return origin == AIMEE_CTX_ORIGIN_PLATFORM || origin == AIMEE_CTX_ORIGIN_OPERATOR;
   if (to == AIMEE_CTX_AUTH_TASK_INSTRUCTION)
      return origin == AIMEE_CTX_ORIGIN_PLATFORM || origin == AIMEE_CTX_ORIGIN_OPERATOR ||
             origin == AIMEE_CTX_ORIGIN_USER;
   return 1;
}

static int classify_blocks(aimee_block_t *blocks, int count, aimee_context_origin_t origin,
                           aimee_context_authority_t authority, aimee_context_trust_t trust)
{
   int changed = 0;
   for (int i = 0; blocks && i < count; i++)
   {
      aimee_block_t *block = &blocks[i];
      if (block->context.origin != AIMEE_CTX_ORIGIN_UNKNOWN)
         continue;
      aimee_context_origin_t block_origin = origin;
      aimee_context_authority_t block_authority = authority;
      if (block->type == AIMEE_BLK_TOOL_RESULT)
      {
         block_origin = AIMEE_CTX_ORIGIN_TOOL;
         block_authority = AIMEE_CTX_AUTH_EVIDENCE;
      }
      else if (block->type == AIMEE_BLK_THINKING)
      {
         block_origin = AIMEE_CTX_ORIGIN_MODEL;
         block_authority = AIMEE_CTX_AUTH_DIAGNOSTIC;
      }
      aimee_ir_block_set_context(block, block_origin, block_authority, trust,
                                 AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL, 0);
      changed++;
   }
   return changed;
}

int aimee_ir_classify_context(aimee_request_t *request)
{
   if (!request)
      return 0;
   int changed = classify_blocks(request->system, request->n_system, AIMEE_CTX_ORIGIN_USER,
                                 AIMEE_CTX_AUTH_TASK_INSTRUCTION, AIMEE_CTX_TRUST_UNVERIFIED);
   for (int i = 0; i < request->n_messages; i++)
   {
      aimee_message_t *message = &request->messages[i];
      aimee_context_origin_t origin = AIMEE_CTX_ORIGIN_UNKNOWN;
      aimee_context_authority_t authority = AIMEE_CTX_AUTH_EVIDENCE;
      aimee_context_trust_t trust = AIMEE_CTX_TRUST_UNVERIFIED;
      if (message->role && strcmp(message->role, "user") == 0)
      {
         origin = AIMEE_CTX_ORIGIN_USER;
         authority = AIMEE_CTX_AUTH_TASK_INSTRUCTION;
      }
      else if (message->role && strcmp(message->role, "assistant") == 0)
         origin = AIMEE_CTX_ORIGIN_MODEL;
      else if (message->role && strcmp(message->role, "tool") == 0)
         origin = AIMEE_CTX_ORIGIN_TOOL;
      changed += classify_blocks(message->blocks, message->n_blocks, origin, authority, trust);
   }
   return changed;
}

static void free_str(char *s)
{
   free(s);
}

/* Canonical comparison views never replace opaque model/tool JSON. They make
 * equivalent object ordering and exact decimal spellings stable for hashes. */
static char *json_decimal_key(const cJSON *number)
{
   char *printed = cJSON_PrintUnformatted(number);
   if (!printed)
      return NULL;
   if (!strcmp(printed, "null"))
      return printed;
   size_t length = strlen(printed), fractional = 0, count = 0;
   char *digits = cJSON_malloc(length + 1);
   if (!digits)
   {
      cJSON_free(printed);
      return NULL;
   }
   const char *p = printed;
   int negative = *p == '-', fraction = 0, exponent_negative = 0;
   if (negative)
      p++;
   while (*p && *p != 'e' && *p != 'E')
   {
      if (*p == '.')
         fraction = 1;
      else
      {
         digits[count++] = *p;
         if (fraction)
            fractional++;
      }
      p++;
   }
   long long exponent = 0;
   if (*p)
   {
      p++;
      if (*p == '+' || *p == '-')
      {
         exponent_negative = *p == '-';
         p++;
      }
      for (; *p; p++)
      {
         int digit = *p - '0';
         if (exponent > (LLONG_MAX - digit) / 10)
         {
            cJSON_free(digits);
            return printed;
         }
         exponent = exponent * 10 + digit;
      }
   }
   if (exponent_negative)
      exponent = -exponent;
   size_t first = 0, last = count;
   while (first < last && digits[first] == '0')
      first++;
   if (first == last)
   {
      cJSON_free(digits);
      cJSON_free(printed);
      char *zero = cJSON_malloc(2);
      if (zero)
         memcpy(zero, "0", 2);
      return zero;
   }
   while (last > first && digits[last - 1] == '0')
      last--;
   size_t trailing = count - last;
   if (fractional > (unsigned long long)LLONG_MAX || trailing > (unsigned long long)LLONG_MAX ||
       exponent < LLONG_MIN + (long long)fractional ||
       exponent - (long long)fractional > LLONG_MAX - (long long)trailing)
   {
      cJSON_free(digits);
      return printed;
   }
   exponent = exponent - (long long)fractional + (long long)trailing;
   char *key = cJSON_malloc(length + 40);
   if (key)
   {
      size_t offset = 0;
      if (negative)
         key[offset++] = '-';
      memcpy(key + offset, digits + first, last - first);
      offset += last - first;
      snprintf(key + offset, length + 40 - offset, "e%lld", exponent);
   }
   cJSON_free(digits);
   cJSON_free(printed);
   return key;
}
static int json_member_compare(const void *left, const void *right)
{
   const cJSON *a = *(const cJSON *const *)left, *b = *(const cJSON *const *)right;
   return strcmp(a->string, b->string);
}
static int json_compare_view(cJSON *node, int normalize, int depth)
{
   if (!node || depth > 512)
      return -1;
   if (cJSON_IsNumber(node) && normalize)
   {
      char *key = json_decimal_key(node);
      if (!key)
         return -1;
      cJSON_free(node->number_lexeme);
      node->number_lexeme = key;
   }
   if (cJSON_IsObject(node))
   {
      size_t count = 0;
      cJSON *child;
      cJSON_ArrayForEach(child, node)
      {
         if (!child->string)
            return -1;
         count++;
      }
      if (count > SIZE_MAX / sizeof(cJSON *))
         return -1;
      cJSON **members = count ? malloc(count * sizeof *members) : NULL;
      if (count && !members)
         return -1;
      size_t index = 0;
      cJSON_ArrayForEach(child, node) members[index++] = child;
      if (count)
         qsort(members, count, sizeof *members, json_member_compare);
      for (size_t i = 1; i < count; i++)
         if (!strcmp(members[i - 1]->string, members[i]->string))
         {
            free(members);
            return -1;
         }
      if (normalize && count)
      {
         node->child = members[0];
         for (size_t i = 0; i < count; i++)
         {
            members[i]->next = i + 1 < count ? members[i + 1] : NULL;
            members[i]->prev = i ? members[i - 1] : members[count - 1];
         }
      }
      free(members);
   }
   if (cJSON_IsObject(node) || cJSON_IsArray(node))
   {
      cJSON *child;
      cJSON_ArrayForEach(child, node) if (json_compare_view(child, normalize, depth + 1)) return -1;
   }
   return 0;
}
int aimee_ir_json_unique(const cJSON *json)
{
   return json_compare_view((cJSON *)json, 0, 0) == 0;
}
cJSON *aimee_ir_json_canonical_copy(const cJSON *json)
{
   cJSON *copy = cJSON_Duplicate(json, 1);
   if (!copy || json_compare_view(copy, 1, 0))
   {
      cJSON_Delete(copy);
      return NULL;
   }
   return copy;
}

int aimee_ir_thinking_binding_valid(const char *binding)
{
   if (!binding)
      return 1;
   if (strlen(binding) != 198 || strncmp(binding, "ap1:", 4) ||
       (binding[68] != ':' || binding[133] != ':'))
      return 0;
   for (int i = 4; i < 198; i++)
      if (i != 68 && i != 133 &&
          !((binding[i] >= '0' && binding[i] <= '9') || (binding[i] >= 'a' && binding[i] <= 'f')))
         return 0;
   return 1;
}

int aimee_ir_block_valid(const aimee_block_t *block)
{
   if (!block || block->type < AIMEE_BLK_TEXT || block->type >= AIMEE_BLK_UNKNOWN ||
       block->message_group < 0 || block->message_group > 4096)
      return 0;
   if ((block->type == AIMEE_BLK_TOOL_USE || block->type == AIMEE_BLK_TOOL_RESULT) &&
       (!block->tool_id || !block->tool_id[0]))
      return 0;
   if ((block->tool_input && !aimee_ir_json_unique(block->tool_input)) ||
       (block->tool_result && !aimee_ir_json_unique(block->tool_result)))
      return 0;
   if (block->thinking_binding && (block->type != AIMEE_BLK_THINKING ||
                                   !aimee_ir_thinking_binding_valid(block->thinking_binding)))
      return 0;
   if (block->type == AIMEE_BLK_TOOL_USE && (!block->tool_name || !block->tool_name[0]))
      return 0;
   if ((block->type == AIMEE_BLK_IMAGE || block->type == AIMEE_BLK_DOCUMENT) &&
       (!block->media_ref || !block->media_ref[0]))
      return 0;
   return 1;
}

void aimee_block_free_contents(aimee_block_t *b)
{
   if (!b)
      return;
   free_str(b->text);
   free_str(b->thinking_signature);
   free_str(b->thinking_binding);
   free_str(b->thinking_id);
   free_str(b->thinking_encrypted_content);
   free_str(b->thinking_redacted);
   free_str(b->tool_id);
   free_str(b->tool_name);
   free_str(b->tool_namespace);
   cJSON_Delete(b->tool_input);
   cJSON_Delete(b->tool_result);
   free_str(b->media_type);
   free_str(b->media_ref);
   free_str(b->cache_control);
   free_str(b->cache_ttl);
   cJSON_Delete(b->raw);
   memset(b, 0, sizeof *b);
}

static void free_blocks(aimee_block_t *blocks, int n)
{
   if (!blocks)
      return;
   for (int i = 0; i < n; i++)
      aimee_block_free_contents(&blocks[i]);
   free(blocks);
}

void aimee_message_free(aimee_message_t *m)
{
   if (!m)
      return;
   free_str(m->role);
   free_blocks(m->blocks, m->n_blocks);
   cJSON_Delete(m->raw);
   memset(m, 0, sizeof(*m));
}

const char *aimee_ir_effort_name(aimee_reasoning_effort_t effort)
{
   static const char *const names[] = {NULL,     "none", "minimal", "low",
                                       "medium", "high", "xhigh",   "max"};
   return effort > AIMEE_EFFORT_UNSET && effort <= AIMEE_EFFORT_MAX ? names[effort] : NULL;
}
int aimee_ir_effort_parse(const char *name, aimee_reasoning_effort_t *effort)
{
   if (!effort)
      return -1;
   for (int i = AIMEE_EFFORT_NONE; name && i <= AIMEE_EFFORT_MAX; i++)
      if (!strcmp(name, aimee_ir_effort_name((aimee_reasoning_effort_t)i)))
      {
         *effort = (aimee_reasoning_effort_t)i;
         return 0;
      }
   return -1;
}

void aimee_tool_free(aimee_tool_t *tool)
{
   if (!tool)
      return;
   free(tool->name);
   free(tool->description);
   cJSON_Delete(tool->schema);
   free(tool->cache_control);
   free(tool->cache_ttl);
   free(tool->native_type);
   cJSON_Delete(tool->native_options);
   cJSON_Delete(tool->raw);
   for (int i = 0; i < tool->n_children; i++)
      aimee_tool_free(&tool->children[i]);
   free(tool->children);
   memset(tool, 0, sizeof *tool);
}
int aimee_ir_tool_valid(const aimee_tool_t *tool)
{
   if (!tool || tool->strict < AIMEE_STRICT_UNSET || tool->strict > AIMEE_STRICT_TRUE ||
       (tool->native_options &&
        (!cJSON_IsObject(tool->native_options) || !aimee_ir_json_unique(tool->native_options))) ||
       (tool->schema && (!cJSON_IsObject(tool->schema) || !aimee_ir_json_unique(tool->schema))))
      return 0;
   if (tool->kind == AIMEE_TOOL_HOSTED)
      return tool->native_type && tool->native_type[0] &&
             (tool->provider_wire == AIMEE_WIRE_RESPONSES ||
              tool->provider_wire == AIMEE_WIRE_ANTHROPIC) &&
             !tool->n_children &&
             (tool->provider_wire != AIMEE_WIRE_ANTHROPIC || (tool->name && tool->name[0]));
   if (!tool->name || !tool->name[0] || (tool->schema && !cJSON_IsObject(tool->schema)))
      return 0;
   if (tool->kind == AIMEE_TOOL_FUNCTION)
      return !tool->n_children;
   if (tool->kind != AIMEE_TOOL_NAMESPACE || tool->n_children < 0 ||
       tool->n_children > AIMEE_IR_MAX_BLOCKS || (tool->n_children && !tool->children))
      return 0;
   for (int i = 0; i < tool->n_children; i++)
      if (tool->children[i].kind != AIMEE_TOOL_FUNCTION || !aimee_ir_tool_valid(&tool->children[i]))
         return 0;
   return 1;
}

void aimee_request_free(aimee_request_t *r)
{
   if (!r)
      return;
   free_str(r->model);
   free_blocks(r->system, r->n_system);
   if (r->messages)
   {
      for (int i = 0; i < r->n_messages; i++)
         aimee_message_free(&r->messages[i]);
      free(r->messages);
   }
   if (r->tools)
   {
      for (int i = 0; i < r->n_tools; i++)
      {
         aimee_tool_free(&r->tools[i]);
      }
      free(r->tools);
   }
   free_str(r->reasoning_summary);
   free_str(r->output_format.name);
   free_str(r->output_format.description);
   cJSON_Delete(r->output_format.schema);
   free_str(r->tool_choice_name);
   cJSON_Delete(r->metadata);
   free_str(r->service_tier);
   cJSON_Delete(r->thinking);
   if (r->stop_sequences)
   {
      for (int i = 0; i < r->n_stop; i++)
         free_str(r->stop_sequences[i]);
      free(r->stop_sequences);
   }
   cJSON_Delete(r->raw);
   free(r->raw_bytes);
   memset(r, 0, sizeof *r);
}

void aimee_response_free(aimee_response_t *r)
{
   if (!r)
      return;
   free_str(r->id);
   free_str(r->model);
   free_str(r->role);
   free_blocks(r->content, r->n_content);
   free_str(r->raw_stop_reason);
   free_str(r->error_message);
   cJSON_Delete(r->raw);
   memset(r, 0, sizeof *r);
}

/* Execute a module's tool-index patch. Validate the whole patch first so a
 * malformed or stale plan cannot partially mutate the request. */
int aimee_ir_tool_paths_valid(const aimee_request_t *request, const cJSON *paths)
{
   if (!request || !cJSON_IsArray(paths) || cJSON_GetArraySize(paths) > AIMEE_IR_MAX_BLOCKS)
      return 0;
   const cJSON *path, *previous = NULL;
   cJSON_ArrayForEach(path, paths)
   {
      int depth = cJSON_GetArraySize(path);
      if (!cJSON_IsArray(path) || depth < 1 || depth > 16)
         return 0;
      const aimee_tool_t *tools = request->tools;
      int count = request->n_tools;
      for (int i = 0; i < depth; i++)
      {
         const cJSON *index = cJSON_GetArrayItem(path, i);
         if (!tools || !cJSON_IsNumber(index) || index->valuedouble < 0 ||
             index->valuedouble >= count || index->valuedouble != index->valueint)
            return 0;
         const aimee_tool_t *tool = &tools[index->valueint];
         if (i + 1 < depth && tool->kind != AIMEE_TOOL_NAMESPACE)
            return 0;
         tools = tool->children;
         count = tool->n_children;
      }
      if (previous)
      {
         int n = cJSON_GetArraySize(previous), different = 0;
         for (int i = 0; i < n && i < depth; i++)
         {
            int left = cJSON_GetArrayItem(previous, i)->valueint;
            int right = cJSON_GetArrayItem(path, i)->valueint;
            if (left < right)
               return 0;
            if (left > right)
            {
               different = 1;
               break;
            }
         }
         if (!different)
            return 0; /* Duplicate or overlapping parent/child paths. */
      }
      previous = path;
   }
   return 1;
}
static void relax_removed_tool_choice(aimee_request_t *request, const aimee_tool_t *tool)
{
   if (request->tool_choice == AIMEE_TOOL_CHOICE_TOOL && request->tool_choice_name && tool->name &&
       !strcmp(tool->name, request->tool_choice_name))
   {
      request->tool_choice = AIMEE_TOOL_CHOICE_AUTO;
      free(request->tool_choice_name);
      request->tool_choice_name = NULL;
   }
   for (int i = 0; i < tool->n_children; i++)
      relax_removed_tool_choice(request, &tool->children[i]);
}
static void prune_empty_tool_namespaces(aimee_tool_t *tools, int *count)
{
   for (int i = *count - 1; i >= 0; i--)
      if (tools[i].kind == AIMEE_TOOL_NAMESPACE)
      {
         prune_empty_tool_namespaces(tools[i].children, &tools[i].n_children);
         if (!tools[i].n_children)
         {
            aimee_tool_free(&tools[i]);
            memmove(&tools[i], &tools[i + 1], (size_t)(*count - i - 1) * sizeof *tools);
            (*count)--;
            memset(&tools[*count], 0, sizeof *tools);
         }
      }
}
int aimee_ir_remove_tool_paths(aimee_request_t *request, const cJSON *paths)
{
   if (!aimee_ir_tool_paths_valid(request, paths))
      return -1;
   int removed = 0;
   const cJSON *path;
   cJSON_ArrayForEach(path, paths)
   {
      aimee_tool_t *tools = request->tools;
      int *count = &request->n_tools;
      int depth = cJSON_GetArraySize(path);
      for (int i = 0; i < depth - 1; i++)
      {
         aimee_tool_t *parent = &tools[cJSON_GetArrayItem(path, i)->valueint];
         tools = parent->children;
         count = &parent->n_children;
      }
      int index = cJSON_GetArrayItem(path, depth - 1)->valueint;
      relax_removed_tool_choice(request, &tools[index]);
      aimee_tool_free(&tools[index]);
      memmove(&tools[index], &tools[index + 1], (size_t)(*count - index - 1) * sizeof *tools);
      (*count)--;
      memset(&tools[*count], 0, sizeof *tools);
      removed++;
   }
   if (removed)
   {
      prune_empty_tool_namespaces(request->tools, &request->n_tools);
      if (!request->n_tools)
      {
         request->tool_choice = AIMEE_TOOL_CHOICE_UNSET;
         request->has_parallel_tool_calls = 0;
         free(request->tool_choice_name);
         request->tool_choice_name = NULL;
      }
      request->mutated = 1;
   }
   return removed;
}

size_t aimee_ir_last_user_text(const aimee_request_t *r, char *buf, size_t n)
{
   if (buf && n)
      buf[0] = '\0';
   if (!r || !buf || !n)
      return 0;
   /* find the LAST user-role message */
   const aimee_message_t *last = NULL;
   for (int i = 0; i < r->n_messages; i++)
      if (r->messages[i].role && strcmp(r->messages[i].role, "user") == 0)
         last = &r->messages[i];
   if (!last)
      return 0;
   /* concat its TEXT blocks (shape-agnostic; no silent drop of the message -- a
    * message that is only tool_result yields empty, which is correct). */
   size_t used = 0;
   for (int i = 0; i < last->n_blocks && used + 1 < n; i++)
   {
      const aimee_block_t *b = &last->blocks[i];
      if (b->type != AIMEE_BLK_TEXT || !b->text)
         continue;
      size_t l = strlen(b->text);
      if (l > n - 1 - used)
         l = n - 1 - used;
      memcpy(buf + used, b->text, l);
      used += l;
   }
   buf[used] = '\0';
   return used;
}

const char *aimee_stop_reason_name(aimee_stop_reason_t s)
{
   switch (s)
   {
   case AIMEE_STOP_END_TURN:
      return "end_turn";
   case AIMEE_STOP_MAX_TOKENS:
      return "max_tokens";
   case AIMEE_STOP_TOOL_USE:
      return "tool_use";
   case AIMEE_STOP_STOP_SEQUENCE:
      return "stop_sequence";
   case AIMEE_STOP_CONTENT_FILTER:
      return "content_filter";
   case AIMEE_STOP_ERROR:
      return "error";
   case AIMEE_STOP_UNKNOWN:
   default:
      return "unknown";
   }
}

static int str_eq(const char *a, const char *b)
{
   if (a == b)
      return 1;
   if (!a || !b)
      return 0;
   return strcmp(a, b) == 0;
}

static int json_eq(const struct cJSON *a, const struct cJSON *b)
{
   if (a == b)
      return 1;
   if (!a || !b)
      return 0;
   return cJSON_Compare((const cJSON *)a, (const cJSON *)b, 1 /* case-sensitive */);
}

static int block_eq(const aimee_block_t *a, const aimee_block_t *b)
{
   if (a->type != b->type || a->message_group != b->message_group)
      return 0;
   if (a->context.origin != b->context.origin || a->context.authority != b->context.authority ||
       a->context.trust != b->context.trust || a->context.sensitivity != b->context.sensitivity ||
       a->context.model_visible != b->context.model_visible ||
       strcmp(a->context.revision_domain, b->context.revision_domain) != 0 ||
       strcmp(a->context.revision_scope, b->context.revision_scope) != 0 ||
       a->context.revision_epoch != b->context.revision_epoch)
      return 0;
   if (!str_eq(a->cache_control, b->cache_control) || !str_eq(a->cache_ttl, b->cache_ttl))
      return 0;
   switch (a->type)
   {
   case AIMEE_BLK_TEXT:
      return str_eq(a->text, b->text);
   case AIMEE_BLK_THINKING:
      return str_eq(a->text, b->text) && str_eq(a->thinking_signature, b->thinking_signature) &&
             str_eq(a->thinking_binding, b->thinking_binding) &&
             str_eq(a->thinking_id, b->thinking_id) &&
             str_eq(a->thinking_encrypted_content, b->thinking_encrypted_content) &&
             str_eq(a->thinking_redacted, b->thinking_redacted);
   case AIMEE_BLK_TOOL_USE:
      /* The namespace is part of the call's identity: two calls to the same bare
       * name in different groups are different calls. */
      return str_eq(a->tool_id, b->tool_id) && str_eq(a->tool_name, b->tool_name) &&
             str_eq(a->tool_namespace, b->tool_namespace) && json_eq(a->tool_input, b->tool_input);
   case AIMEE_BLK_TOOL_RESULT:
      return str_eq(a->tool_id, b->tool_id) && a->tool_is_error == b->tool_is_error &&
             json_eq(a->tool_result, b->tool_result);
   case AIMEE_BLK_IMAGE:
   case AIMEE_BLK_DOCUMENT:
      return str_eq(a->media_type, b->media_type) && str_eq(a->media_ref, b->media_ref);
   case AIMEE_BLK_UNKNOWN:
   default:
      return json_eq(a->raw, b->raw);
   }
}

static int blocks_eq(const aimee_block_t *a, int na, const aimee_block_t *b, int nb, int wire)
{
   if (na != nb)
      return 0;
   for (int i = 0; i < na; i++)
   {
      aimee_block_t left = a[i], right = b[i];
      if (wire)
      {
         memset(&left.context, 0, sizeof left.context);
         memset(&right.context, 0, sizeof right.context);
         left.thinking_binding = right.thinking_binding = NULL;
      }
      if (!block_eq(&left, &right))
         return 0;
   }
   return 1;
}

static int tools_equal(const aimee_tool_t *a, const aimee_tool_t *b, int count)
{
   for (int i = 0; i < count; i++)
      if (a[i].kind != b[i].kind || a[i].provider_wire != b[i].provider_wire ||
          !str_eq(a[i].name, b[i].name) || !str_eq(a[i].description, b[i].description) ||
          !str_eq(a[i].cache_control, b[i].cache_control) ||
          !str_eq(a[i].cache_ttl, b[i].cache_ttl) || !json_eq(a[i].schema, b[i].schema) ||
          !str_eq(a[i].native_type, b[i].native_type) ||
          !json_eq(a[i].native_options, b[i].native_options) || a[i].strict != b[i].strict ||
          a[i].has_defer_loading != b[i].has_defer_loading ||
          (a[i].has_defer_loading && a[i].defer_loading != b[i].defer_loading) ||
          a[i].n_children != b[i].n_children ||
          !tools_equal(a[i].children, b[i].children, a[i].n_children))
         return 0;
   return 1;
}

static int request_equal(const aimee_request_t *a, const aimee_request_t *b, int wire)
{
   if (a == b)
      return 1;
   if (!a || !b)
      return 0;
   if (a->cache_policy != b->cache_policy)
      return 0;
   if (!blocks_eq(a->system, a->n_system, b->system, b->n_system, wire))
      return 0;
   if (a->n_messages != b->n_messages)
      return 0;
   for (int i = 0; i < a->n_messages; i++)
   {
      if ((wire && a->messages[i].content_scalar != b->messages[i].content_scalar) ||
          !str_eq(a->messages[i].role, b->messages[i].role))
         return 0;
      if (!blocks_eq(a->messages[i].blocks, a->messages[i].n_blocks, b->messages[i].blocks,
                     b->messages[i].n_blocks, wire))
         return 0;
   }
   if (a->n_tools != b->n_tools)
      return 0;
   if (!tools_equal(a->tools, b->tools, a->n_tools))
      return 0;
   if (a->reasoning_effort != b->reasoning_effort ||
       !str_eq(a->reasoning_summary, b->reasoning_summary))
      return 0;
   if (a->output_format.kind != b->output_format.kind ||
       a->output_format.strict != b->output_format.strict ||
       !str_eq(a->output_format.name, b->output_format.name) ||
       !str_eq(a->output_format.description, b->output_format.description) ||
       !json_eq(a->output_format.schema, b->output_format.schema))
      return 0;
   if (a->tool_choice != b->tool_choice || (a->tool_choice == AIMEE_TOOL_CHOICE_TOOL &&
                                            !str_eq(a->tool_choice_name, b->tool_choice_name)))
      return 0;
   if (a->has_parallel_tool_calls != b->has_parallel_tool_calls ||
       (a->has_parallel_tool_calls && !a->parallel_tool_calls != !b->parallel_tool_calls))
      return 0;
   /* sampling params (content, not provenance) */
   if (a->has_max_tokens != b->has_max_tokens ||
       (a->has_max_tokens && a->max_tokens != b->max_tokens))
      return 0;
   if (a->has_temperature != b->has_temperature ||
       (a->has_temperature && a->temperature != b->temperature))
      return 0;
   if (a->has_top_p != b->has_top_p || (a->has_top_p && a->top_p != b->top_p))
      return 0;
   if (a->has_top_k != b->has_top_k || (a->has_top_k && a->top_k != b->top_k))
      return 0;
   if (!json_eq(a->metadata, b->metadata))
      return 0;
   if (!str_eq(a->service_tier, b->service_tier))
      return 0;
   if (!json_eq(a->thinking, b->thinking))
      return 0;
   if (a->stream != b->stream || a->n_stop != b->n_stop)
      return 0;
   for (int i = 0; i < a->n_stop; i++)
      if (!str_eq(a->stop_sequences[i], b->stop_sequences[i]))
         return 0;
   /* IGNORED: frontend wire tag, raw sidecar, model string (provenance). */
   return 1;
}

int aimee_ir_request_equal(const aimee_request_t *a, const aimee_request_t *b)
{
   return request_equal(a, b, 0);
}
int aimee_ir_request_wire_equal(const aimee_request_t *a, const aimee_request_t *b)
{
   return request_equal(a, b, 1);
}

aimee_stop_reason_t aimee_stop_reason_parse(const char *name)
{
   if (!name)
      return AIMEE_STOP_UNKNOWN;
   if (strcmp(name, "end_turn") == 0)
      return AIMEE_STOP_END_TURN;
   if (strcmp(name, "max_tokens") == 0)
      return AIMEE_STOP_MAX_TOKENS;
   if (strcmp(name, "tool_use") == 0)
      return AIMEE_STOP_TOOL_USE;
   if (strcmp(name, "stop_sequence") == 0)
      return AIMEE_STOP_STOP_SEQUENCE;
   if (strcmp(name, "content_filter") == 0)
      return AIMEE_STOP_CONTENT_FILTER;
   if (strcmp(name, "error") == 0)
      return AIMEE_STOP_ERROR;
   return AIMEE_STOP_UNKNOWN;
}

/* --- response-side accessors (see aimee_ir.h) --- */

/* Shared block-text concatenator: appends every block of `want` type, truncating
 * at n-1. Factored because text/reasoning differ only in the type they select --
 * and keeping one copy means the truncation rule cannot drift between them. */
static size_t ir_concat_blocks(const aimee_block_t *blocks, int n_blocks, aimee_block_type_t want,
                               char *buf, size_t n)
{
   size_t used = 0;
   int previous_group = 0;
   for (int i = 0; i < n_blocks && used + 1 < n; i++)
   {
      const aimee_block_t *b = &blocks[i];
      if (b->type != want || !b->text || !b->text[0])
         continue;
      if (want == AIMEE_BLK_TEXT && used && previous_group > 0 && b->message_group > 0 &&
          previous_group != b->message_group)
         for (int newline = 0; newline < 2 && used + 1 < n; newline++)
            buf[used++] = '\n';
      previous_group = b->message_group;
      size_t l = strlen(b->text);
      if (l > n - 1 - used)
         l = n - 1 - used;
      memcpy(buf + used, b->text, l);
      used += l;
   }
   buf[used] = '\0';
   return used;
}

int aimee_ir_response_from_text(aimee_response_t *out, const char *text, const char *model)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   aimee_block_t *blk = calloc(1, sizeof *blk);
   out->role = strdup("assistant");
   out->raw_stop_reason = strdup("end_turn");
   if (!blk || !out->role || !out->raw_stop_reason)
   {
      free(blk);
      aimee_response_free(out);
      return -1;
   }
   blk->type = AIMEE_BLK_TEXT;
   blk->text = strdup(text ? text : "");
   if (!blk->text)
   {
      free(blk);
      aimee_response_free(out);
      return -1;
   }
   out->content = blk; /* owned single-element array; freed by aimee_response_free */
   out->n_content = 1;
   out->stop_reason = AIMEE_STOP_END_TURN;
   if (model && model[0])
      out->model = strdup(model); /* best-effort; NULL is fine */
   return 0;
}

size_t aimee_ir_response_text(const aimee_response_t *r, char *buf, size_t n)
{
   if (buf && n)
      buf[0] = '\0';
   if (!r || !buf || !n)
      return 0;
   /* TEXT only. A response whose blocks are all TOOL_USE yields empty, which is
    * correct -- it said nothing, it asked to call something. */
   return ir_concat_blocks(r->content, r->n_content, AIMEE_BLK_TEXT, buf, n);
}

size_t aimee_ir_response_reasoning(const aimee_response_t *r, char *buf, size_t n)
{
   if (buf && n)
      buf[0] = '\0';
   if (!r || !buf || !n)
      return 0;
   return ir_concat_blocks(r->content, r->n_content, AIMEE_BLK_THINKING, buf, n);
}

int aimee_ir_response_has_tool_use(const aimee_response_t *r)
{
   if (!r)
      return 0;
   for (int i = 0; i < r->n_content; i++)
      if (r->content[i].type == AIMEE_BLK_TOOL_USE)
         return 1;
   return 0;
}

int aimee_ir_response_tool_count(const aimee_response_t *response)
{
   int count = 0;
   if (response)
      for (int i = 0; i < response->n_content; i++)
         count += response->content[i].type == AIMEE_BLK_TOOL_USE;
   return count;
}

aimee_block_t *aimee_ir_response_tool(aimee_response_t *response, int ordinal)
{
   if (!response || ordinal < 0)
      return NULL;
   for (int i = 0; i < response->n_content; i++)
      if (response->content[i].type == AIMEE_BLK_TOOL_USE && ordinal-- == 0)
         return &response->content[i];
   return NULL;
}

int aimee_ir_response_drop_tool(aimee_response_t *response, int ordinal)
{
   aimee_block_t *block = aimee_ir_response_tool(response, ordinal);
   if (!block)
      return -1;
   int index = (int)(block - response->content);
   aimee_block_free_contents(block);
   memmove(block, block + 1, (size_t)(response->n_content - index - 1) * sizeof(*block));
   response->n_content--;
   memset(&response->content[response->n_content], 0, sizeof(*block));
   /* An edited response must not replay its native sidecar. */
   cJSON_Delete(response->raw);
   response->raw = NULL;
   return 0;
}

char *aimee_ir_media_url(const aimee_block_t *block)
{
   if (!block || !block->media_ref)
      return NULL;
   const char *ref = block->media_ref;
   if (!block->media_type || !strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8) ||
       !strncmp(ref, "data:", 5) || !strncmp(ref, "file-", 5))
      return strdup(ref);
   size_t type_len = strlen(block->media_type), ref_len = strlen(ref);
   if (type_len > SIZE_MAX - 16 || ref_len > SIZE_MAX - type_len - 16)
      return NULL;
   size_t size = type_len + ref_len + 16;
   char *url = malloc(size);
   if (url)
      snprintf(url, size, "data:%s;base64,%s", block->media_type, ref);
   return url;
}

char *aimee_ir_response_text_alloc(const aimee_response_t *response)
{
   size_t length = 1;
   if (!response)
      return NULL;
   int previous_group = 0;
   for (int i = 0; i < response->n_content; i++)
      if (response->content[i].type == AIMEE_BLK_TEXT && response->content[i].text &&
          response->content[i].text[0])
      {
         const aimee_block_t *block = &response->content[i];
         size_t separator = length > 1 && previous_group > 0 && block->message_group > 0 &&
                                    previous_group != block->message_group
                                ? 2
                                : 0;
         size_t n = strlen(block->text);
         if (separator > SIZE_MAX - length || n > SIZE_MAX - length - separator)
            return NULL;
         length += separator + n;
         previous_group = block->message_group;
      }
   char *text = malloc(length);
   if (text)
      aimee_ir_response_text(response, text, length);
   return text;
}

void aimee_ir_run_transforms(aimee_request_t *ir, const aimee_ir_transform_t *stages, size_t n)
{
   if (!ir || !stages)
      return;
   for (size_t i = 0; i < n; i++)
   {
      const aimee_ir_transform_t *s = &stages[i];
      if (!s->enabled || !s->fn || !s->name || !s->name[0])
         continue;
      if (s->fn(ir, s->ud))
         ir->mutated = 1; /* a change means the raw sidecar is stale -> backend rebuilds */
   }
}

int aimee_ir_response_admit(aimee_response_t *response)
{
   if (!response || response->n_content < 0 || response->n_content > AIMEE_IR_MAX_BLOCKS ||
       (response->n_content && !response->content))
      return -1;
   if (response->role && strcmp(response->role, "assistant"))
      return -1;
   if (response->usage_in < 0 || response->usage_out < 0 || response->usage_cache_read < 0 ||
       response->usage_cache_write < 0 || response->usage_reasoning < 0)
      return -1;
   for (int i = 0; i < response->n_content; i++)
   {
      const aimee_block_t *block = &response->content[i];
      if (!aimee_ir_block_valid(block) || block->type == AIMEE_BLK_TOOL_RESULT)
         return -1;
      if (block->type == AIMEE_BLK_TOOL_USE && !cJSON_IsObject(block->tool_input))
         return -1;
      if (block->type == AIMEE_BLK_TOOL_USE)
         for (int j = 0; j < i; j++)
            if (response->content[j].type == AIMEE_BLK_TOOL_USE &&
                !strcmp(block->tool_id, response->content[j].tool_id))
               return -1;
   }
   aimee_message_t message = {.role = response->role ? response->role : "assistant",
                              .blocks = response->content,
                              .n_blocks = response->n_content};
   aimee_request_t request = {.messages = &message, .n_messages = 1};
   aimee_ir_classify_context(&request);
   return 0;
}

int aimee_ir_read_usage(const cJSON *usage, const char *name, long *out)
{
   if (!out || !name || (usage && !cJSON_IsNull(usage) && !cJSON_IsObject(usage)))
      return -1;
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(usage, name);
   if (!value)
      return 0;
   if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0 ||
       value->valuedouble > 9007199254740991.0 || value->valuedouble > LONG_MAX ||
       (double)(long)value->valuedouble != value->valuedouble)
      return -1;
   *out = (long)value->valuedouble;
   return 0;
}

int aimee_ir_request_admit(const aimee_request_t *request)
{
   if (!request || request->n_system < 0 || request->n_messages < 0 || request->n_tools < 0 ||
       request->n_system > AIMEE_IR_MAX_BLOCKS || request->n_messages > AIMEE_IR_MAX_BLOCKS ||
       request->n_tools > AIMEE_IR_MAX_BLOCKS || (request->n_system && !request->system) ||
       (request->n_messages && !request->messages) || (request->n_tools && !request->tools))
      return -1;
   if (request->frontend < AIMEE_WIRE_UNKNOWN || request->frontend > AIMEE_WIRE_RESPONSES ||
       request->cache_policy < AIMEE_CACHE_DEFAULT ||
       request->cache_policy > AIMEE_CACHE_STABLE_PREFIX ||
       request->tool_choice < AIMEE_TOOL_CHOICE_UNSET ||
       request->tool_choice > AIMEE_TOOL_CHOICE_TOOL ||
       (request->tool_choice == AIMEE_TOOL_CHOICE_TOOL &&
        (!request->tool_choice_name || !request->tool_choice_name[0])) ||
       (request->has_max_tokens && request->max_tokens < 1) ||
       (request->has_temperature && !isfinite(request->temperature)) ||
       (request->has_top_p && !isfinite(request->top_p)) || request->n_stop < 0 ||
       request->n_stop > AIMEE_IR_MAX_BLOCKS || (request->n_stop && !request->stop_sequences) ||
       (request->thinking &&
        (!cJSON_IsObject(request->thinking) || !aimee_ir_json_unique(request->thinking))) ||
       (request->output_format.schema && !aimee_ir_json_unique(request->output_format.schema)) ||
       request->output_format.kind < AIMEE_OUTPUT_UNSET ||
       request->output_format.kind > AIMEE_OUTPUT_JSON_SCHEMA ||
       request->output_format.strict < AIMEE_STRICT_UNSET ||
       request->output_format.strict > AIMEE_STRICT_TRUE ||
       (request->output_format.kind == AIMEE_OUTPUT_JSON_SCHEMA &&
        !cJSON_IsObject(request->output_format.schema)))
      return -1;
   for (int i = 0; i < request->n_stop; i++)
      if (!request->stop_sequences[i])
         return -1;
   if (request->reasoning_effort < AIMEE_EFFORT_UNSET ||
       request->reasoning_effort > AIMEE_EFFORT_MAX ||
       (request->reasoning_summary && strcmp(request->reasoning_summary, "auto") &&
        strcmp(request->reasoning_summary, "concise") &&
        strcmp(request->reasoning_summary, "detailed")))
      return -1;
   for (int i = 0; i < request->n_system; i++)
      if (!aimee_ir_block_valid(&request->system[i]) ||
          request->system[i].type == AIMEE_BLK_TOOL_USE ||
          request->system[i].type == AIMEE_BLK_TOOL_RESULT)
         return -1;
   for (int i = 0; i < request->n_messages; i++)
   {
      const aimee_message_t *message = &request->messages[i];
      const char *role = message->role;
      if (!role ||
          (strcmp(role, "system") && strcmp(role, "developer") && strcmp(role, "user") &&
           strcmp(role, "assistant") && strcmp(role, "tool")) ||
          message->n_blocks < 0 || message->n_blocks > AIMEE_IR_MAX_BLOCKS ||
          (message->n_blocks && !message->blocks))
         return -1;
      if (message->content_scalar != 0 && message->content_scalar != 1)
         return -1;
      for (int j = 0; j < message->n_blocks; j++)
      {
         const aimee_block_t *block = &message->blocks[j];
         if (!aimee_ir_block_valid(block) ||
             (block->type == AIMEE_BLK_TOOL_USE && strcmp(role, "assistant")) ||
             (block->type == AIMEE_BLK_TOOL_RESULT && strcmp(role, "user") && strcmp(role, "tool")))
            return -1;
      }
   }
   for (int i = 0; i < request->n_tools; i++)
      if (!aimee_ir_tool_valid(&request->tools[i]))
         return -1;
   return 0;
}

/* Owned assembly of canonical model deltas. Provider parsing and native framing
 * stay outside this consumer; no text/native-message history reconstruction. */
#define IR_BUILDER_MAX_BYTES (16u * 1024u * 1024u)
struct aimee_ir_builder_buffer
{
   size_t length, capacity;
};
struct aimee_ir_builder_storage
{
   char *arguments;
   struct aimee_ir_builder_buffer args, text, signature;
   unsigned char stopped;
};
void aimee_ir_response_builder_free(aimee_ir_response_builder_t *builder)
{
   if (!builder)
      return;
   for (int i = 0; i < builder->response.n_content; i++)
      free(builder->storage[i].arguments);
   free(builder->storage);
   aimee_response_free(&builder->response);
   memset(builder, 0, sizeof *builder);
}
static int builder_append_buffer(aimee_ir_response_builder_t *builder, char **destination,
                                 struct aimee_ir_builder_buffer *buffer, const char *text)
{
   if (!text)
      return 0;
   size_t add = strlen(text);
   if (add > IR_BUILDER_MAX_BYTES || builder->bytes > IR_BUILDER_MAX_BYTES - add)
      return -1;
   size_t required = buffer->length + add + 1;
   if (required > buffer->capacity)
   {
      size_t capacity = buffer->capacity ? buffer->capacity : 64;
      while (capacity < required)
         capacity =
             capacity > (IR_BUILDER_MAX_BYTES + 1) / 2 ? IR_BUILDER_MAX_BYTES + 1 : capacity * 2;
      char *copy = realloc(*destination, capacity);
      if (!copy)
         return -1;
      *destination = copy;
      buffer->capacity = capacity;
   }
   memcpy(*destination + buffer->length, text, add + 1);
   buffer->length += add;
   builder->bytes += add;
   return 0;
}
/* Identifiers and redacted payloads arrive once, unlike streamed fragments. */
static int builder_append(aimee_ir_response_builder_t *builder, char **destination,
                          const char *text)
{
   struct aimee_ir_builder_buffer buffer = {0};
   if (*destination)
      return -1;
   return builder_append_buffer(builder, destination, &buffer, text);
}
static int builder_grow(aimee_ir_response_builder_t *builder)
{
   if ((size_t)builder->response.n_content < builder->capacity)
      return 0;
   size_t capacity = builder->capacity ? builder->capacity * 2 : 4;
   if (capacity > AIMEE_IR_MAX_BLOCKS)
      capacity = AIMEE_IR_MAX_BLOCKS;
   if (capacity <= builder->capacity)
      return -1;
   aimee_block_t *blocks = calloc(capacity, sizeof *blocks);
   struct aimee_ir_builder_storage *storage = calloc(capacity, sizeof *storage);
   if (!blocks || !storage)
   {
      free(blocks);
      free(storage);
      return -1;
   }
   size_t count = (size_t)builder->response.n_content;
   if (count)
   {
      memcpy(blocks, builder->response.content, count * sizeof *blocks);
      memcpy(storage, builder->storage, count * sizeof *storage);
   }
   free(builder->response.content);
   free(builder->storage);
   builder->response.content = blocks;
   builder->storage = storage;
   builder->capacity = capacity;
   return 0;
}
int aimee_ir_response_builder_apply(aimee_ir_response_builder_t *builder,
                                    const aimee_delta_t *delta)
{
   if (!builder || !delta || builder->complete)
      return -1;
   if (delta->type == AIMEE_DELTA_TURN_START)
   {
      if (builder->started)
         return -1;
      builder->response.role = strdup("assistant");
      if (!builder->response.role)
         return -1;
      builder->started = 1;
      return 0;
   }
   if (!builder->started)
      return -1;
   if (delta->type == AIMEE_DELTA_ERROR)
   {
      builder->response.error_message =
          strdup(delta->error_message ? delta->error_message : "Provider stream failed");
      if (!builder->response.error_message)
         return -1;
      builder->response.stop_reason = AIMEE_STOP_ERROR;
      builder->complete = 1;
      return 0;
   }
   if (delta->type == AIMEE_DELTA_TURN_STOP)
   {
      for (int i = 0; i < builder->response.n_content; i++)
         if (!builder->storage[i].stopped)
            return -1;
      builder->response.stop_reason = delta->stop_reason;
      builder->response.raw_stop_reason =
          delta->raw_stop_reason ? strdup(delta->raw_stop_reason) : NULL;
      if (delta->raw_stop_reason && !builder->response.raw_stop_reason)
         return -1;
      builder->response.usage_in = delta->usage_in;
      builder->response.usage_out = delta->usage_out;
      builder->response.usage_cache_read = delta->usage_cache_read;
      builder->response.usage_cache_write = delta->usage_cache_write;
      builder->response.usage_reasoning = delta->usage_reasoning;
      if (aimee_ir_response_admit(&builder->response))
         return -1;
      builder->complete = 1;
      return 0;
   }
   int index = delta->block_id;
   if (index < 0 || index >= AIMEE_IR_MAX_BLOCKS)
      return -1;
   if (delta->type == AIMEE_DELTA_BLOCK_START)
   {
      if ((delta->kind != AIMEE_BLK_THINKING &&
           (delta->signature_delta || delta->redacted_content)) ||
          (delta->kind == AIMEE_BLK_TOOL_USE && delta->text_delta) ||
          (delta->kind != AIMEE_BLK_TOOL_USE &&
           (delta->tool_id || delta->tool_name || delta->tool_namespace || delta->tool_input ||
            delta->tool_args_delta)))
         return -1;
      if (index != builder->response.n_content ||
          (delta->kind != AIMEE_BLK_TEXT && delta->kind != AIMEE_BLK_THINKING &&
           delta->kind != AIMEE_BLK_TOOL_USE) ||
          builder_grow(builder))
         return -1;
      aimee_block_t *block = &builder->response.content[builder->response.n_content++];
      block->type = delta->kind;
      if (delta->kind == AIMEE_BLK_TOOL_USE)
      {
         if (!delta->tool_id || !*delta->tool_id || !delta->tool_name || !*delta->tool_name ||
             (delta->tool_input && !cJSON_IsObject(delta->tool_input)) ||
             builder_append(builder, &block->tool_id, delta->tool_id) ||
             builder_append(builder, &block->tool_name, delta->tool_name) ||
             builder_append(builder, &block->tool_namespace, delta->tool_namespace))
            return -1;
         if (delta->tool_input)
         {
            char *json = cJSON_PrintUnformatted(delta->tool_input);
            if (!json)
               return -1;
            size_t size = strlen(json);
            free(json);
            if (size > IR_BUILDER_MAX_BYTES || builder->bytes > IR_BUILDER_MAX_BYTES - size)
               return -1;
            builder->bytes += size;
            block->tool_input = cJSON_Duplicate(delta->tool_input, 1);
            if (!block->tool_input)
               return -1;
         }
      }
      else if (builder_append_buffer(builder, &block->text, &builder->storage[index].text,
                                     delta->text_delta ? delta->text_delta : "") ||
               builder_append_buffer(builder, &block->thinking_signature,
                                     &builder->storage[index].signature, delta->signature_delta) ||
               builder_append(builder, &block->thinking_redacted, delta->redacted_content))
         return -1;
      return 0;
   }
   if (index >= builder->response.n_content || builder->storage[index].stopped)
      return -1;
   aimee_block_t *block = &builder->response.content[index];
   if (delta->type == AIMEE_DELTA_BLOCK_STOP)
   {
      if (block->type == AIMEE_BLK_TOOL_USE)
      {
         if (builder->storage[index].arguments && *builder->storage[index].arguments)
         {
            cJSON *input = cJSON_ParseWithOpts(builder->storage[index].arguments, NULL, 1);
            if (!cJSON_IsObject(input))
            {
               cJSON_Delete(input);
               return -1;
            }
            cJSON_Delete(block->tool_input);
            block->tool_input = input;
         }
         if (!cJSON_IsObject(block->tool_input))
            return -1;
      }
      builder->storage[index].stopped = 1;
      return 0;
   }
   if (delta->type != AIMEE_DELTA_BLOCK_DELTA || delta->kind != block->type)
      return -1;
   if (delta->tool_args_delta)
   {
      if (block->type != AIMEE_BLK_TOOL_USE ||
          (block->tool_input && cJSON_GetArraySize(block->tool_input) && *delta->tool_args_delta) ||
          builder_append_buffer(builder, &builder->storage[index].arguments,
                                &builder->storage[index].args, delta->tool_args_delta))
         return -1;
   }
   if (delta->text_delta && (block->type != AIMEE_BLK_TEXT && block->type != AIMEE_BLK_THINKING))
      return -1;
   if ((delta->signature_delta || delta->redacted_content) && block->type != AIMEE_BLK_THINKING)
      return -1;
   if (builder_append_buffer(builder, &block->text, &builder->storage[index].text,
                             delta->text_delta) ||
       builder_append_buffer(builder, &block->thinking_signature,
                             &builder->storage[index].signature, delta->signature_delta))
      return -1;
   if (delta->redacted_content)
   {
      if (block->thinking_redacted ||
          builder_append(builder, &block->thinking_redacted, delta->redacted_content))
         return -1;
   }
   return 0;
}
