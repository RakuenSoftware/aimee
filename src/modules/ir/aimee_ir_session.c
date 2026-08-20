/* aimee_ir_session.c -- what a turn actually did, measured from the IR.
 *
 * Protocol- and client-neutral: it reads the parsed request, so one
 * implementation covers every ingress rather than one per wire format. */
#include <aimee/ir/aimee_ir.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

/* Place the persona on the first user message, once per conversation.
 *
 * Prepending to the first USER message rather than adding a system message is
 * what makes this client-agnostic: every ingress shape has a first user turn,
 * while system-message support and placement rules differ per protocol.
 *
 * Two independent guards keep it from firing twice. An assistant turn anywhere
 * in the history proves this is an existing conversation, and the caller
 * transcript may not contain our earlier mutated request -- so the absence of
 * the marker alone is not permission to inject again. The marker check then
 * covers the case where the caller does echo our mutated first turn back.
 *
 * Returns 1 when the persona was placed, 0 when it was not needed or could not
 * be placed. Allocation failure restores the block array before returning. */
int aimee_ir_prepend_persona_instructions(aimee_request_t *request, const char *instructions)
{
   if (!request || !instructions || !instructions[0])
      return 0;

   for (int i = 0; i < request->n_messages; i++)
      if (request->messages[i].role && strcmp(request->messages[i].role, "assistant") == 0)
         return 0;

   aimee_message_t *first_user = NULL;
   for (int i = 0; i < request->n_messages; i++)
   {
      if (request->messages[i].role && strcmp(request->messages[i].role, "user") == 0)
      {
         first_user = &request->messages[i];
         break;
      }
   }
   if (!first_user)
      return 0;

   for (int i = 0; i < first_user->n_blocks; i++)
      if (first_user->blocks[i].type == AIMEE_BLK_TEXT && first_user->blocks[i].text &&
          strstr(first_user->blocks[i].text, "<aimee-persona ") != NULL)
         return 0;

   aimee_block_t *blocks =
       realloc(first_user->blocks, (size_t)(first_user->n_blocks + 1) * sizeof *blocks);
   if (!blocks)
      return 0;
   first_user->blocks = blocks;
   memmove(&blocks[1], &blocks[0], (size_t)first_user->n_blocks * sizeof *blocks);
   memset(&blocks[0], 0, sizeof blocks[0]);
   blocks[0].type = AIMEE_BLK_TEXT;
   blocks[0].text = strdup(instructions);
   if (!blocks[0].text)
   {
      memmove(&blocks[0], &blocks[1], (size_t)first_user->n_blocks * sizeof *blocks);
      return 0;
   }
   first_user->n_blocks += 1;
   return 1;
}

/* Flat-text persona placement, for plain-chat handlers with no IR to mutate. */
char *aimee_ir_prepend_persona_text(const char *text, const char *instructions)
{
   const char *body = text ? text : "";
   if (!instructions || !instructions[0] || strstr(body, "<aimee-persona ") != NULL)
      return strdup(body);
   size_t ilen = strlen(instructions), blen = strlen(body);
   char *out = malloc(ilen + blen + 2);
   if (!out)
      return NULL;
   memcpy(out, instructions, ilen);
   out[ilen] = '\n';
   memcpy(out + ilen + 1, body, blen + 1);
   return out;
}

static int session_text_has(const char *text, const char *needle)
{
   return text && needle && strstr(text, needle) != NULL;
}

static const char *session_tool_command(const aimee_block_t *block)
{
   if (!block || !cJSON_IsObject(block->tool_input))
      return NULL;
   static const char *const keys[] = {"cmd", "command", "script", "path", "file_path", NULL};
   for (int i = 0; keys[i]; i++)
   {
      cJSON *value = cJSON_GetObjectItemCaseSensitive(block->tool_input, keys[i]);
      if (cJSON_IsString(value))
         return value->valuestring;
   }
   return NULL;
}

static int session_tool_is_edit(const aimee_block_t *block)
{
   const char *name = block ? block->tool_name : NULL;
   const char *cmd = session_tool_command(block);
   return session_text_has(name, "patch") || session_text_has(name, "edit") ||
          session_text_has(name, "write") || session_text_has(cmd, "apply_patch") ||
          session_text_has(cmd, "sed -i") || session_text_has(cmd, "perl -pi");
}

static int session_tool_is_test(const aimee_block_t *block)
{
   const char *name = block ? block->tool_name : NULL;
   const char *cmd = session_tool_command(block);
   return session_text_has(name, "test") || session_text_has(cmd, "pytest") ||
          session_text_has(cmd, "ctest") || session_text_has(cmd, "cargo test") ||
          session_text_has(cmd, "go test") || session_text_has(cmd, "npm test") ||
          session_text_has(cmd, "make test") || session_text_has(cmd, "make check");
}

static int session_tool_is_read_or_search(const aimee_block_t *block)
{
   const char *name = block ? block->tool_name : NULL;
   const char *cmd = session_tool_command(block);
   return session_text_has(name, "read") || session_text_has(name, "open") ||
          session_text_has(name, "search") || session_text_has(name, "find") ||
          session_text_has(name, "span") || session_text_has(name, "structure") ||
          session_text_has(cmd, "rg ") || session_text_has(cmd, "grep ") ||
          session_text_has(cmd, "find ") || session_text_has(cmd, "sed -n") ||
          session_text_has(cmd, "head ") || session_text_has(cmd, "tail ") ||
          session_text_has(cmd, "cat ");
}

static char *session_tool_signature(const aimee_block_t *block)
{
   if (!block || !block->tool_name)
      return NULL;
   char *input = block->tool_input ? cJSON_PrintUnformatted(block->tool_input) : strdup("{}");
   if (!input)
      return NULL;
   size_t n = strlen(block->tool_name) + strlen(input) + 2;
   char *signature = malloc(n);
   if (signature)
      snprintf(signature, n, "%s:%s", block->tool_name, input);
   free(input);
   return signature;
}

static int session_append_guidance(aimee_request_t *request, const char *kind, const char *text)
{
   size_t n = strlen(kind) + strlen(text) + 48;
   char *payload = malloc(n);
   if (!payload)
      return 0;
   snprintf(payload, n, "<aimee-guidance kind=\"%s\">%s</aimee-guidance>", kind, text);
   aimee_block_t *blocks =
       realloc(request->system, (size_t)(request->n_system + 1) * sizeof *blocks);
   if (!blocks)
   {
      free(payload);
      return 0;
   }
   request->system = blocks;
   memset(&blocks[request->n_system], 0, sizeof blocks[request->n_system]);
   blocks[request->n_system].type = AIMEE_BLK_TEXT;
   blocks[request->n_system].text = payload;
   request->n_system++;
   request->mutated = 1;
   return 1;
}

void aimee_ir_session_measure(const aimee_request_t *request, aimee_ir_session_metrics_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof *out);
   if (!request)
      return;
   enum
   {
      MAX_SIGNATURES = 128
   };
   char *signatures[MAX_SIGNATURES] = {0};
   int signature_count = 0;
   int repeated_reads = 0;
   int read_searches = 0;
   int edits = 0;
   int tests = 0;
   int calls_after_last_edit = 0;

   for (int i = 0; i < request->n_messages; i++)
   {
      for (int j = 0; j < request->messages[i].n_blocks; j++)
      {
         const aimee_block_t *block = &request->messages[i].blocks[j];
         if (block->type != AIMEE_BLK_TOOL_USE)
            continue;
         out->tool_calls++;
         if (session_tool_is_edit(block))
         {
            edits++;
            calls_after_last_edit = 0;
         }
         else if (edits > 0)
            calls_after_last_edit++;
         if (session_tool_is_test(block))
            tests++;
         if (session_tool_is_read_or_search(block))
            read_searches++;

         char *signature = session_tool_signature(block);
         if (!signature)
            continue;
         int repeats = 1;
         for (int k = 0; k < signature_count; k++)
            if (strcmp(signatures[k], signature) == 0)
               repeats++;
         if (repeats > 1)
            out->redundant_tool_calls++;
         if (session_tool_is_read_or_search(block) && repeats == 3)
            repeated_reads = 1;
         if (signature_count < MAX_SIGNATURES)
            signatures[signature_count++] = signature;
         else
            free(signature);
      }
   }
   for (int i = 0; i < signature_count; i++)
      free(signatures[i]);

   if (read_searches == 4 && edits == 0)
      snprintf(out->intervention, sizeof(out->intervention), "%s", "scope-search-before-change");
   else if (repeated_reads)
      snprintf(out->intervention, sizeof(out->intervention), "%s", "repeated-read");
   else if (tests == 2 && edits == 0)
      snprintf(out->intervention, sizeof(out->intervention), "%s", "tests-without-change");
   else if (edits > 0 && tests == 0 && calls_after_last_edit == 3)
      snprintf(out->intervention, sizeof(out->intervention), "%s", "change-without-test");
}

int aimee_ir_stage_session_assist(aimee_request_t *request, void *unused)
{
   (void)unused;
   aimee_ir_session_metrics_t metrics;
   aimee_ir_session_measure(request, &metrics);
   if (strcmp(metrics.intervention, "scope-search-before-change") == 0)
      return session_append_guidance(
          request, "scope-search-before-change",
          "Several files have been inspected without a change. Before editing, identify the "
          "defect shape and run `aimee index ast-grep --lang <language> [--path <path>] "
          "'<pattern>'` to find analogous production instances. Repair every confirmed in-scope "
          "instance now; do not defer known matching defects as optional follow-up.");
   if (strcmp(metrics.intervention, "repeated-read") == 0)
      return session_append_guidance(
          request, "repeated-read",
          "The same read or search has now repeated without new evidence. Change the query or use "
          "`aimee index investigate` for ranked context, analogues, callers, and impact evidence.");
   if (strcmp(metrics.intervention, "tests-without-change") == 0)
      return session_append_guidance(
          request, "tests-without-change",
          "Tests have been run repeatedly without a code change. Re-check the failure evidence and "
          "make the smallest supported change before repeating the same test.");
   if (strcmp(metrics.intervention, "change-without-test") == 0)
      return session_append_guidance(
          request, "change-without-test",
          "Code changed and several actions followed without verification. Run the narrowest "
          "relevant test, then expand only if it passes.");
   return 0;
}
