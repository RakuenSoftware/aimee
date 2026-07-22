/* edit_anchored.c: transactional anchor-based edit planner (pure).
 * (proposal: hashline-edit-and-lean-websearch, Part I.)
 *
 * Every anchor resolves against the AS-READ snapshot; freshness is verified by
 * re-hashing the current file's line at each cited ordinal and comparing to the
 * snapshot's full recorded digest. The batch is validated in full before any
 * byte is produced (atomic), applied conceptually bottom-first so no edit
 * renumbers another, and write-back preserves unchanged lines' raw bytes while
 * normalizing only edited regions to the file's dominant terminator. */
#include "edit_anchored.h"
#include "dstr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EDIT_MAX_OPS    512
#define REANCHOR_MARGIN 3 /* context lines each side of a contested range */

typedef enum
{
   OP_REPLACE = 0,
   OP_REPLACE_RANGE,
   OP_INSERT_AFTER,
   OP_DELETE_RANGE
} op_kind_t;

typedef struct
{
   op_kind_t kind;
   int from;         /* 1-based ordinal (as read) */
   int to;           /* 1-based ordinal; == from for replace/insert */
   const char *text; /* borrowed from cJSON; NULL for delete */
   int op_index;
} anchored_op_t;

static cJSON *reject_new(const char *status)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "status", status);
   return o;
}

static cJSON *fail_row(int op_index, const char *anchor, const char *reason)
{
   cJSON *r = cJSON_CreateObject();
   cJSON_AddNumberToObject(r, "op_index", op_index);
   if (anchor)
      cJSON_AddStringToObject(r, "anchor", anchor);
   cJSON_AddStringToObject(r, "reason", reason);
   return r;
}

/* Append re-anchored current lines [lo..hi] (1-based, clamped) as {anchor,text}
 * rows to `arr`, using the CURRENT file digests. */
static void append_context(cJSON *arr, anchor_line_t *lines, int line_count, int lo, int hi)
{
   if (lo < 1)
      lo = 1;
   if (hi > line_count)
      hi = line_count;
   for (int i = lo; i <= hi; i++)
   {
      uint64_t d = anchor_line_digest(lines[i - 1].ptr, lines[i - 1].len, i == 1);
      char tag[3];
      anchor_short_tag(d, tag);
      char anchor[32];
      snprintf(anchor, sizeof(anchor), "%d:%s", i, tag);
      cJSON *row = cJSON_CreateObject();
      cJSON_AddStringToObject(row, "anchor", anchor);
      char *txt = malloc(lines[i - 1].content_len + 1);
      if (txt)
      {
         memcpy(txt, lines[i - 1].ptr, lines[i - 1].content_len);
         txt[lines[i - 1].content_len] = '\0';
         cJSON_AddStringToObject(row, "text", txt);
         free(txt);
      }
      cJSON_AddItemToArray(arr, row);
   }
}

/* Parse "op" + anchors of one edit into `op`. On malformed input fills a
 * validation reason (returned pointer to a static string) and returns -1. */
static const char *parse_op(cJSON *e, int idx, anchored_op_t *op)
{
   op->op_index = idx;
   cJSON *opj = cJSON_GetObjectItem(e, "op");
   if (!opj || !cJSON_IsString(opj))
      return "missing 'op'";
   const char *name = opj->valuestring;
   cJSON *textj = cJSON_GetObjectItem(e, "text");
   op->text = (textj && cJSON_IsString(textj)) ? textj->valuestring : NULL;

   cJSON *atj = cJSON_GetObjectItem(e, "at");
   cJSON *fromj = cJSON_GetObjectItem(e, "from");
   cJSON *toj = cJSON_GetObjectItem(e, "to");
   int at_ord = 0, from_ord = 0, to_ord = 0;
   unsigned tag = 0;

   if (strcmp(name, "replace") == 0 || strcmp(name, "insert_after") == 0)
   {
      if (!atj || !cJSON_IsString(atj) || anchor_parse(atj->valuestring, &at_ord, &tag) != 0)
         return "missing or malformed 'at' anchor";
      op->kind = (name[0] == 'r') ? OP_REPLACE : OP_INSERT_AFTER;
      op->from = op->to = at_ord;
      if (!op->text)
         return "missing 'text'";
   }
   else if (strcmp(name, "replace_range") == 0 || strcmp(name, "delete_range") == 0)
   {
      if (!fromj || !cJSON_IsString(fromj) ||
          anchor_parse(fromj->valuestring, &from_ord, &tag) != 0)
         return "missing or malformed 'from' anchor";
      if (!toj || !cJSON_IsString(toj) || anchor_parse(toj->valuestring, &to_ord, &tag) != 0)
         return "missing or malformed 'to' anchor";
      if (to_ord < from_ord)
         return "'to' anchor precedes 'from' anchor";
      op->kind = (name[0] == 'r') ? OP_REPLACE_RANGE : OP_DELETE_RANGE;
      op->from = from_ord;
      op->to = to_ord;
      if (op->kind == OP_REPLACE_RANGE && !op->text)
         return "missing 'text'";
   }
   else
   {
      return "unknown 'op' (use replace, replace_range, insert_after, delete_range)";
   }
   return NULL;
}

/* If a line begins with an echoed display anchor — "<ordinal>:<hex>| " that a
 * model copied verbatim from the anchored read — return the byte length of that
 * prefix so append_block can drop it. The display tag is never part of the
 * file's content, so stripping it recovers an edit that would otherwise inject
 * the prefix into the source. Returns 0 when the line does not start with one
 * (the common case — clean edits are untouched). */
static size_t anchor_prefix_len(const char *s, size_t len)
{
   size_t i = 0;
   if (i >= len || !isdigit((unsigned char)s[i]))
      return 0;
   while (i < len && isdigit((unsigned char)s[i]))
      i++;
   if (i >= len || s[i] != ':')
      return 0;
   i++;
   size_t hexstart = i;
   while (i < len && isxdigit((unsigned char)s[i]))
      i++;
   if (i == hexstart || i >= len || s[i] != '|')
      return 0;
   i++;
   if (i < len && s[i] == ' ')
      i++;
   return i;
}

/* Append `text` as logical lines each terminated by `eol` (internal \r\n / \n
 * normalized to `eol`). A no-final-newline file is honored by a single global
 * strip after the whole file is built, not here. An echoed "LINE:HASH| " prefix
 * on any line is dropped (model robustness — the tag is display-only). */
static void append_block(dstr_t *out, const char *text, const char *eol)
{
   size_t len = strlen(text);
   /* strip one trailing newline so we don't synthesize a spurious empty line */
   if (len > 0 && text[len - 1] == '\n')
   {
      len--;
      if (len > 0 && text[len - 1] == '\r')
         len--;
   }
   size_t start = 0;
   for (size_t i = 0;; i++)
   {
      if (i == len || text[i] == '\n')
      {
         size_t clen = i - start;
         if (clen > 0 && text[start + clen - 1] == '\r')
            clen--;
         size_t skip = anchor_prefix_len(text + start, clen);
         dstr_append(out, text + start + skip, clen - skip);
         dstr_append_str(out, eol);
         if (i == len)
            break;
         start = i + 1;
      }
   }
}

int edit_anchored_plan(const char *cur_bytes, size_t cur_len, const anchor_snapshot_t *snap,
                       cJSON *edits, edit_anchored_result_t *res)
{
   res->new_text = NULL;
   res->reject = NULL;

   if (!edits || !cJSON_IsArray(edits) || cJSON_GetArraySize(edits) == 0)
   {
      res->reject = reject_new("error");
      cJSON_AddStringToObject(res->reject, "hint", "edits[] is empty");
      return -1;
   }
   int nedits = cJSON_GetArraySize(edits);
   if (nedits > EDIT_MAX_OPS)
   {
      res->reject = reject_new("error");
      cJSON_AddStringToObject(res->reject, "hint", "too many edits in one batch");
      return -1;
   }

   anchor_line_t *lines = NULL;
   int line_count = anchor_split_lines(cur_bytes, cur_len, &lines);
   if (line_count < 0)
   {
      res->reject = reject_new("error");
      cJSON_AddStringToObject(res->reject, "hint", "out of memory");
      return -1;
   }

   anchored_op_t *ops = calloc((size_t)nedits, sizeof(*ops));
   if (!ops)
   {
      free(lines);
      res->reject = reject_new("error");
      cJSON_AddStringToObject(res->reject, "hint", "out of memory");
      return -1;
   }

   /* --- pass 1: parse + shape validation --- */
   for (int i = 0; i < nedits; i++)
   {
      cJSON *e = cJSON_GetArrayItem(edits, i);
      const char *err = parse_op(e, i, &ops[i]);
      if (err)
      {
         cJSON *rej = reject_new("invalid_edit");
         cJSON *failed = cJSON_AddArrayToObject(rej, "failed");
         cJSON_AddItemToArray(failed, fail_row(i, NULL, err));
         cJSON_AddStringToObject(rej, "hint",
                                 "fix the edit and retry against the same snapshot_id");
         free(ops);
         free(lines);
         res->reject = rej;
         return 1;
      }
   }

   /* --- pass 2: freshness (current line digest == snapshot digest) --- */
   cJSON *stale = NULL; /* built lazily */
   int stale_lo = 0, stale_hi = 0;
   for (int i = 0; i < nedits; i++)
   {
      /* the lines an op depends on: [from..to] (insert_after depends on `from`) */
      int lo = ops[i].from, hi = (ops[i].kind == OP_INSERT_AFTER) ? ops[i].from : ops[i].to;
      for (int ord = lo; ord <= hi; ord++)
      {
         const char *reason = NULL;
         if (ord < 1 || ord > snap->line_count)
            reason = "anchor out of range for snapshot";
         else if (ord > line_count)
            reason = "line no longer exists (file shrank since read)";
         else
         {
            uint64_t cur = anchor_line_digest(lines[ord - 1].ptr, lines[ord - 1].len, ord == 1);
            if (cur != snap->line_digests[ord - 1])
               reason = "hash_mismatch";
         }
         if (reason)
         {
            if (!stale)
            {
               stale = reject_new("stale_anchor");
               cJSON_AddArrayToObject(stale, "failed");
               stale_lo = ord;
               stale_hi = ord;
            }
            char anchor[24];
            snprintf(anchor, sizeof(anchor), "%d", ord);
            cJSON *failed = cJSON_GetObjectItem(stale, "failed");
            cJSON_AddItemToArray(failed, fail_row(ops[i].op_index, anchor, reason));
            if (ord < stale_lo)
               stale_lo = ord;
            if (ord > stale_hi)
               stale_hi = ord;
         }
      }
   }
   if (stale)
   {
      cJSON *ctx = cJSON_AddArrayToObject(stale, "context");
      append_context(ctx, lines, line_count, stale_lo - REANCHOR_MARGIN,
                     stale_hi + REANCHOR_MARGIN);
      cJSON_AddStringToObject(stale, "hint",
                              "file changed since read; re-anchor from context and retry against "
                              "the fresh snapshot_id");
      free(ops);
      free(lines);
      res->reject = stale;
      return 1;
   }

   /* --- pass 3: interaction checks (overlap, same-anchor inserts, insert into
    * a replaced/deleted range) --- */
   for (int i = 0; i < nedits; i++)
   {
      for (int j = i + 1; j < nedits; j++)
      {
         int overlap = 0;
         const anchored_op_t *a = &ops[i], *b = &ops[j];
         int a_ins = (a->kind == OP_INSERT_AFTER), b_ins = (b->kind == OP_INSERT_AFTER);
         if (a_ins && b_ins)
         {
            if (a->from == b->from)
               overlap = 1; /* two inserts at same anchor: ambiguous order */
         }
         else if (a_ins || b_ins)
         {
            const anchored_op_t *ins = a_ins ? a : b;
            const anchored_op_t *rng = a_ins ? b : a;
            if (ins->from >= rng->from && ins->from <= rng->to)
               overlap = 1; /* insert target inside a replaced/deleted span */
         }
         else
         {
            if (a->from <= b->to && b->from <= a->to)
               overlap = 1; /* two ranges intersect */
         }
         if (overlap)
         {
            cJSON *rej = reject_new("conflicting_edits");
            cJSON *failed = cJSON_AddArrayToObject(rej, "failed");
            cJSON_AddItemToArray(failed, fail_row(a->op_index, NULL, "interacts with another op"));
            cJSON_AddItemToArray(failed, fail_row(b->op_index, NULL, "interacts with another op"));
            cJSON_AddStringToObject(rej, "hint",
                                    "edits touch or reorder the same lines; split them or widen a "
                                    "single replace_range");
            free(ops);
            free(lines);
            res->reject = rej;
            return 1;
         }
      }
   }

   /* --- apply: build action maps over 1..line_count --- */
   char *deleted = calloc((size_t)line_count + 1, 1);                /* 1 => skip line */
   int *replace_start = calloc((size_t)line_count + 1, sizeof(int)); /* range end, 0 => none */
   const char **replace_text = calloc((size_t)line_count + 1, sizeof(char *));
   const char **insert_after = calloc((size_t)line_count + 1, sizeof(char *));
   if (!deleted || !replace_start || !replace_text || !insert_after)
   {
      free(deleted);
      free(replace_start);
      free(replace_text);
      free(insert_after);
      free(ops);
      free(lines);
      res->reject = reject_new("error");
      cJSON_AddStringToObject(res->reject, "hint", "out of memory");
      return -1;
   }

   for (int i = 0; i < nedits; i++)
   {
      anchored_op_t *op = &ops[i];
      switch (op->kind)
      {
      case OP_REPLACE:
         replace_start[op->from] = op->from;
         replace_text[op->from] = op->text;
         break;
      case OP_REPLACE_RANGE:
         replace_start[op->from] = op->to;
         replace_text[op->from] = op->text;
         for (int k = op->from + 1; k <= op->to; k++)
            deleted[k] = 1;
         break;
      case OP_DELETE_RANGE:
         for (int k = op->from; k <= op->to; k++)
            deleted[k] = 1;
         break;
      case OP_INSERT_AFTER:
         insert_after[op->from] = op->text;
         break;
      }
   }

   const char *eol = snap->eol[0] ? snap->eol : "\n";

   dstr_t out;
   dstr_init(&out);
   for (int i = 1; i <= line_count; i++)
   {
      if (replace_start[i])
         append_block(&out, replace_text[i] ? replace_text[i] : "", eol);
      else if (deleted[i])
         ; /* skipped */
      else
         dstr_append(&out, lines[i - 1].ptr, lines[i - 1].len); /* keep verbatim */
      if (insert_after[i])
         append_block(&out, insert_after[i], eol);
   }

   /* Honor a no-final-newline file: if the original last line carried no
    * terminator and the emitted tail is an edited block (which append_block
    * always terminates), drop that single trailing terminator. A kept verbatim
    * last line already lacks the terminator, so the guard below won't fire. */
   if (snap->no_final_newline && out.len > 0)
   {
      size_t elen = strlen(eol);
      if (out.len >= elen && memcmp(out.data + out.len - elen, eol, elen) == 0)
      {
         out.len -= elen;
         out.data[out.len] = '\0';
      }
   }

   free(deleted);
   free(replace_start);
   free(replace_text);
   free(insert_after);
   free(ops);
   free(lines);

   char *text = dstr_steal(&out);
   if (!text)
   {
      dstr_free(&out);
      text = calloc(1, 1); /* everything deleted -> empty file */
   }
   res->new_text = text;
   return 0;
}
