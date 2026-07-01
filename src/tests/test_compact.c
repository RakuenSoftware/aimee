/* test_compact.c: unit tests for tool-result compaction (the shared compact_body core) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../headers/compact.h"

#define PASS(name) printf("  PASS: %s\n", name)

/* ------------------------------------------------------------------ helpers */

static char *make_str(char c, size_t len)
{
   char *s = malloc(len + 1);
   assert(s);
   memset(s, c, len);
   s[len] = '\0';
   return s;
}

/* Drive the shared core into a heap string so these golden assertions read the
 * same way the removed compact_tool_result() did. Sized per the compact_body
 * contract (raw_len + COMPACT_JSON_SUMMARY_MAX) so no strategy is ever truncated. */
static char *compact_str(const char *raw, size_t raw_len, const compact_config_t *cfg,
                         const char *tool)
{
   size_t cap = raw_len + COMPACT_JSON_SUMMARY_MAX + 1;
   char *buf = malloc(cap);
   assert(buf);
   compact_body(raw, raw_len, tool, cfg, buf, cap);
   return buf;
}

/* ------------------------------------------------------------------ pass-through */

static void test_small_passthrough(void)
{
   const char *input = "hello world";
   char *out = compact_str(input, strlen(input), NULL, NULL);
   assert(out);
   assert(strcmp(out, input) == 0);
   free(out);
   PASS("small_passthrough");
}

static void test_exactly_threshold_passthrough(void)
{
   /* A string exactly at the default threshold (4096) should pass through */
   char *input = make_str('x', COMPACT_DEFAULT_THRESHOLD);
   char *out = compact_str(input, COMPACT_DEFAULT_THRESHOLD, NULL, NULL);
   assert(out);
   assert(strlen(out) == COMPACT_DEFAULT_THRESHOLD);
   assert(memcmp(out, input, COMPACT_DEFAULT_THRESHOLD) == 0);
   free(out);
   free(input);
   PASS("exactly_threshold_passthrough");
}

/* ------------------------------------------------------------------ disabled */

static void test_disabled_passthrough(void)
{
   compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.enabled = 0;

   char *big = make_str('z', COMPACT_DEFAULT_THRESHOLD * 2);
   size_t len = (size_t)(COMPACT_DEFAULT_THRESHOLD * 2);
   char *out = compact_str(big, len, &cfg, NULL);
   assert(out);
   assert(strlen(out) == len);
   free(out);
   free(big);
   PASS("disabled_passthrough");
}

/* ------------------------------------------------------------------ per-tool override */

static void test_per_tool_disabled(void)
{
   compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.enabled = 1;
   snprintf(cfg.per_tool[0].tool, sizeof(cfg.per_tool[0].tool), "bash");
   cfg.per_tool[0].threshold = -1; /* disabled for bash */
   cfg.per_tool_count = 1;

   char *big = make_str('b', COMPACT_DEFAULT_THRESHOLD * 2);
   size_t len = (size_t)(COMPACT_DEFAULT_THRESHOLD * 2);
   char *out = compact_str(big, len, &cfg, "bash");
   assert(out);
   assert(strlen(out) == len); /* not compacted */
   free(out);
   free(big);
   PASS("per_tool_disabled");
}

static void test_per_tool_threshold_lower(void)
{
   compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.enabled = 1;
   cfg.head_bytes = 64;
   cfg.tail_bytes = 64;
   snprintf(cfg.per_tool[0].tool, sizeof(cfg.per_tool[0].tool), "read_file");
   cfg.per_tool[0].threshold = 512; /* lower threshold for read_file */
   cfg.per_tool_count = 1;

   /* 2 KB input: bigger than the 512-byte per-tool threshold */
   char *big = make_str('r', 2048);
   char *out = compact_str(big, 2048, &cfg, "read_file");
   assert(out);
   assert(strlen(out) < 2048); /* should have been compacted */
   free(out);
   free(big);
   PASS("per_tool_threshold_lower");
}

/* ------------------------------------------------------------------ plain-text head+tail */

static void test_plaintext_contains_head_and_tail(void)
{
   /* Build a string big enough to trigger compaction */
   size_t total = (size_t)(COMPACT_DEFAULT_THRESHOLD + 1000);
   char *input = malloc(total + 1);
   assert(input);
   memset(input, 'm', total);
   /* Mark the head region with 'H' and tail with 'T' */
   memset(input, 'H', COMPACT_DEFAULT_HEAD_BYTES);
   memset(input + total - COMPACT_DEFAULT_TAIL_BYTES, 'T', COMPACT_DEFAULT_TAIL_BYTES);
   input[total] = '\0';

   char *out = compact_str(input, total, NULL, NULL);
   assert(out);

   /* Output should start with 'H' and end with 'T' */
   assert(out[0] == 'H');
   size_t out_len = strlen(out);
   assert(out[out_len - 1] == 'T');

   /* Should contain the truncation notice */
   assert(strstr(out, "omitted") != NULL);

   free(out);
   free(input);
   PASS("plaintext_contains_head_and_tail");
}

static void test_custom_head_tail(void)
{
   compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.enabled = 1;
   cfg.threshold = 100;
   cfg.head_bytes = 20;
   cfg.tail_bytes = 20;

   /* 200 bytes of input */
   char *input = malloc(201);
   assert(input);
   memset(input, 'A', 20);
   memset(input + 20, 'M', 160);
   memset(input + 180, 'Z', 20);
   input[200] = '\0';

   char *out = compact_str(input, 200, &cfg, NULL);
   assert(out);
   assert(out[0] == 'A');
   size_t out_len = strlen(out);
   assert(out[out_len - 1] == 'Z');
   assert(strstr(out, "omitted") != NULL);

   free(out);
   free(input);
   PASS("custom_head_tail");
}

/* ------------------------------------------------------------------ JSON summary */

static void test_json_object_summary(void)
{
   /* Build a JSON object larger than the default threshold */
   char *json = NULL;
   size_t json_len = 0;

   /* Start with a fixed prefix */
   const char *prefix = "{\"status\": \"ok\", \"data\": \"";
   size_t prefix_len = strlen(prefix);

   /* Pad the value to exceed threshold */
   size_t pad = COMPACT_DEFAULT_THRESHOLD + 100;
   json = malloc(prefix_len + pad + 4);
   assert(json);
   memcpy(json, prefix, prefix_len);
   memset(json + prefix_len, 'x', pad);
   json[prefix_len + pad] = '"';
   json[prefix_len + pad + 1] = '}';
   json[prefix_len + pad + 2] = '\0';
   json_len = prefix_len + pad + 2;

   char *out = compact_str(json, json_len, NULL, NULL);
   assert(out);
   /* Should contain the compacted JSON summary marker */
   assert(strstr(out, "compacted JSON summary") != NULL || strstr(out, "status") != NULL ||
          strstr(out, "omitted") != NULL);
   /* Should NOT pass through the full original */
   assert(strlen(out) < json_len);

   free(out);
   free(json);
   PASS("json_object_summary");
}

static void test_json_array_summary(void)
{
   /* Build a large JSON array */
   const char *prefix = "[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}";
   size_t prefix_len = strlen(prefix);
   size_t pad = COMPACT_DEFAULT_THRESHOLD + 200;
   char *json = malloc(prefix_len + pad + 2);
   assert(json);
   memcpy(json, prefix, prefix_len);
   /* Add many more items as a long string */
   memset(json + prefix_len, ',', pad);
   json[prefix_len + pad] = ']';
   json[prefix_len + pad + 1] = '\0';

   char *out = compact_str(json, prefix_len + pad + 1, NULL, NULL);
   assert(out);
   assert(strlen(out) < prefix_len + pad + 1);

   free(out);
   free(json);
   PASS("json_array_summary");
}

/* The JSON structural summary is internally bounded to compact_json_summary's fixed
 * summary[2048] buffer, so it stays < COMPACT_JSON_SUMMARY_MAX. This pins that
 * invariant (the buffer-sizing formula out_cap = raw_len + COMPACT_JSON_SUMMARY_MAX
 * + 1 relies on it) against a future compact.c refactor: a JSON object with thousands
 * of keys would overflow any naive summary, yet must come back bounded and intact. */
static void test_json_summary_bounded(void)
{
   size_t cap_in = 200000;
   char *json = malloc(cap_in);
   assert(json);
   size_t p = 0;
   p += (size_t)snprintf(json + p, cap_in - p, "{");
   for (int i = 0; i < 4000 && p < cap_in - 64; i++)
      p += (size_t)snprintf(json + p, cap_in - p, "%s\"key_%d\":%d", i ? "," : "", i, i);
   p += (size_t)snprintf(json + p, cap_in - p, "}");
   size_t jl = p;

   size_t cap = jl + COMPACT_JSON_SUMMARY_MAX + 1; /* the formula both seams use */
   char *buf = malloc(cap);
   assert(buf);
   size_t n = compact_body(json, jl, NULL, NULL, buf, cap);
   assert(n > 0);
   assert(n < COMPACT_JSON_SUMMARY_MAX); /* summary never exceeds the documented bound */
   assert(buf[n] == '\0');               /* NUL-terminated, not truncated mid-write */
   assert(strstr(buf, "compacted JSON summary") != NULL); /* JSON-summary path taken */
   free(buf);
   free(json);
   PASS("json_summary_bounded");
}

/* ------------------------------------------------------------------ buffer contract */

/* compact_body writes into a caller buffer and never exceeds out_cap-1. A tight
 * buffer must still leave a valid NUL-terminated (truncated) result, never a
 * write past the end. */
static void test_buffer_cap_respected(void)
{
   char *input = make_str('q', COMPACT_DEFAULT_THRESHOLD + 1000);
   size_t len = (size_t)(COMPACT_DEFAULT_THRESHOLD + 1000);
   char small[64];
   size_t n = compact_body(input, len, NULL, NULL, small, sizeof(small));
   assert(n <= sizeof(small) - 1);
   assert(small[n] == '\0');
   free(input);
   PASS("buffer_cap_respected");
}

/* ------------------------------------------------------------------ null / empty input */

static void test_null_input(void)
{
   char buf[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
   size_t n = compact_body(NULL, 0, NULL, NULL, buf, sizeof(buf));
   assert(n == 0);
   assert(buf[0] == '\0');
   PASS("null_input");
}

static void test_empty_body(void)
{
   char buf[8] = {'y'};
   size_t n = compact_body("", 0, NULL, NULL, buf, sizeof(buf));
   assert(n == 0);
   assert(buf[0] == '\0');
   PASS("empty_body");
}

/* ------------------------------------------------------------------ main */

int main(void)
{
   printf("compact:\n");

   test_null_input();
   test_empty_body();
   test_small_passthrough();
   test_exactly_threshold_passthrough();
   test_disabled_passthrough();
   test_per_tool_disabled();
   test_per_tool_threshold_lower();
   test_plaintext_contains_head_and_tail();
   test_custom_head_tail();
   test_json_object_summary();
   test_json_array_summary();
   test_json_summary_bounded();
   test_buffer_cap_respected();

   printf("all compact tests passed\n");
   return 0;
}
