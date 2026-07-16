#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anchor_snapshot.h"
#include "edit_anchored.h"
#include "cJSON.h"

/* Build an in-memory snapshot over `body` and return a copy the caller disposes. */
static void snap_of(const char *body, anchor_snapshot_t *snap)
{
   char id[ANCHOR_SNAPSHOT_ID_MAX];
   assert(anchor_snapshot_create("/t.c", body, strlen(body), id) == 0);
   assert(anchor_snapshot_get_copy(id, snap) == 1);
}

/* Compute the "LINE:HASH" anchor for 1-based ordinal in body. */
static void anchor_of(const char *body, int ord, char out[24])
{
   anchor_line_t *l = NULL;
   int n = anchor_split_lines(body, strlen(body), &l);
   assert(ord >= 1 && ord <= n);
   char tag[3];
   anchor_short_tag(anchor_line_digest(l[ord - 1].ptr, l[ord - 1].len, ord == 1), tag);
   snprintf(out, 24, "%d:%s", ord, tag);
   free(l);
}

static cJSON *edit_replace(const char *anchor, const char *text)
{
   cJSON *e = cJSON_CreateObject();
   cJSON_AddStringToObject(e, "op", "replace");
   cJSON_AddStringToObject(e, "at", anchor);
   cJSON_AddStringToObject(e, "text", text);
   return e;
}

int main(void)
{
   printf("edit_anchored: ");

   /* --- single replace --- */
   {
      const char *body = "one\ntwo\nthree\n";
      anchor_snapshot_t snap;
      snap_of(body, &snap);
      char a2[24];
      anchor_of(body, 2, a2);
      cJSON *edits = cJSON_CreateArray();
      cJSON_AddItemToArray(edits, edit_replace(a2, "TWO"));
      edit_anchored_result_t res;
      int rc = edit_anchored_plan(body, strlen(body), &snap, edits, &res);
      assert(rc == 0 && res.new_text);
      assert(strcmp(res.new_text, "one\nTWO\nthree\n") == 0);
      free(res.new_text);
      cJSON_Delete(edits);
      anchor_snapshot_dispose(&snap);
   }

   /* --- multi-edit batch: replace line 1 and line 4; order-independent, server
    *     resolves offsets (bottom-first) --- */
   {
      const char *body = "a\nb\nc\nd\n";
      anchor_snapshot_t snap;
      snap_of(body, &snap);
      char a1[24], a4[24];
      anchor_of(body, 1, a1);
      anchor_of(body, 4, a4);
      cJSON *edits = cJSON_CreateArray();
      cJSON_AddItemToArray(edits, edit_replace(a4, "D"));
      cJSON_AddItemToArray(edits, edit_replace(a1, "A"));
      edit_anchored_result_t res;
      int rc = edit_anchored_plan(body, strlen(body), &snap, edits, &res);
      assert(rc == 0);
      assert(strcmp(res.new_text, "A\nb\nc\nD\n") == 0);
      free(res.new_text);
      cJSON_Delete(edits);
      anchor_snapshot_dispose(&snap);
   }

   /* --- DUPLICATE IDENTICAL LINES: ordinal disambiguates; editing line 4 must
    *     not touch the identical line 2 (str_replace's "occurs N times") --- */
   {
      const char *body = "x\nsame\ny\nsame\nz\n";
      anchor_snapshot_t snap;
      snap_of(body, &snap);
      char a4[24];
      anchor_of(body, 4, a4);
      /* the two 'same' lines share a display tag but distinct ordinals */
      char a2[24];
      anchor_of(body, 2, a2);
      assert(strcmp(a2 + 2, a4 + 2) == 0); /* same ":HASH" suffix */
      assert(a2[0] == '2' && a4[0] == '4');
      cJSON *edits = cJSON_CreateArray();
      cJSON_AddItemToArray(edits, edit_replace(a4, "SAME4"));
      edit_anchored_result_t res;
      int rc = edit_anchored_plan(body, strlen(body), &snap, edits, &res);
      assert(rc == 0);
      assert(strcmp(res.new_text, "x\nsame\ny\nSAME4\nz\n") == 0);
      free(res.new_text);
      cJSON_Delete(edits);
      anchor_snapshot_dispose(&snap);
   }

   /* --- STALE DRIFT: file changed under the model -> stale_anchor, no apply --- */
   {
      const char *read_body = "one\ntwo\nthree\n";
      anchor_snapshot_t snap;
      snap_of(read_body, &snap);
      char a2[24];
      anchor_of(read_body, 2, a2);
      /* current file diverged: line 2 now different bytes */
      const char *cur_body = "one\nTWO-CHANGED\nthree\n";
      cJSON *edits = cJSON_CreateArray();
      cJSON_AddItemToArray(edits, edit_replace(a2, "TWO"));
      edit_anchored_result_t res;
      int rc = edit_anchored_plan(cur_body, strlen(cur_body), &snap, edits, &res);
      assert(rc == 1 && res.reject);
      const char *status = cJSON_GetObjectItem(res.reject, "status")->valuestring;
      assert(strcmp(status, "stale_anchor") == 0);
      assert(cJSON_GetArraySize(cJSON_GetObjectItem(res.reject, "failed")) == 1);
      assert(cJSON_GetArraySize(cJSON_GetObjectItem(res.reject, "context")) > 0);
      cJSON_Delete(res.reject);
      cJSON_Delete(edits);
      anchor_snapshot_dispose(&snap);
   }

   /* --- replace_range + insert_after + delete_range --- */
   {
      const char *body = "L1\nL2\nL3\nL4\nL5\n";
      anchor_snapshot_t snap;
      snap_of(body, &snap);
      char a2[24], a4[24], a1[24];
      anchor_of(body, 1, a1);
      anchor_of(body, 2, a2);
      anchor_of(body, 4, a4);
      cJSON *edits = cJSON_CreateArray();
      /* replace_range 2..3 with a single line */
      cJSON *rr = cJSON_CreateObject();
      cJSON_AddStringToObject(rr, "op", "replace_range");
      cJSON_AddStringToObject(rr, "from", a2);
      char a3[24];
      anchor_of(body, 3, a3);
      cJSON_AddStringToObject(rr, "to", a3);
      cJSON_AddStringToObject(rr, "text", "MID");
      cJSON_AddItemToArray(edits, rr);
      /* insert_after line 1 */
      cJSON *ins = cJSON_CreateObject();
      cJSON_AddStringToObject(ins, "op", "insert_after");
      cJSON_AddStringToObject(ins, "at", a1);
      cJSON_AddStringToObject(ins, "text", "NEW");
      cJSON_AddItemToArray(edits, ins);
      /* delete line 4 */
      cJSON *del = cJSON_CreateObject();
      cJSON_AddStringToObject(del, "op", "delete_range");
      cJSON_AddStringToObject(del, "from", a4);
      cJSON_AddStringToObject(del, "to", a4);
      cJSON_AddItemToArray(edits, del);
      edit_anchored_result_t res;
      int rc = edit_anchored_plan(body, strlen(body), &snap, edits, &res);
      assert(rc == 0);
      /* L1, +NEW, [L2 L3 -> MID], (L4 deleted), L5 */
      assert(strcmp(res.new_text, "L1\nNEW\nMID\nL5\n") == 0);
      free(res.new_text);
      cJSON_Delete(edits);
      anchor_snapshot_dispose(&snap);
   }

   /* --- OVERLAP rejection: two ranges that intersect --- */
   {
      const char *body = "a\nb\nc\nd\n";
      anchor_snapshot_t snap;
      snap_of(body, &snap);
      char a1[24], a2[24], a3[24];
      anchor_of(body, 1, a1);
      anchor_of(body, 2, a2);
      anchor_of(body, 3, a3);
      cJSON *edits = cJSON_CreateArray();
      cJSON *r1 = cJSON_CreateObject();
      cJSON_AddStringToObject(r1, "op", "replace_range");
      cJSON_AddStringToObject(r1, "from", a1);
      cJSON_AddStringToObject(r1, "to", a2);
      cJSON_AddStringToObject(r1, "text", "X");
      cJSON_AddItemToArray(edits, r1);
      cJSON *r2 = cJSON_CreateObject();
      cJSON_AddStringToObject(r2, "op", "replace_range");
      cJSON_AddStringToObject(r2, "from", a2);
      cJSON_AddStringToObject(r2, "to", a3);
      cJSON_AddStringToObject(r2, "text", "Y");
      cJSON_AddItemToArray(edits, r2);
      edit_anchored_result_t res;
      int rc = edit_anchored_plan(body, strlen(body), &snap, edits, &res);
      assert(rc == 1);
      assert(strcmp(cJSON_GetObjectItem(res.reject, "status")->valuestring, "conflicting_edits") ==
             0);
      cJSON_Delete(res.reject);
      cJSON_Delete(edits);
      anchor_snapshot_dispose(&snap);
   }

   /* --- CRLF preservation: unchanged lines keep CRLF; edited line normalized to
    *     the dominant CRLF terminator --- */
   {
      const char *body = "a\r\nb\r\nc\r\n";
      anchor_snapshot_t snap;
      snap_of(body, &snap);
      assert(strcmp(snap.eol, "\r\n") == 0);
      char a2[24];
      anchor_of(body, 2, a2);
      cJSON *edits = cJSON_CreateArray();
      cJSON_AddItemToArray(edits, edit_replace(a2, "B"));
      edit_anchored_result_t res;
      int rc = edit_anchored_plan(body, strlen(body), &snap, edits, &res);
      assert(rc == 0);
      assert(strcmp(res.new_text, "a\r\nB\r\nc\r\n") == 0);
      free(res.new_text);
      cJSON_Delete(edits);
      anchor_snapshot_dispose(&snap);
   }

   /* --- no-final-newline preserved when the last line is edited --- */
   {
      const char *body = "a\nb\nc"; /* no trailing newline */
      anchor_snapshot_t snap;
      snap_of(body, &snap);
      assert(snap.no_final_newline == 1);
      char a3[24];
      anchor_of(body, 3, a3);
      cJSON *edits = cJSON_CreateArray();
      cJSON_AddItemToArray(edits, edit_replace(a3, "C"));
      edit_anchored_result_t res;
      int rc = edit_anchored_plan(body, strlen(body), &snap, edits, &res);
      assert(rc == 0);
      assert(strcmp(res.new_text, "a\nb\nC") == 0); /* still no trailing newline */
      free(res.new_text);
      cJSON_Delete(edits);
      anchor_snapshot_dispose(&snap);
   }

   /* --- invalid edit: unknown op --- */
   {
      const char *body = "a\nb\n";
      anchor_snapshot_t snap;
      snap_of(body, &snap);
      cJSON *edits = cJSON_CreateArray();
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "op", "frobnicate");
      cJSON_AddStringToObject(e, "at", "1:aa");
      cJSON_AddItemToArray(edits, e);
      edit_anchored_result_t res;
      int rc = edit_anchored_plan(body, strlen(body), &snap, edits, &res);
      assert(rc == 1);
      assert(strcmp(cJSON_GetObjectItem(res.reject, "status")->valuestring, "invalid_edit") == 0);
      cJSON_Delete(res.reject);
      cJSON_Delete(edits);
      anchor_snapshot_dispose(&snap);
   }

   printf("ok\n");
   return 0;
}
