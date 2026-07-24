/* test_repetition_collapse.c: see headers/repetition_collapse.h for the contract.
 *
 * The checked-in fixture corpus is tests/repetition_collapse/corpus.json - the
 * test loads it via cJSON (already linked), exercises the detector on every
 * fixture, applies CI precision/false-positive/specificity/recall gates, and
 * re-runs the detector twice for repeatability checks.
 *
 * CI gating policy: the FP-zero gate and the precision-0.98 floor are enforced
 * with explicit exit(1) (NOT assert()), so they survive -DNDEBUG builds.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "headers/repetition_collapse.h"

static void banner(const char *name) { printf("  %s: ok\n", name); }

static void fail_hard(const char *fmt, ...)
{
   va_list ap;
   fprintf(stderr, "FAIL: ");
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fprintf(stderr, "\n");
   exit(1);
}

static int slurp_file(const char *path, char **out_buf, size_t *out_len)
{
   FILE *fp = fopen(path, "rb");
   if (!fp)
      return 0;
   fseek(fp, 0, SEEK_END);
   long n = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (n < 0) { fclose(fp); return 0; }
   char *buf = (char *)malloc((size_t)n + 1);
   if (!buf) { fclose(fp); return 0; }
   if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return 0; }
   buf[n] = '\0';
   fclose(fp);
   *out_buf = buf;
   *out_len = (size_t)n;
   return 1;
}

/* Try candidate corpus paths, in order: alongside running binary, relative to
 * the worktree root (unit-tests runs from src/), and up one level. */
static int load_corpus(char **out_buf, size_t *out_len)
{
   static const char *kCandidates[] = {
      "../tests/repetition_collapse/corpus.json",
      "tests/repetition_collapse/corpus.json",
      "../../tests/repetition_collapse/corpus.json",
      NULL
   };
   for (int i = 0; kCandidates[i]; i++)
   {
      if (slurp_file(kCandidates[i], out_buf, out_len))
         return 1;
   }
   return 0;
}

typedef struct
{
   char *id;
   char *body;
   int   expected_hit;
} loaded_fixture_t;

static loaded_fixture_t *load_fixtures(int *out_n)
{
   char *buf = NULL; size_t len = 0;
   if (!load_corpus(&buf, &len)) fail_hard("corpus missing");
   cJSON *root = cJSON_ParseWithLength(buf, len);
   if (!root) fail_hard("corpus JSON invalid");
   cJSON *fixtures = cJSON_GetObjectItemCaseSensitive(root, "fixtures");
   if (!cJSON_IsArray(fixtures)) { cJSON_Delete(root); fail_hard("corpus fixtures not array"); }
   int n = cJSON_GetArraySize(fixtures);
   loaded_fixture_t *out = (loaded_fixture_t *)calloc((size_t)n, sizeof(*out));
   for (int i = 0; i < n; i++)
   {
      cJSON *item = cJSON_GetArrayItem(fixtures, i);
      cJSON *id   = cJSON_GetObjectItemCaseSensitive(item, "id");
      cJSON *body = cJSON_GetObjectItemCaseSensitive(item, "body");
      cJSON *hit  = cJSON_GetObjectItemCaseSensitive(item, "expected_loop_hit");
      out[i].id = (id && cJSON_IsString(id) && id->valuestring) ? strdup(id->valuestring) : NULL;
      out[i].body = (body && cJSON_IsString(body) && body->valuestring) ? strdup(body->valuestring) : NULL;
      out[i].expected_hit = (cJSON_IsTrue(hit) || (cJSON_IsBool(hit) && !cJSON_IsFalse(hit))) ? 1 : 0;
   }
   cJSON_Delete(root);
   free(buf);
   *out_n = n;
   return out;
}

static void test_range_overlap_basic(void)
{
   if (rc_range_overlap(0, 10, 5, 15) != 1) fail_hard("overlap expected");
   if (rc_range_overlap(0, 5,  5, 10) != 0) fail_hard("no overlap expected (touch)");
   if (rc_range_overlap(0, 5, 10, 15) != 0) fail_hard("no overlap expected (gap)");
   banner("test_range_overlap_basic");
}

static void test_clean_buffer_no_hit(void)
{
   const char *s = "A short sentence that does not repeat anything verbatim across multiple paragraphs of mostly unrelated prose.";
   rc_result_t r;
   rc_detect(s, strlen(s), 0, 0, &r);
   if (r.hit) fail_hard("clean buffer flagged as loop");
   banner("test_clean_buffer_no_hit");
}

static int run_detect(const char *body, int expected_hit,
                      rc_result_t *out_first, rc_result_t *out_second)
{
   size_t len = strlen(body);
   rc_detect(body, len, 0, 0, out_first);
   rc_detect(body, len, 0, 0, out_second);
   int ok = 1;
   if (expected_hit)
   {
      if (!out_first->hit) { ok = 0; }
   }
   else
   {
      if (out_first->hit) { ok = 0; }
   }
   /* Repeatability: byte-equal loop_start_offset and loop_span_bytes across runs. */
   if (out_first->hit != out_second->hit ||
       out_first->loop_start_offset != out_second->loop_start_offset ||
       out_first->loop_span_bytes != out_second->loop_span_bytes ||
       out_first->repeats != out_second->repeats)
   {
      ok = 0;
   }
   return ok;
}

static void test_corpus_precision_and_repeatability(int *out_tp, int *out_fp,
                                                    int *out_tn, int *out_fn)
{
   int n = 0;
   loaded_fixture_t *fx = load_fixtures(&n);
   int tp = 0, fp = 0, tn = 0, fn = 0;
   int saw_hetero = 0;
   for (int i = 0; i < n; i++)
   {
      rc_result_t a, b;
      if (!run_detect(fx[i].body, fx[i].expected_hit, &a, &b))
      {
         fprintf(stderr, "  corpus fixture failed: id=%s expected_hit=%d got_hit=%d loop_start=%zu span=%zu repeats=%zu\n",
                 fx[i].id ? fx[i].id : "NULL", fx[i].expected_hit, a.hit, a.loop_start_offset, a.loop_span_bytes, a.repeats);
         fail_hard("corpus fixture failed");
      }
      if (fx[i].expected_hit)
      {
         if (a.hit) tp++; else fn++;
         if (strcmp(fx[i].id, "heterogeneous_object_curls") == 0)
         {
            saw_hetero = 1;
            if (a.hit != 1) fail_hard("heterogeneous_object_curls not flagged");
            if (a.loop_span_bytes < RC_DEFAULT_MIN_SPAN_BYTES)
               fail_hard("heterogeneous_object_curls period < min_span");
            if (a.repeats < RC_DEFAULT_MIN_REPEATS)
               fail_hard("heterogeneous_object_curls repeats < min_repeats");
         }
      }
      else
      {
         if (a.hit) fp++; else tn++;
      }
   }
   if (!saw_hetero) fail_hard("heterogeneous_object_curls fixture must exist");
   double prec = 0, rec = 0, spec = 0;
   rc_metrics(tp, fp, tn, fn, &prec, &rec, &spec);
   printf("  precision=%.4f recall=%.4f specificity=%.4f\n", prec, rec, spec);
   printf("  TP=%d FP=%d TN=%d FN=%d (corpus size=%d)\n", tp, fp, tn, fn, n);
   /* CI gates: explicit exit(1) so they survive -DNDEBUG. */
   if (fp > 0)
   {
      fprintf(stderr, "  CI GATE: %d false positive(s) against legitimate fixtures - hard-fail.\n", fp);
      fail_hard("CI FP-zero gate failed");
   }
   if (prec < RC_REQUIRED_PRECISION)
   {
      fprintf(stderr, "  CI GATE: precision %.4f below required %.4f\n", prec, RC_REQUIRED_PRECISION);
      fail_hard("CI precision floor failed");
   }
   *out_tp = tp; *out_fp = fp; *out_tn = tn; *out_fn = fn;
   for (int i = 0; i < n; i++) { free(fx[i].id); free(fx[i].body); } free(fx);
   banner("test_corpus_precision_and_repeatability");
}

static void test_short_period_inside_each_region_suppressed(void)
{
   const char *control =
      "plain repeated fragment xx\n"
      "plain repeated fragment xx\n"
      "plain repeated fragment xx\n"
      "plain repeated fragment xx\n"
      "plain repeated fragment xx\n";
   rc_result_t r;
   rc_detect(control, strlen(control), 8, 4, &r);
   if (!r.hit) fail_hard("control collapse not flagged");

   const char *fence =
      "Example:\n\n```\n  /\\_/\\  \n ( o.o ) \n  > ^ <  \n  /\\_/\\  \n ( o.o ) \n"
      "  > ^ <  \n  /\\_/\\  \n ( o.o ) \n  > ^ <  \n  /\\_/\\  \n ( o.o ) \n"
      "  > ^ <  \n```\n";
   rc_detect(fence, strlen(fence), 8, 4, &r);
   if (r.hit) fail_hard("fence region not suppressed");

   const char *list_body =
      "Steps:\n\n- step one runs first, please be patient\n"
      "- step one runs first, please be patient\n"
      "- step one runs first, please be patient\n"
      "- step one runs first, please be patient\n"
      "- step one runs first, please be patient\n";
   rc_detect(list_body, strlen(list_body), 8, 4, &r);
   if (r.hit) fail_hard("list_body region not suppressed");

   const char *code =
      "Output:\n\n"
      "    printf(\"hello world\\n\");\n"
      "    printf(\"hello world\\n\");\n"
      "    printf(\"hello world\\n\");\n"
      "    printf(\"hello world\\n\");\n"
      "    printf(\"hello world\\n\");\n";
   rc_detect(code, strlen(code), 8, 4, &r);
   if (r.hit) fail_hard("code_block region not suppressed");

   const char *table =
      "Results:\n\n"
      "| name  | value |\n"
      "|-------|-------|\n"
      "| alpha | one   |\n"
      "| alpha | one   |\n"
      "| alpha | one   |\n"
      "| alpha | one   |\n"
      "| alpha | one   |\n";
   rc_detect(table, strlen(table), 8, 4, &r);
   if (r.hit) fail_hard("table region not suppressed");

   const char *json =
      "Schema samples:\n\n"
      "{\"id\":1,\"name\":\"a\",\"kind\":\"repeat\"}\n"
      "{\"id\":1,\"name\":\"a\",\"kind\":\"repeat\"}\n"
      "{\"id\":1,\"name\":\"a\",\"kind\":\"repeat\"}\n"
      "{\"id\":1,\"name\":\"a\",\"kind\":\"repeat\"}\n"
      "{\"id\":1,\"name\":\"a\",\"kind\":\"repeat\"}\n";
   rc_detect(json, strlen(json), 8, 4, &r);
   if (r.hit) fail_hard("json fragment not suppressed");

   banner("test_short_period_inside_each_region_suppressed");
}

static void test_mid_stream_collapse_detected(void)
{
   /* Mid-stream collapse (F3): the full-buffer scanner must find a repeat
    * that does not extend to the tail. */
   const char *s =
      "Completely unrelated prose line one with no repetition at all.\n"
      "Completely unrelated prose line two with no repetition at all.\n"
      "Repeated verbatim payload line alpha bravo charlie delta echo foxtrot.\n"
      "Repeated verbatim payload line alpha bravo charlie delta echo foxtrot.\n"
      "Repeated verbatim payload line alpha bravo charlie delta echo foxtrot.\n"
      "Repeated verbatim payload line alpha bravo charlie delta echo foxtrot.\n"
      "Tail prose that is also unrelated to the repeated payload.\n";
   rc_result_t r;
   rc_detect(s, strlen(s), 0, 0, &r);
   if (!r.hit) fail_hard("mid-stream collapse not detected");
   banner("test_mid_stream_collapse_detected");
}

static void test_cand_end_is_exact(void)
{
   /* F4: cand_end must be exact, not clamped to len. The repeat count at the
    * chosen position is the count of byte-equal copies - the reported range
    * must not extend past the actual last repeating byte. We construct a
    * buffer where the repeat has a 60-byte period and 4 copies plus a
    * non-repeating tail; the reported loop_start_offset + repeats*span_bytes
    * must equal the byte just past the last repeated copy (NOT past the tail). */
   char line[128];
   memset(line, 'A', 70);
   line[70] = '\n';
   line[71] = '\0';
   char buf[1024];
   size_t off = 0;
   for (int i = 0; i < 4; i++) {
      memcpy(buf + off, line, 71);
      off += 71;
   }
   /* Tail - 50 bytes of 'X' (less than min_span). */
   for (int i = 0; i < 50; i++) buf[off++] = 'X';
   buf[off] = '\0';
   rc_result_t r;
   rc_detect(buf, off, 60, 4, &r);
   if (!r.hit) fail_hard("exact-end control not flagged");
   size_t expected_end = r.loop_start_offset + r.repeats * r.loop_span_bytes;
   /* The reported end (loop_start_offset + repeats*span_bytes) must be
    * exactly the byte just past the 4th A-line. */
   size_t expected_repeat_end = 4 * 71;
   if (expected_end != expected_repeat_end)
      fail_hard("cand_end not exact: got %zu expected %zu", expected_end, expected_repeat_end);
   banner("test_cand_end_is_exact");
}

static void test_repeatability_byte_equal(void)
{
   /* AC: running the detector twice on the same byte sequence must produce
    * identical loop_start_offset and loop_span_bytes. */
   const char *s =
      "intro\n"
      "repeated verbatim payload line alpha bravo charlie delta echo foxtrot\n"
      "repeated verbatim payload line alpha bravo charlie delta echo foxtrot\n"
      "repeated verbatim payload line alpha bravo charlie delta echo foxtrot\n"
      "repeated verbatim payload line alpha bravo charlie delta echo foxtrot\n";
   rc_result_t a, b;
   rc_detect(s, strlen(s), 0, 0, &a);
   rc_detect(s, strlen(s), 0, 0, &b);
   if (a.hit != b.hit) fail_hard("repeatability hit mismatch");
   if (a.loop_start_offset != b.loop_start_offset) fail_hard("repeatability loop_start mismatch");
   if (a.loop_span_bytes != b.loop_span_bytes) fail_hard("repeatability span mismatch");
   if (a.repeats != b.repeats) fail_hard("repeatability repeats mismatch");
   banner("test_repeatability_byte_equal");
}

int main(void)
{
   printf("repetition_collapse tests:\n");
   test_range_overlap_basic();
   test_clean_buffer_no_hit();
   test_short_period_inside_each_region_suppressed();
   test_mid_stream_collapse_detected();
   test_cand_end_is_exact();
   test_repeatability_byte_equal();
   int tp = 0, fp = 0, tn = 0, fn = 0;
   test_corpus_precision_and_repeatability(&tp, &fp, &tn, &fn);
   printf("All repetition_collapse tests passed.\n");
   return 0;
}
