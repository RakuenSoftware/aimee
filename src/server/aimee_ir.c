/* aimee_ir.c -- lifecycle + accessors for the canonical IR. See aimee_ir.h.
 * Pure: depends only on cJSON + libc, so it unit-tests standalone. */
#include "aimee_ir.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static void free_str(char *s)
{
   free(s);
}

void aimee_block_free_contents(aimee_block_t *b)
{
   if (!b)
      return;
   free_str(b->text);
   free_str(b->tool_id);
   free_str(b->tool_name);
   cJSON_Delete(b->tool_input);
   cJSON_Delete(b->tool_result);
   free_str(b->media_type);
   free_str(b->media_ref);
   free_str(b->cache_control);
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

static void free_message(aimee_message_t *m)
{
   if (!m)
      return;
   free_str(m->role);
   free_blocks(m->blocks, m->n_blocks);
   cJSON_Delete(m->raw);
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
         free_message(&r->messages[i]);
      free(r->messages);
   }
   if (r->tools)
   {
      for (int i = 0; i < r->n_tools; i++)
      {
         free_str(r->tools[i].name);
         free_str(r->tools[i].description);
         cJSON_Delete(r->tools[i].schema);
         free_str(r->tools[i].cache_control);
         cJSON_Delete(r->tools[i].raw);
      }
      free(r->tools);
   }
   cJSON_Delete(r->tool_choice);
   if (r->stop_sequences)
   {
      for (int i = 0; i < r->n_stop; i++)
         free_str(r->stop_sequences[i]);
      free(r->stop_sequences);
   }
   cJSON_Delete(r->raw);
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
   cJSON_Delete(r->raw);
   memset(r, 0, sizeof *r);
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
   if (a->type != b->type)
      return 0;
   if (!str_eq(a->cache_control, b->cache_control))
      return 0;
   switch (a->type)
   {
   case AIMEE_BLK_TEXT:
   case AIMEE_BLK_THINKING:
      return str_eq(a->text, b->text);
   case AIMEE_BLK_TOOL_USE:
      return str_eq(a->tool_id, b->tool_id) && str_eq(a->tool_name, b->tool_name) &&
             json_eq(a->tool_input, b->tool_input);
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

static int blocks_eq(const aimee_block_t *a, int na, const aimee_block_t *b, int nb)
{
   if (na != nb)
      return 0;
   for (int i = 0; i < na; i++)
      if (!block_eq(&a[i], &b[i]))
         return 0;
   return 1;
}

int aimee_ir_request_equal(const aimee_request_t *a, const aimee_request_t *b)
{
   if (a == b)
      return 1;
   if (!a || !b)
      return 0;
   if (!blocks_eq(a->system, a->n_system, b->system, b->n_system))
      return 0;
   if (a->n_messages != b->n_messages)
      return 0;
   for (int i = 0; i < a->n_messages; i++)
   {
      if (!str_eq(a->messages[i].role, b->messages[i].role))
         return 0;
      if (!blocks_eq(a->messages[i].blocks, a->messages[i].n_blocks, b->messages[i].blocks,
                     b->messages[i].n_blocks))
         return 0;
   }
   if (a->n_tools != b->n_tools)
      return 0;
   for (int i = 0; i < a->n_tools; i++)
   {
      if (!str_eq(a->tools[i].name, b->tools[i].name) ||
          !str_eq(a->tools[i].description, b->tools[i].description) ||
          !str_eq(a->tools[i].cache_control, b->tools[i].cache_control) ||
          !json_eq(a->tools[i].schema, b->tools[i].schema))
         return 0;
   }
   if (!json_eq(a->tool_choice, b->tool_choice))
      return 0;
   /* sampling params (content, not provenance) */
   if (a->has_max_tokens != b->has_max_tokens ||
       (a->has_max_tokens && a->max_tokens != b->max_tokens))
      return 0;
   if (a->has_temperature != b->has_temperature ||
       (a->has_temperature && a->temperature != b->temperature))
      return 0;
   if (a->stream != b->stream || a->n_stop != b->n_stop)
      return 0;
   for (int i = 0; i < a->n_stop; i++)
      if (!str_eq(a->stop_sequences[i], b->stop_sequences[i]))
         return 0;
   /* IGNORED: frontend wire tag, raw sidecar, model string (provenance). */
   return 1;
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
   for (int i = 0; i < n_blocks && used + 1 < n; i++)
   {
      const aimee_block_t *b = &blocks[i];
      if (b->type != want || !b->text)
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
