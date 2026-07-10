/* test_hashline_edit.c: unit tests for the pure anchor-edit splice/plan core. */
#include "hashline_edit.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build a caller-owned snapshot view over `content` (no store needed). */
static hashline_snapshot_view_t make_view(const char *content, size_t len)
{
   hashline_snapshot_view_t v;
   memset(&v, 0, sizeof(v));
   v.path = "t";
   v.file_digest = hashline_digest64_raw(content, len);
   v.size = len;
   v.line_count = hashline_line_count(content, len);
   v.line_digests = calloc(v.line_count ? v.line_count : 1, sizeof(uint64_t));
   hashline_line_digests(content, len, (uint64_t *)v.line_digests, v.line_count);
   return v;
}
static void free_view(hashline_snapshot_view_t *v)
{
   free((void *)v->line_digests);
}

static void tag_of(const hashline_snapshot_view_t *v, int ord, char *buf)
{
   hashline_display_tag(v->line_digests[ord - 1], buf, 8);
}

/* Apply and assert success, returning the new content (caller frees). */
static char *apply_ok(const char *content, const hl_edit_op_t *ops, size_t n)
{
   hashline_snapshot_view_t v = make_view(content, strlen(content));
   char *out = NULL;
   size_t outlen = 0;
   hl_edit_fail_t f;
   hl_edit_status_t st =
       hashline_edit_apply(content, strlen(content), &v, ops, n, &out, &outlen, &f);
   free_view(&v);
   assert(st == HL_EDIT_OK);
   assert(out != NULL);
   assert(strlen(out) == outlen); /* text (no embedded NUL) */
   return out;
}

static void test_replace_preserves_unchanged_bytes(void)
{
   const char *c = "alpha\nbeta\ngamma\n";
   hl_edit_op_t ops[1] = {{HL_OP_REPLACE, 2, 2, "", "", "BETA"}};
   char *out = apply_ok(c, ops, 1);
   assert(strcmp(out, "alpha\nBETA\ngamma\n") == 0);
   free(out);
   printf("  replace preserves unchanged bytes ok\n");
}

static void test_duplicate_lines_hit_right_ordinal(void)
{
   const char *c = "x\ndup\ny\ndup\nz\n"; /* dup at ord 2 and 4 */
   hl_edit_op_t ops[1] = {{HL_OP_REPLACE, 4, 4, "", "", "DUP4"}};
   char *out = apply_ok(c, ops, 1);
   assert(strcmp(out, "x\ndup\ny\nDUP4\nz\n") == 0); /* only ord 4 changed */
   free(out);
   printf("  duplicate lines hit right ordinal ok\n");
}

static void test_range_insert_delete(void)
{
   const char *c = "1\n2\n3\n4\n5\n";
   hl_edit_op_t rr[1] = {{HL_OP_REPLACE_RANGE, 2, 4, "", "", "TWO\nTHREE"}};
   char *o1 = apply_ok(c, rr, 1);
   assert(strcmp(o1, "1\nTWO\nTHREE\n5\n") == 0);
   free(o1);

   hl_edit_op_t del[1] = {{HL_OP_DELETE_RANGE, 2, 3, "", "", NULL}};
   char *o2 = apply_ok(c, del, 1);
   assert(strcmp(o2, "1\n4\n5\n") == 0);
   free(o2);

   hl_edit_op_t ins[1] = {{HL_OP_INSERT_AFTER, 2, 2, "", "", "2a\n2b"}};
   char *o3 = apply_ok(c, ins, 1);
   assert(strcmp(o3, "1\n2\n2a\n2b\n3\n4\n5\n") == 0);
   free(o3);
   printf("  replace_range / delete_range / insert_after ok\n");
}

static void test_multi_op_batch_bottom_and_top(void)
{
   const char *c = "a\nb\nc\nd\n";
   /* Non-overlapping ops in one batch, given against as-read ordinals. */
   hl_edit_op_t ops[2] = {{HL_OP_REPLACE, 1, 1, "", "", "A"},
                          {HL_OP_INSERT_AFTER, 3, 3, "", "", "cc"}};
   char *out = apply_ok(c, ops, 2);
   assert(strcmp(out, "A\nb\nc\ncc\nd\n") == 0);
   free(out);
   printf("  multi-op batch ok\n");
}

static void test_stale_and_badop(void)
{
   const char *c = "one\ntwo\n";
   hashline_snapshot_view_t v = make_view(c, strlen(c));

   /* File diverged: verify against a DIFFERENT current buffer. */
   const char *changed = "one\nCHANGED\n";
   char *out = NULL;
   size_t ol = 0;
   hl_edit_fail_t f;
   hl_edit_op_t op[1] = {{HL_OP_REPLACE, 2, 2, "", "", "X"}};
   hl_edit_status_t st = hashline_edit_apply(changed, strlen(changed), &v, op, 1, &out, &ol, &f);
   assert(st == HL_EDIT_STALE && out == NULL && strcmp(f.reason, "file_diverged") == 0);

   /* Out of range. */
   hl_edit_op_t bad[1] = {{HL_OP_REPLACE, 9, 9, "", "", "X"}};
   st = hashline_edit_apply(c, strlen(c), &v, bad, 1, &out, &ol, &f);
   assert(st == HL_EDIT_BADOP && out == NULL && strcmp(f.reason, "out_of_range") == 0);

   /* Wrong anchor tag while file matches snapshot -> hash_mismatch. */
   hl_edit_op_t wt[1] = {{HL_OP_REPLACE, 1, 1, "zzz", "", "X"}};
   st = hashline_edit_apply(c, strlen(c), &v, wt, 1, &out, &ol, &f);
   assert(st == HL_EDIT_STALE && strcmp(f.reason, "hash_mismatch") == 0);

   /* Correct anchor tag passes. */
   char tg[8];
   tag_of(&v, 1, tg);
   hl_edit_op_t gt[1] = {{HL_OP_REPLACE, 1, 1, {0}, "", "X"}};
   memcpy(gt[0].from_tag, tg, strlen(tg) + 1);
   st = hashline_edit_apply(c, strlen(c), &v, gt, 1, &out, &ol, &f);
   assert(st == HL_EDIT_OK && out && strcmp(out, "X\ntwo\n") == 0);
   free(out);
   free_view(&v);
   printf("  stale / badop / tag check ok\n");
}

static void test_conflicts_rejected_atomically(void)
{
   const char *c = "a\nb\nc\nd\n";
   hashline_snapshot_view_t v = make_view(c, strlen(c));
   char *out = (char *)1;
   size_t ol = 0;
   hl_edit_fail_t f;

   /* Overlapping ranges. */
   hl_edit_op_t ov[2] = {{HL_OP_REPLACE_RANGE, 1, 3, "", "", "X"},
                         {HL_OP_REPLACE, 2, 2, "", "", "Y"}};
   out = NULL;
   assert(hashline_edit_apply(c, strlen(c), &v, ov, 2, &out, &ol, &f) == HL_EDIT_CONFLICT);
   assert(out == NULL && strcmp(f.reason, "overlap") == 0);

   /* Two inserts after the same anchor. */
   hl_edit_op_t di[2] = {{HL_OP_INSERT_AFTER, 2, 2, "", "", "p"},
                         {HL_OP_INSERT_AFTER, 2, 2, "", "", "q"}};
   assert(hashline_edit_apply(c, strlen(c), &v, di, 2, &out, &ol, &f) == HL_EDIT_CONFLICT);
   assert(strcmp(f.reason, "dup_insert") == 0);

   /* Insert into a deleted range. */
   hl_edit_op_t idl[2] = {{HL_OP_DELETE_RANGE, 2, 3, "", "", NULL},
                          {HL_OP_INSERT_AFTER, 3, 3, "", "", "z"}};
   assert(hashline_edit_apply(c, strlen(c), &v, idl, 2, &out, &ol, &f) == HL_EDIT_CONFLICT);
   assert(strcmp(f.reason, "insert_into_deleted") == 0);
   free_view(&v);
   printf("  conflicts rejected atomically ok\n");
}

static void test_crlf_and_no_final_newline(void)
{
   /* CRLF file: unchanged lines keep CRLF; edited line normalized to dominant
    * (CRLF here). */
   const char *crlf = "a\r\nb\r\nc\r\n";
   hl_edit_op_t r[1] = {{HL_OP_REPLACE, 2, 2, "", "", "B"}};
   char *o = apply_ok(crlf, r, 1);
   assert(strcmp(o, "a\r\nB\r\nc\r\n") == 0);
   free(o);

   /* No final newline preserved on replace of the last line. */
   const char *nonl = "a\nb\nc"; /* last line has no '\n' */
   hl_edit_op_t r2[1] = {{HL_OP_REPLACE, 3, 3, "", "", "C"}};
   char *o2 = apply_ok(nonl, r2, 1);
   assert(strcmp(o2, "a\nb\nC") == 0); /* still no trailing newline */
   free(o2);

   /* Editing a middle line of a no-final-newline file keeps that property. */
   hl_edit_op_t r3[1] = {{HL_OP_REPLACE, 1, 1, "", "", "A"}};
   char *o3 = apply_ok(nonl, r3, 1);
   assert(strcmp(o3, "A\nb\nc") == 0);
   free(o3);

   /* LF model text into a CRLF file is normalized to CRLF on the edited line. */
   hl_edit_op_t r4[1] = {{HL_OP_REPLACE_RANGE, 1, 2, "", "", "X\nY"}};
   char *o4 = apply_ok(crlf, r4, 1);
   assert(strcmp(o4, "X\r\nY\r\nc\r\n") == 0);
   free(o4);

   /* insert_after the last line of a no-final-newline file: the old last line
    * gains a separator terminator, the appended final line keeps no newline. */
   hl_edit_op_t r5[1] = {{HL_OP_INSERT_AFTER, 3, 3, "", "", "d"}};
   char *o5 = apply_ok(nonl, r5, 1);
   assert(strcmp(o5, "a\nb\nc\nd") == 0);
   free(o5);
   printf("  CRLF preservation + no-final-newline ok\n");
}

int main(void)
{
   test_replace_preserves_unchanged_bytes();
   test_duplicate_lines_hit_right_ordinal();
   test_range_insert_delete();
   test_multi_op_batch_bottom_and_top();
   test_stale_and_badop();
   test_conflicts_rejected_atomically();
   test_crlf_and_no_final_newline();
   printf("test_hashline_edit: all passed\n");
   return 0;
}
