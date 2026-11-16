/* trajectory_export.c: owner-checked canonical session history -> export artifact. */
#include "trajectory.h"
#include "aimee_sha256.h"
#include "cJSON.h"
#include "db1_client/primary_sessions.h"
#include "module_commands.h"
#include <aimee/ir/session_event.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRAJ_DEFAULT_RESULT_CAP  512
#define TRAJ_REDACTION_BUF_BYTES 8192

/* The memory owner decides screening. Structural identities, signatures and
 * opaque replay state cannot be rewritten as if they were ordinary prose. */
static int screen_string(cJSON *node, int mutable, int capacity, int *redactions)
{
   cJSON *request = cJSON_CreateObject(), *response = NULL;
   if (!request || !cJSON_AddStringToObject(request, "content", node->valuestring) ||
       !cJSON_AddNumberToObject(request, "capacity", capacity))
   {
      cJSON_Delete(request);
      return -1;
   }
   int dispatched = aimee_module_commands_dispatch("memory.screen_content", request, &response);
   cJSON_Delete(request);
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "status"));
   const char *verdict =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "verdict"));
   int rc = -1;
   if (dispatched == 1 && status && !strcmp(status, "ok") && verdict)
   {
      if (!strcmp(verdict, "allow"))
         rc = 0;
      else if (!strcmp(verdict, "redact"))
      {
         const char *text =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "redacted"));
         if (text && !strcmp(text, node->valuestring))
            rc = 0;
         else if (text && mutable && cJSON_SetValuestring(node, text))
         {
            (*redactions)++;
            rc = 0;
         }
      }
   }
   cJSON_Delete(response);
   return rc;
}

static int screen_key(const char *name, int capacity, int *redactions)
{
   cJSON *key = name ? cJSON_CreateString(name) : NULL;
   int rc = key ? screen_string(key, 0, capacity, redactions) : -1;
   cJSON_Delete(key);
   return rc;
}

static int screen_tree(cJSON *node, const char *label, int mutable, int capacity, int *redactions)
{
   if (!node)
      return 0;
   if (label)
   {
      /* Preserve field context when asking the existing owner about scalar
       * arguments/results. Screening "ordinary" alone cannot recognize
       * {"api_key":"ordinary"}; numeric credentials must not escape either.
       * A sensitive field assignment refuses export rather than changing an
       * opaque value's type or claiming the identity was preserved. */
      /* Container markers retain the assignment context without repeatedly
       * serializing each ancestor's entire subtree. Leaf values are checked
       * below under their own field names. */
      char *wire = cJSON_IsObject(node)  ? strdup("{}")
                   : cJSON_IsArray(node) ? strdup("[]")
                                         : cJSON_PrintUnformatted(node);
      if (label && !wire)
         return -1;
      if (wire)
      {
         size_t size = strlen(label) + strlen(wire) + 2;
         char *context = malloc(size);
         if (context)
            snprintf(context, size, "%s=%s", label, wire);
         int rc = context ? screen_key(context, capacity, redactions) : -1;
         free(context);
         free(wire);
         if (rc)
            return -1;
      }
   }
   if (cJSON_IsString(node))
      return screen_string(node, mutable, capacity, redactions);
   cJSON *child;
   cJSON_ArrayForEach(child, node)
   {
      /* Object member names are opaque data too. Refuse unsafe keys rather
       * than altering schemas or allowing a secret to escape in a JSON key. */
      if (child->string && screen_key(child->string, capacity, redactions))
         return -1;
      if (screen_tree(child, child->string ? child->string : label, mutable, capacity, redactions))
         return -1;
   }
   return 0;
}

/* Refuse unmodeled input instead of silently dropping fields at the typed
 * admission boundary. The codec may add explicit defaults on serialization. */
static int projection_complete(const cJSON *source, const cJSON *typed)
{
   if (cJSON_IsObject(source))
   {
      if (!cJSON_IsObject(typed))
         return 0;
      const cJSON *field;
      cJSON_ArrayForEach(field, source) if (!projection_complete(
                                                field, cJSON_GetObjectItemCaseSensitive(
                                                           typed, field->string))) return 0;
      return 1;
   }
   if (cJSON_IsArray(source))
   {
      if (!cJSON_IsArray(typed) || cJSON_GetArraySize(source) != cJSON_GetArraySize(typed))
         return 0;
      const cJSON *a = source->child, *b = typed->child;
      for (; a && b; a = a->next, b = b->next)
         if (!projection_complete(a, b))
            return 0;
      return 1;
   }
   return cJSON_Compare(source, typed, 1);
}

static int screen_owned(char **text, int mutable, int capacity, int *redactions)
{
   if (!*text)
      return 0;
   cJSON *value = cJSON_CreateString(*text);
   if (!value)
      return -1;
   int rc = screen_string(value, mutable, capacity, redactions);
   if (!rc && strcmp(*text, value->valuestring))
   {
      char *copy = strdup(value->valuestring);
      if (!copy)
         rc = -1;
      else
      {
         free(*text);
         *text = copy;
      }
   }
   cJSON_Delete(value);
   return rc;
}

static int screen_message(aimee_message_t *message, int capacity, int *redactions)
{
   if (screen_owned(&message->role, 0, capacity, redactions))
      return -1;
   for (int i = 0; i < message->n_blocks; i++)
   {
      aimee_block_t *block = &message->blocks[i];
      if (screen_owned(&block->text, block->type == AIMEE_BLK_TEXT, capacity, redactions) ||
          screen_tree(block->tool_input, NULL, 1, capacity, redactions) ||
          screen_tree(block->tool_result, NULL, 1, capacity, redactions))
         return -1;
      char **immutable[] = {&block->tool_id,
                            &block->tool_name,
                            &block->tool_namespace,
                            &block->thinking_signature,
                            &block->thinking_binding,
                            &block->thinking_id,
                            &block->thinking_encrypted_content,
                            &block->thinking_redacted,
                            &block->media_ref,
                            &block->media_type,
                            &block->cache_control,
                            &block->cache_ttl};
      for (size_t j = 0; j < sizeof immutable / sizeof immutable[0]; j++)
         if (screen_owned(immutable[j], 0, capacity, redactions))
            return -1;
      if (screen_key(block->context.revision_domain, capacity, redactions) ||
          screen_key(block->context.revision_scope, capacity, redactions))
         return -1;
   }
   return 0;
}

static int compress_results(aimee_message_t *message, int mi, int capacity, cJSON *omissions)
{
   for (int bi = 0; bi < message->n_blocks; bi++)
   {
      aimee_block_t *block = &message->blocks[bi];
      if (block->type != AIMEE_BLK_TOOL_RESULT || !block->tool_result)
         continue;
      char *wire = cJSON_PrintUnformatted(block->tool_result);
      if (!wire)
         return -1;
      size_t length = strlen(wire);
      if (length > (size_t)capacity)
      {
         char digest[65];
         cJSON *omission = cJSON_CreateObject();
         cJSON *replacement = cJSON_CreateString("[tool result omitted from export]");
         int ok = !aimee_sha256_hex(wire, length, digest) && omission && replacement &&
                  cJSON_AddNumberToObject(omission, "message_index", mi) &&
                  cJSON_AddNumberToObject(omission, "block_index", bi) &&
                  cJSON_AddStringToObject(omission, "kind", "tool_result_content") &&
                  cJSON_AddNumberToObject(omission, "export_bytes", (double)length) &&
                  cJSON_AddStringToObject(omission, "sha256", digest);
         if (!ok || !cJSON_AddItemToArray(omissions, omission))
         {
            cJSON_Delete(omission);
            cJSON_Delete(replacement);
            free(wire);
            return -1;
         }
         cJSON_Delete(block->tool_result);
         block->tool_result = replacement;
      }
      free(wire);
   }
   return 0;
}

int trajectory_export(const char *selector, const char *actor, const trajectory_opts_t *opts,
                      char **out_json)
{
   if (!out_json)
      return -1;
   *out_json = NULL;
   if (!selector || !selector[0] || !actor || !actor[0])
      return -1;
   char *history = NULL;
   if (db1_session_ir_history_load(selector, "primary", actor, "", &history))
   {
      free(history);
      return -1;
   }
   cJSON *source = history ? cJSON_ParseWithOpts(history, NULL, 1) : NULL;
   free(history);
   cJSON *root = cJSON_CreateObject(), *messages = cJSON_CreateArray(),
         *omissions = cJSON_CreateArray();
   int redactions = 0;
   int screen = !opts || opts->redact;
   int capacity =
       opts && opts->redaction_buf_bytes > 0 ? opts->redaction_buf_bytes : TRAJ_REDACTION_BUF_BYTES;
   int cap = opts && opts->max_tool_result_bytes > 0 ? opts->max_tool_result_bytes
                                                     : TRAJ_DEFAULT_RESULT_CAP;
   if (!cJSON_IsArray(source) || !root || !messages || !omissions ||
       (screen && screen_key(selector, capacity, &redactions)))
      goto fail;
   int mi = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, source)
   {
      aimee_message_t message = {0};
      int rc = aimee_ir_message_parse(item, &message);
      cJSON *admitted = !rc ? aimee_ir_message_json(&message) : NULL;
      if (!admitted || !projection_complete(item, admitted))
         rc = -1;
      cJSON_Delete(admitted);
      if (!rc && screen)
         rc = screen_message(&message, capacity, &redactions);
      if (!rc && opts && opts->compress)
         rc = compress_results(&message, mi, cap, omissions);
      cJSON *encoded = !rc ? aimee_ir_message_json(&message) : NULL;
      aimee_message_free(&message);
      if (!encoded || !cJSON_AddItemToArray(messages, encoded))
      {
         cJSON_Delete(encoded);
         goto fail;
      }
      mi++;
   }
   if (!cJSON_AddStringToObject(root, "schema", "aimee.trajectory.v2") ||
       !cJSON_AddStringToObject(root, "source", "canonical_session_log") ||
       !cJSON_AddStringToObject(root, "session_id", selector))
      goto fail;
   cJSON *fidelity = cJSON_AddObjectToObject(root, "fidelity");
   if (!fidelity || !cJSON_AddNumberToObject(fidelity, "redactions", redactions) ||
       !cJSON_AddBoolToObject(fidelity, "content_unchanged",
                              !redactions && !cJSON_GetArraySize(omissions)))
      goto fail;
   if (!cJSON_AddItemToObject(fidelity, "omissions", omissions))
      goto fail;
   omissions = NULL;
   if (!cJSON_AddItemToObject(root, "messages", messages))
      goto fail;
   messages = NULL;
   *out_json = cJSON_PrintUnformatted(root);
fail:
   cJSON_Delete(source);
   cJSON_Delete(root);
   cJSON_Delete(messages);
   cJSON_Delete(omissions);
   return *out_json ? 0 : -1;
}
