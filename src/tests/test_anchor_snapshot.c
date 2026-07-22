#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anchor_snapshot.h"

int main(void)
{
   printf("anchor_snapshot: ");

   /* --- line splitting: LF, no trailing newline, CRLF --- */
   {
      anchor_line_t *l = NULL;
      int n = anchor_split_lines("a\nbb\nccc\n", 9, &l);
      assert(n == 3);
      assert(l[0].content_len == 1 && l[0].len == 2);
      assert(l[2].content_len == 3 && l[2].len == 4);
      free(l);

      n = anchor_split_lines("a\nbb", 4, &l); /* no trailing newline */
      assert(n == 2);
      assert(l[1].content_len == 2 && l[1].len == 2);
      free(l);

      n = anchor_split_lines("a\r\nb\r\n", 6, &l); /* CRLF */
      assert(n == 2);
      assert(l[0].content_len == 1 && l[0].len == 3); /* "a" + "\r\n" */
      free(l);

      n = anchor_split_lines("", 0, &l);
      assert(n == 0 && l == NULL);
   }

   /* --- canonicalization: LF vs CRLF hash identically; BOM stripped on line 1;
    *     trailing whitespace is a real change --- */
   {
      uint64_t lf = anchor_line_digest("hello\n", 6, 0);
      uint64_t crlf = anchor_line_digest("hello\r\n", 7, 0);
      uint64_t bare = anchor_line_digest("hello", 5, 0);
      assert(lf == crlf && lf == bare);

      uint64_t ws = anchor_line_digest("hello \n", 7, 0);
      assert(ws != lf); /* trailing space must be caught */

      const char bom_line[] = "\xEF\xBB\xBFhello\n";
      uint64_t with_bom = anchor_line_digest(bom_line, sizeof(bom_line) - 1, 1);
      assert(with_bom == lf); /* BOM stripped on first line */
      uint64_t not_first = anchor_line_digest(bom_line, sizeof(bom_line) - 1, 0);
      assert(not_first != lf); /* BOM only stripped on line 1 */
   }

   /* --- short tag is the low byte in 2 hex --- */
   {
      char tag[3];
      anchor_short_tag(0xABCDEF12ull, tag);
      assert(strcmp(tag, "12") == 0);
      anchor_short_tag(0x5ull, tag);
      assert(strcmp(tag, "05") == 0);
   }

   /* --- shape detection: dominant EOL, BOM, final-newline --- */
   {
      char eol[3];
      int bom = -1, nofinal = -1;
      anchor_detect_shape("a\r\nb\r\nc\n", 8, eol, &bom, &nofinal);
      assert(strcmp(eol, "\r\n") == 0); /* 2 CRLF vs 1 LF */
      assert(bom == 0 && nofinal == 0);

      anchor_detect_shape("a\nb", 3, eol, &bom, &nofinal);
      assert(strcmp(eol, "\n") == 0);
      assert(nofinal == 1); /* no trailing newline */

      anchor_detect_shape("a\r\nb\n", 5, eol, &bom, &nofinal);
      assert(strcmp(eol, "\n") == 0); /* tie -> LF */
   }

   /* --- anchor_parse --- */
   {
      int ord = 0;
      unsigned tag = 0;
      assert(anchor_parse("12:f1", &ord, &tag) == 0 && ord == 12 && tag == 0xf1);
      assert(anchor_parse("1:0", &ord, &tag) == 0 && ord == 1 && tag == 0);
      assert(anchor_parse("0:aa", &ord, &tag) == -1); /* ordinal must be >=1 */
      assert(anchor_parse("12", &ord, &tag) == -1);   /* no hash */
      assert(anchor_parse("12:zz", &ord, &tag) == -1);
      assert(anchor_parse(":aa", &ord, &tag) == -1);
   }

   /* --- snapshot round-trip: create, get_copy, verify digests, identity --- */
   {
      const char *body = "function hello() {\n  return \"world\";\n}\n";
      char id[ANCHOR_SNAPSHOT_ID_MAX];
      int rc = anchor_snapshot_create("/tmp/foo.c", body, strlen(body), id);
      assert(rc == 0 && id[0] == 's');

      anchor_snapshot_t snap;
      int found = anchor_snapshot_get_copy(id, &snap);
      assert(found == 1);
      assert(snap.line_count == 3);
      assert(strcmp(snap.eol, "\n") == 0);
      assert(snap.no_final_newline == 0);
      assert(strcmp(snap.path, "/tmp/foo.c") == 0);

      /* recorded per-line digests match a fresh hash of the same bytes */
      anchor_line_t *l = NULL;
      int n = anchor_split_lines(body, strlen(body), &l);
      assert(n == 3);
      for (int i = 0; i < n; i++)
         assert(snap.line_digests[i] == anchor_line_digest(l[i].ptr, l[i].len, i == 0));
      free(l);
      anchor_snapshot_dispose(&snap);

      /* two reads mint distinct snapshots (no clobber) */
      char id2[ANCHOR_SNAPSHOT_ID_MAX];
      assert(anchor_snapshot_create("/tmp/foo.c", body, strlen(body), id2) == 0);
      assert(strcmp(id, id2) != 0);

      /* unknown id -> not found */
      anchor_snapshot_t miss;
      assert(anchor_snapshot_get_copy("sdeadbeef", &miss) == 0);
   }

   /* --- format_read prefixes LINE:HH| and carries the snapshot header --- */
   {
      const char *body = "alpha\nbeta\n";
      char *out = anchor_format_read(body, strlen(body), 0, 0, "sABC");
      assert(out);
      assert(strstr(out, "snapshot=sABC") != NULL);
      assert(strstr(out, "1:") != NULL && strstr(out, "| alpha") != NULL);
      assert(strstr(out, "2:") != NULL && strstr(out, "| beta") != NULL);
      free(out);

      /* offset/limit window */
      char *win = anchor_format_read("l1\nl2\nl3\nl4\n", 12, 1, 2, NULL);
      assert(win);
      assert(strstr(win, "| l2") != NULL && strstr(win, "| l3") != NULL);
      assert(strstr(win, "| l1") == NULL && strstr(win, "| l4") == NULL);
      free(win);
   }

   printf("ok\n");
   return 0;
}
