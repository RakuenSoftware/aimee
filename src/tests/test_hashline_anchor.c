/* test_hashline_anchor.c: unit tests for composite anchors + immutable snapshots.
 * Covers the roundtable-mandated cases: distinct snapshots for concurrent reads
 * of identical content, CRLF/BOM/trailing-whitespace canonicalization, duplicate
 * identical lines (same digest, distinct ordinal), sid scoping, eviction, TTL. */
#include "hashline_anchor.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void test_canonicalization(void)
{
   /* A CRLF-terminated line (content ends in '\r', had_terminator=1) and an
    * LF-terminated line of the same text hash identically. */
   const char *lf = "return \"world\";";
   const char *crlf = "return \"world\";\r";
   assert(hashline_digest64(lf, strlen(lf), 0, 1) == hashline_digest64(crlf, strlen(crlf), 0, 1));

   /* But a trailing '\r' with NO terminator (bare CR at end of file) is real
    * content: `foo` and `foo\r` must NOT collide (the evasion the review caught). */
   assert(hashline_digest64("foo", 3, 0, 0) != hashline_digest64("foo\r", 4, 0, 0));
   /* ...while the same `foo\r` as a CRLF terminator (had_terminator=1) DOES fold
    * to `foo`. */
   assert(hashline_digest64("foo", 3, 0, 1) == hashline_digest64("foo\r", 4, 0, 1));

   /* Trailing whitespace IS significant (a real edit). */
   const char *ws = "return \"world\"; ";
   assert(hashline_digest64(lf, strlen(lf), 0, 1) != hashline_digest64(ws, strlen(ws), 0, 1));

   /* A UTF-8 BOM on line 1 is stripped before hashing; the same text without a
    * BOM hashes identically. */
   const char bom_line[] = {(char)0xEF, (char)0xBB, (char)0xBF, 'h', 'i'};
   const char *plain = "hi";
   assert(hashline_digest64(bom_line, sizeof(bom_line), 1, 1) ==
          hashline_digest64(plain, strlen(plain), 0, 1));

   /* BOM bytes NOT on line 1 (is_first_line=0) are hashed verbatim. */
   assert(hashline_digest64(bom_line, sizeof(bom_line), 0, 1) !=
          hashline_digest64(plain, strlen(plain), 0, 1));

   /* Canonical view is a no-copy pointer into the input; CRLF terminator's CR
    * dropped (had_terminator=1). */
   const char *vp = NULL;
   size_t vl = 0;
   hashline_canonicalize_line(crlf, strlen(crlf), 0, 1, &vp, &vl);
   assert(vp == crlf && vl == strlen(lf));
   printf("  canonicalization ok\n");
}

static void test_display_tag(void)
{
   char a[HASHLINE_DISPLAY_TAG_HEX + 1];
   char b[HASHLINE_DISPLAY_TAG_HEX + 1];
   hashline_display_tag(0x1234abcdULL, a, sizeof(a));
   hashline_display_tag(0x1234abcdULL, b, sizeof(b));
   assert(strlen(a) == HASHLINE_DISPLAY_TAG_HEX);
   assert(strcmp(a, b) == 0); /* deterministic */
   /* Low HASHLINE_DISPLAY_TAG_HEX nibbles of the digest. */
   assert(strcmp(a, "bcd") == 0);
   printf("  display tag ok\n");
}

static void test_line_digests(void)
{
   /* Two identical lines at different ordinals share a digest (the ordinal, not
    * the digest, disambiguates them). Trailing no-newline segment is a line. */
   const char *content = "foo\nbar\nfoo"; /* 3 lines, last has no '\n' */
   assert(hashline_line_count(content, strlen(content)) == 3);
   uint64_t d[3];
   size_t n = hashline_line_digests(content, strlen(content), d, 3);
   assert(n == 3);
   assert(d[0] == d[2]); /* both "foo" */
   assert(d[0] != d[1]);

   /* Empty buffer => 0 lines. */
   assert(hashline_line_count("", 0) == 0);

   /* A trailing newline does not add a phantom empty line beyond content. */
   const char *tn = "a\nb\n";
   assert(hashline_line_count(tn, strlen(tn)) == 2);
   printf("  line digests ok\n");
}

static void test_snapshot_roundtrip(void)
{
   hashline_snapshot_evict_all();
   const char *path = "src/foo.c";
   const char *content = "line1\nline2\nline2\n"; /* dup line2 */
   char *id = hashline_snapshot_mint("sessA", path, content, strlen(content));
   assert(id != NULL);

   hashline_snapshot_view_t v = {0};
   assert(hashline_snapshot_get("sessA", id, &v) == 1);
   assert(strcmp(v.path, path) == 0);
   assert(v.size == strlen(content));
   assert(v.line_count == 3);
   assert(v.line_digests[1] == v.line_digests[2]); /* both "line2" */
   assert(v.file_digest == hashline_digest64_raw(content, strlen(content)));
   hashline_snapshot_view_free(&v);

   /* Wrong session id must not resolve. */
   assert(hashline_snapshot_get("sessB", id, &v) == 0);
   /* NULL sid lookup ignores scoping and resolves. */
   assert(hashline_snapshot_get(NULL, id, &v) == 1);
   hashline_snapshot_view_free(&v);
   /* Unknown id misses. */
   assert(hashline_snapshot_get("sessA", "s-nope", &v) == 0);
   free(id);
   printf("  snapshot roundtrip ok\n");
}

static void test_concurrent_reads_distinct(void)
{
   hashline_snapshot_evict_all();
   /* N reads of IDENTICAL content must mint N distinct, independently
    * retrievable snapshots (no (path,ordinal) clobbering). */
   const char *content = "same\ncontent\n";
   char *id1 = hashline_snapshot_mint("s", "p", content, strlen(content));
   char *id2 = hashline_snapshot_mint("s", "p", content, strlen(content));
   assert(id1 && id2);
   assert(strcmp(id1, id2) != 0);
   hashline_snapshot_view_t v = {0};
   assert(hashline_snapshot_get("s", id1, &v) == 1);
   hashline_snapshot_view_free(&v);
   assert(hashline_snapshot_get("s", id2, &v) == 1);
   hashline_snapshot_view_free(&v);
   free(id1);
   free(id2);
   printf("  concurrent-read distinct snapshots ok\n");
}

static void test_eviction_and_ttl(void)
{
   hashline_snapshot_evict_all();
   char *id = hashline_snapshot_mint("s", "p", "x\n", 2);
   assert(id != NULL);
   hashline_snapshot_view_t v = {0};
   assert(hashline_snapshot_get("s", id, &v) == 1);
   hashline_snapshot_view_free(&v);

   /* TTL expiry: a tiny TTL + a short sleep evicts on next access. */
   hashline_snapshot_set_ttl_ms(1);
   struct timespec ts = {0, 5 * 1000 * 1000}; /* 5ms */
   nanosleep(&ts, NULL);
   assert(hashline_snapshot_get("s", id, &v) == 0); /* expired => miss (re-read) */
   hashline_snapshot_set_ttl_ms(0);                 /* restore default */
   free(id);

   /* evict_all clears everything. */
   char *id2 = hashline_snapshot_mint("s", "p", "y\n", 2);
   assert(id2 && hashline_snapshot_get("s", id2, &v) == 1);
   hashline_snapshot_view_free(&v);
   hashline_snapshot_evict_all();
   assert(hashline_snapshot_get("s", id2, &v) == 0);
   free(id2);

   /* A fetched view is a caller-owned DEEP COPY: it stays valid after the source
    * slot is evicted out from under it (no borrowed-pointer use-after-free). */
   char *id3 = hashline_snapshot_mint("s", "p", "hello\nworld\n", 12);
   hashline_snapshot_view_t v3 = {0};
   assert(id3 && hashline_snapshot_get("s", id3, &v3) == 1);
   hashline_snapshot_evict_all(); /* frees the store slot + its buffers */
   assert(v3.line_count == 2 && strcmp(v3.path, "p") == 0);
   assert(v3.line_digests[0] != v3.line_digests[1]); /* still readable */
   hashline_snapshot_view_free(&v3);
   free(id3);
   printf("  eviction + ttl + deep-copy-survives-eviction ok\n");
}

int main(void)
{
   test_canonicalization();
   test_display_tag();
   test_line_digests();
   test_snapshot_roundtrip();
   test_concurrent_reads_distinct();
   test_eviction_and_ttl();
   printf("test_hashline_anchor: all passed\n");
   return 0;
}
