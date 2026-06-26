/* test_code_span.c: ingress-compression P2 — the code_span_get recovery resolver.
 * Pure tests of code_span_read() against temp files through the default (shared)
 * workspace provider: happy-path slice, line-span clamp, B4 path-safety
 * (traversal / outside-root / control chars), and the drift baseline (source_version
 * is content-sensitive — B7's independent-oracle property at the unit level). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "code_span.h"

#define PASS(name) printf("  PASS: %s\n", (name))

static char g_root[4096];

/* Build a fresh temp dir to act as the project root and return its realpath. */
static void make_root(void)
{
   char tmpl[] = "/tmp/code_span_test_XXXXXX";
   char *d = mkdtemp(tmpl);
   assert(d);
   /* realpath so containment compares apples to apples (e.g. /tmp symlinks). */
   char *r = realpath(d, g_root);
   assert(r);
}

static void write_file(const char *rel, const char *content)
{
   char path[4096];
   snprintf(path, sizeof(path), "%s/%s", g_root, rel);
   FILE *f = fopen(path, "wb");
   assert(f);
   fwrite(content, 1, strlen(content), f);
   fclose(f);
}

static const char *jstr(cJSON *o, const char *k)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   return cJSON_IsString(v) ? v->valuestring : NULL;
}
static int jint(cJSON *o, const char *k)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   return cJSON_IsNumber(v) ? v->valueint : -999999;
}
static int jbool_true(cJSON *o, const char *k)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   return cJSON_IsTrue(v);
}

int main(void)
{
   make_root();
   write_file("a.txt", "line1\nline2\nline3\nline4\nline5\n");

   /* 1. Happy path: read [2,4]. */
   {
      cJSON *r = code_span_read("proj", g_root, "a.txt", 2, 4, 400);
      assert(r);
      assert(!jstr(r, "error"));
      assert(strcmp(jstr(r, "content"), "line2\nline3\nline4\n") == 0);
      assert(jint(r, "line_start") == 2);
      assert(jint(r, "line_end") == 4);
      assert(jint(r, "line_count") == 3);
      assert(!jbool_true(r, "truncated"));
      const char *v1 = jstr(r, "source_version");
      assert(v1 && strlen(v1) == 64); /* sha256 hex */
      cJSON_Delete(r);
      PASS("happy-path slice [2,4]");
   }

   /* 2. Clamp to max_lines: request [1,100] with max 2 -> 2 lines, truncated. */
   {
      cJSON *r = code_span_read("proj", g_root, "a.txt", 1, 100, 2);
      assert(r);
      assert(jint(r, "line_count") == 2);
      assert(jbool_true(r, "truncated"));
      assert(strcmp(jstr(r, "content"), "line1\nline2\n") == 0);
      cJSON_Delete(r);
      PASS("clamp to max_lines");
   }

   /* 2b. Span exceeds max_lines but the FILE is shorter -> not truncated (we
    *     returned everything the request covered). */
   {
      cJSON *r = code_span_read("proj", g_root, "a.txt", 1, 1000, 400);
      assert(r);
      assert(jint(r, "line_count") == 5);
      assert(!jbool_true(r, "truncated"));
      cJSON_Delete(r);
      PASS("clamp but short file -> not truncated");
   }

   /* 3. Beyond EOF: line_start past the file -> 0 lines, no error. */
   {
      cJSON *r = code_span_read("proj", g_root, "a.txt", 99, 100, 400);
      assert(r);
      assert(!jstr(r, "error"));
      assert(jint(r, "line_count") == 0);
      cJSON_Delete(r);
      PASS("beyond EOF -> empty");
   }

   /* 4. Traversal: "../escape" must be refused (guardrails and/or containment). */
   {
      cJSON *r = code_span_read("proj", g_root, "../a.txt", 1, 1, 400);
      assert(r);
      assert(jstr(r, "error")); /* rejected */
      cJSON_Delete(r);
      PASS("traversal ../ refused");
   }

   /* 5. Absolute path outside the root -> "outside workspace". */
   {
      cJSON *r = code_span_read("proj", g_root, "/etc/hostname", 1, 1, 400);
      assert(r);
      assert(jstr(r, "error"));
      cJSON_Delete(r);
      PASS("absolute path outside root refused");
   }

   /* 6. Control chars in file_path -> refused. */
   {
      cJSON *r = code_span_read("proj", g_root, "a\x01.txt", 1, 1, 400);
      assert(r);
      assert(jstr(r, "error"));
      cJSON_Delete(r);
      PASS("control chars refused");
   }

   /* 7. Drift baseline: same content -> same version; mutate -> different. */
   {
      cJSON *r1 = code_span_read("proj", g_root, "a.txt", 1, 5, 400);
      char v_before[80];
      snprintf(v_before, sizeof(v_before), "%s", jstr(r1, "source_version"));
      cJSON_Delete(r1);

      /* same bytes -> identical version (deterministic, content-addressed) */
      cJSON *r2 = code_span_read("proj", g_root, "a.txt", 1, 5, 400);
      assert(strcmp(v_before, jstr(r2, "source_version")) == 0);
      cJSON_Delete(r2);

      /* source_version is the WHOLE-FILE hash, so a different span of the same
       * unchanged file reports the SAME version (it identifies the file state,
       * not the slice). */
      cJSON *r2b = code_span_read("proj", g_root, "a.txt", 2, 3, 400);
      assert(strcmp(v_before, jstr(r2b, "source_version")) == 0);
      cJSON_Delete(r2b);

      /* mutate the span content -> version must change (independent oracle: the
       * test changed the bytes, so a correct content hash MUST differ). */
      write_file("a.txt", "line1\nCHANGED\nline3\nline4\nline5\n");
      cJSON *r3 = code_span_read("proj", g_root, "a.txt", 1, 5, 400);
      assert(strcmp(v_before, jstr(r3, "source_version")) != 0);
      cJSON_Delete(r3);
      PASS("source_version is content-sensitive (drift baseline)");
   }

   /* 8. Missing file -> error, never NULL. */
   {
      cJSON *r = code_span_read("proj", g_root, "nope.txt", 1, 1, 400);
      assert(r);
      assert(jstr(r, "error"));
      cJSON_Delete(r);
      PASS("missing file -> error");
   }

   printf("All code_span tests passed.\n");
   return 0;
}
