/* hashline_edit.c: PURE transactional splice/plan core for anchor edits.
 * See hashline_edit.h. No I/O, no snapshot store, no writes. */
#include "hashline_edit.h"

#include "dstr.h"

#include <stdlib.h>
#include <string.h>

/* The op anchor tag buffers (hl_edit_op_t.from_tag/to_tag, char[8]) must hold a
 * display tag + NUL. Guard the invariant at compile time so growing the display
 * tag cannot silently overflow them. */
_Static_assert(HASHLINE_DISPLAY_TAG_HEX + 1 <= 8, "display tag must fit in hl_edit_op_t tag[8]");

/* One physical line of the original file. content is [start,content_end) and
 * excludes the terminator (and the CR of a CRLF); the terminator bytes are
 * [content_end,full_end) — "\r\n", "\n", or "" for a final line with no
 * newline. */
typedef struct
{
   size_t start;
   size_t content_end;
   size_t full_end;
   int has_term;
   int is_crlf;
} linerec_t;

/* One output line: content bytes [ptr,ptr+len) plus its terminator bytes. For a
 * kept original line the terminator is the line's original bytes (term_len 0 for
 * a final no-newline line); for a new/edited line term points at the dominant
 * terminator. The final emission's terminator is decided at serialize time to
 * preserve the file's no-final-newline property. */
typedef struct
{
   const char *ptr;
   size_t len;
   const char *term;
   size_t term_len;
} emit_t;

typedef struct
{
   emit_t *v;
   size_t n, cap;
   int oom;
} emitvec_t;

static void ev_push(emitvec_t *e, const char *ptr, size_t len, const char *term, size_t term_len)
{
   if (e->oom)
      return;
   if (e->n == e->cap)
   {
      size_t nc = e->cap ? e->cap * 2 : 16;
      emit_t *nv = realloc(e->v, nc * sizeof(emit_t));
      if (!nv)
      {
         e->oom = 1;
         return;
      }
      e->v = nv;
      e->cap = nc;
   }
   e->v[e->n].ptr = ptr;
   e->v[e->n].len = len;
   e->v[e->n].term = term;
   e->v[e->n].term_len = term_len;
   e->n++;
}

/* Push a model-supplied text block as one or more new lines (terminator =
 * dominant). A trailing '\n' does not create a phantom empty line; a lone empty
 * text becomes a single empty line. Each segment's trailing '\r' is stripped —
 * edited regions are normalized to the dominant terminator. */
static void ev_push_text(emitvec_t *e, const char *text, const char *dom, size_t dom_len)
{
   const char *t = text ? text : "";
   size_t tl = strlen(t);
   if (tl == 0)
   {
      ev_push(e, t, 0, dom, dom_len); /* one empty line */
      return;
   }
   size_t seg_start = 0, i = 0;
   for (;;)
   {
      int at_end = (i >= tl);
      if (at_end || t[i] == '\n')
      {
         size_t seg_end = i;
         if (seg_end > seg_start && t[seg_end - 1] == '\r')
            seg_end--;
         int phantom = at_end && (i == seg_start); /* trailing empty after final '\n' */
         if (!phantom)
            ev_push(e, t + seg_start, seg_end - seg_start, dom, dom_len);
         if (at_end)
            break;
         seg_start = i + 1;
      }
      i++;
   }
}

static linerec_t *parse_lines(const char *content, size_t len, size_t *out_n)
{
   size_t n = hashline_line_count(content, len);
   linerec_t *lines = calloc(n ? n : 1, sizeof(linerec_t));
   if (!lines)
      return NULL;
   size_t i = 0, k = 0;
   while (i < len)
   {
      size_t s = i, e = i;
      while (e < len && content[e] != '\n')
         e++;
      int has_term = (e < len);
      size_t content_end = e;
      int is_crlf = 0;
      if (has_term && content_end > s && content[content_end - 1] == '\r')
      {
         content_end--;
         is_crlf = 1;
      }
      lines[k].start = s;
      lines[k].content_end = content_end;
      lines[k].full_end = has_term ? e + 1 : e;
      lines[k].has_term = has_term;
      lines[k].is_crlf = is_crlf;
      k++;
      i = has_term ? e + 1 : e;
   }
   *out_n = k;
   return lines;
}

static void set_fail(hl_edit_fail_t *f, int op, const char *reason, int cs, int ce)
{
   if (!f)
      return;
   f->failed_op = op;
   f->reason = reason;
   f->ctx_start = cs;
   f->ctx_end = ce;
}

hl_edit_status_t hashline_edit_apply(const char *content, size_t len,
                                     const hashline_snapshot_view_t *snap, const hl_edit_op_t *ops,
                                     size_t nops, char **out_content, size_t *out_len,
                                     hl_edit_fail_t *fail)
{
   if (out_content)
      *out_content = NULL;
   if (out_len)
      *out_len = 0;
   set_fail(fail, -1, NULL, 0, 0);
   if (!content || !snap || (nops > 0 && !ops))
      return HL_EDIT_BADOP;
   if (nops == 0)
   {
      set_fail(fail, -1, "empty_batch", 0, 0);
      return HL_EDIT_BADOP;
   }

   /* Whole-file divergence check: if the current bytes differ from the snapshot
    * the model read, reject conservatively. Size is checked alongside the digest
    * (cheap, and closes the astronomically-unlikely different-size collision).
    * When this matches, every line matches the snapshot by construction. */
   if (len != snap->size || hashline_digest64_raw(content, len) != snap->file_digest)
   {
      set_fail(fail, -1, "file_diverged", 0, 0);
      return HL_EDIT_STALE;
   }

   size_t nlines = 0;
   linerec_t *lines = parse_lines(content, len, &nlines);
   if (!lines)
      return HL_EDIT_OOM;
   if (nlines != snap->line_count)
   {
      free(lines);
      set_fail(fail, -1, "file_diverged", 0, 0);
      return HL_EDIT_STALE;
   }
   int N = (int)nlines;

   const char **replace_text = calloc((size_t)N + 1, sizeof(char *));
   const char **ins_text = calloc((size_t)N + 1, sizeof(char *));
   int *owner = malloc(((size_t)N + 1) * sizeof(int));
   int *ins_owner = malloc(((size_t)N + 1) * sizeof(int));
   char *skip = calloc((size_t)N + 1, sizeof(char));
   if (!replace_text || !ins_text || !owner || !ins_owner || !skip)
   {
      free(lines);
      free(replace_text);
      free(ins_text);
      free(owner);
      free(ins_owner);
      free(skip);
      return HL_EDIT_OOM;
   }
   for (int i = 0; i <= N; i++)
   {
      owner[i] = -1;
      ins_owner[i] = -1;
   }

   hl_edit_status_t st = HL_EDIT_OK;

   for (size_t k = 0; k < nops && st == HL_EDIT_OK; k++)
   {
      const hl_edit_op_t *op = &ops[k];
      int a = op->from;
      int b =
          (op->kind == HL_OP_REPLACE_RANGE || op->kind == HL_OP_DELETE_RANGE) ? op->to : op->from;
      if (a < 1 || a > N || b < 1 || b > N || b < a)
      {
         set_fail(fail, (int)k, "out_of_range", a, b);
         st = HL_EDIT_BADOP;
         break;
      }
      /* Anchor display-tag check: catches a mis-referenced ordinal even when the
       * file still matches the snapshot. Tags are optional ("" skips). */
      if (op->from_tag[0])
      {
         char t[HASHLINE_DISPLAY_TAG_HEX + 1];
         hashline_display_tag(snap->line_digests[a - 1], t, sizeof(t));
         if (strcmp(t, op->from_tag) != 0)
         {
            set_fail(fail, (int)k, "hash_mismatch", a, a);
            st = HL_EDIT_STALE;
            break;
         }
      }
      if ((op->kind == HL_OP_REPLACE_RANGE || op->kind == HL_OP_DELETE_RANGE) && op->to_tag[0])
      {
         char t[HASHLINE_DISPLAY_TAG_HEX + 1];
         hashline_display_tag(snap->line_digests[b - 1], t, sizeof(t));
         if (strcmp(t, op->to_tag) != 0)
         {
            set_fail(fail, (int)k, "hash_mismatch", b, b);
            st = HL_EDIT_STALE;
            break;
         }
      }

      if (op->kind == HL_OP_INSERT_AFTER)
      {
         if (ins_owner[a] != -1)
         {
            set_fail(fail, (int)k, "dup_insert", a, a);
            st = HL_EDIT_CONFLICT;
            break;
         }
         ins_owner[a] = (int)k;
         ins_text[a] = op->text ? op->text : "";
         continue;
      }

      for (int ord = a; ord <= b; ord++)
      {
         if (owner[ord] != -1)
         {
            set_fail(fail, (int)k, "overlap", a, b);
            st = HL_EDIT_CONFLICT;
            break;
         }
         owner[ord] = (int)k;
      }
      if (st != HL_EDIT_OK)
         break;

      if (op->kind == HL_OP_REPLACE)
         replace_text[a] = op->text ? op->text : "";
      else if (op->kind == HL_OP_REPLACE_RANGE)
      {
         replace_text[a] = op->text ? op->text : "";
         for (int ord = a + 1; ord <= b; ord++)
            skip[ord] = 1;
      }
      else /* HL_OP_DELETE_RANGE */
      {
         for (int ord = a; ord <= b; ord++)
            skip[ord] = 1;
      }
   }

   if (st == HL_EDIT_OK)
   {
      for (int ord = 1; ord <= N; ord++)
         if (ins_owner[ord] != -1 && skip[ord])
         {
            set_fail(fail, ins_owner[ord], "insert_into_deleted", ord, ord);
            st = HL_EDIT_CONFLICT;
            break;
         }
   }

   if (st != HL_EDIT_OK)
   {
      free(lines);
      free(replace_text);
      free(ins_text);
      free(owner);
      free(ins_owner);
      free(skip);
      return st;
   }

   /* Dominant terminator: strict majority CRLF vs LF among terminated lines; tie
    * or none -> LF. */
   size_t crlf = 0, lf = 0;
   for (int i = 0; i < N; i++)
      if (lines[i].has_term)
      {
         if (lines[i].is_crlf)
            crlf++;
         else
            lf++;
      }
   const char *dom = (crlf > lf) ? "\r\n" : "\n";
   size_t dom_len = (crlf > lf) ? 2 : 1;
   int file_had_final_newline = (N > 0) ? lines[N - 1].has_term : 0;

   /* Build the ordered emission list (forward over as-read ordinals; skips
    * removed lines; replacements/inserts contribute new lines). */
   emitvec_t ev = {0};
   for (int ord = 1; ord <= N; ord++)
   {
      if (skip[ord])
         continue;
      if (replace_text[ord])
         ev_push_text(&ev, replace_text[ord], dom, dom_len);
      else
      {
         const linerec_t *L = &lines[ord - 1];
         const char *term = (L->full_end > L->content_end) ? content + L->content_end : NULL;
         size_t term_len = L->full_end - L->content_end;
         ev_push(&ev, content + L->start, L->content_end - L->start, term, term_len);
      }
      if (ins_text[ord])
         ev_push_text(&ev, ins_text[ord], dom, dom_len);
   }

   free(replace_text);
   free(ins_text);
   free(owner);
   free(ins_owner);
   free(skip);
   free(lines);

   if (ev.oom)
   {
      free(ev.v);
      return HL_EDIT_OOM;
   }

   /* Serialize with byte preservation. Non-last emissions get their natural
    * terminator (dominant if a kept line had none). The final emission honors the
    * file's no-final-newline property. */
   dstr_t out;
   dstr_init(&out);
   for (size_t j = 0; j < ev.n; j++)
   {
      dstr_append(&out, ev.v[j].ptr, ev.v[j].len);
      int is_last = (j + 1 == ev.n);
      if (is_last)
      {
         if (file_had_final_newline)
         {
            if (ev.v[j].term_len > 0)
               dstr_append(&out, ev.v[j].term, ev.v[j].term_len);
            else
               dstr_append(&out, dom, dom_len);
         }
         /* else preserve no-final-newline: append nothing */
      }
      else
      {
         if (ev.v[j].term_len > 0)
            dstr_append(&out, ev.v[j].term, ev.v[j].term_len);
         else
            dstr_append(&out, dom, dom_len);
      }
   }
   free(ev.v);

   if (out_content)
      *out_content = out.data;
   else
      dstr_free(&out);
   if (out_len)
      *out_len = out.len;
   return HL_EDIT_OK;
}
