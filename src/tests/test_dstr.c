/* test_dstr.c: unit tests for the dynamic string library */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dstr.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define PASS(name) printf("  PASS: %s\n", name)

static void test_init_free(void)
{
   dstr_t s;
   dstr_init(&s);
   assert(dstr_len(&s) == 0);
   assert(strcmp(dstr_cstr(&s), "") == 0);
   dstr_free(&s);
   assert(s.data == NULL);
   assert(s.len == 0);
   PASS("init_free");
}

static void test_append_str(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append_str(&s, "hello");
   assert(dstr_len(&s) == 5);
   assert(strcmp(dstr_cstr(&s), "hello") == 0);
   dstr_append_str(&s, " world");
   assert(dstr_len(&s) == 11);
   assert(strcmp(dstr_cstr(&s), "hello world") == 0);
   dstr_free(&s);
   PASS("append_str");
}

static void test_append_char(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append_char(&s, 'a');
   dstr_append_char(&s, 'b');
   dstr_append_char(&s, 'c');
   assert(dstr_len(&s) == 3);
   assert(strcmp(dstr_cstr(&s), "abc") == 0);
   dstr_free(&s);
   PASS("append_char");
}

static void test_append_raw(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append(&s, "hello\0world", 11);
   assert(dstr_len(&s) == 11);
   assert(memcmp(s.data, "hello\0world", 11) == 0);
   dstr_free(&s);
   PASS("append_raw");
}

static void test_appendf(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_appendf(&s, "num=%d str=%s", 42, "test");
   assert(strcmp(dstr_cstr(&s), "num=42 str=test") == 0);
   dstr_appendf(&s, " more=%d", 99);
   assert(strcmp(dstr_cstr(&s), "num=42 str=test more=99") == 0);
   dstr_free(&s);
   PASS("appendf");
}

static void test_reset(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append_str(&s, "hello world");
   size_t old_cap = s.cap;
   dstr_reset(&s);
   assert(dstr_len(&s) == 0);
   assert(strcmp(dstr_cstr(&s), "") == 0);
   assert(s.cap == old_cap); /* buffer retained */
   dstr_free(&s);
   PASS("reset");
}

static void test_steal(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append_str(&s, "stolen");
   char *p = dstr_steal(&s);
   assert(strcmp(p, "stolen") == 0);
   assert(s.data == NULL);
   assert(s.len == 0);
   free(p);

   /* Steal from empty */
   dstr_init(&s);
   assert(dstr_steal(&s) == NULL);
   PASS("steal");
}

static void test_reserve(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_reserve(&s, 1000);
   assert(s.cap >= 1001);
   dstr_free(&s);
   PASS("reserve");
}

static void test_large_append(void)
{
   dstr_t s;
   dstr_init(&s);
   /* Append 10000 characters to trigger multiple reallocs */
   for (int i = 0; i < 10000; i++)
      dstr_append_char(&s, 'x');
   assert(dstr_len(&s) == 10000);
   assert(s.data[0] == 'x');
   assert(s.data[9999] == 'x');
   assert(s.data[10000] == '\0');
   dstr_free(&s);
   PASS("large_append");
}

static void test_appendf_large(void)
{
   dstr_t s;
   dstr_init(&s);
   /* Format a string longer than the initial capacity */
   char buf[200];
   memset(buf, 'A', 199);
   buf[199] = '\0';
   dstr_appendf(&s, "prefix:%s:suffix", buf);
   assert(dstr_len(&s) == 7 + 199 + 7);
   assert(strncmp(dstr_cstr(&s), "prefix:", 7) == 0);
   dstr_free(&s);
   PASS("appendf_large");
}

static void test_equals_cstr(void)
{
   dstr_t s;
   dstr_init(&s);
   assert(dstr_equals_cstr(&s, ""));
   assert(dstr_equals_cstr(&s, NULL));
   assert(!dstr_equals_cstr(&s, "x"));
   dstr_append_str(&s, "hello");
   assert(dstr_equals_cstr(&s, "hello"));
   assert(!dstr_equals_cstr(&s, "hell"));
   assert(!dstr_equals_cstr(&s, "hello!"));
   assert(!dstr_equals_cstr(&s, NULL));
   dstr_free(&s);
   PASS("equals_cstr");
}

static void test_read_file_large(void)
{
   /* Write a file larger than any fixed buffer we previously used (>20K) and
    * confirm dstr_read_file returns the full contents. */
   char tmp[256];
   snprintf(tmp, sizeof tmp, "%s/dstr_test_XXXXXX", platform_tmpdir());
   int fd = mkstemp(tmp);
   assert(fd >= 0);
   const size_t N = 40000;
   char *payload = malloc(N);
   assert(payload);
   for (size_t i = 0; i < N; i++)
      payload[i] = (char)('a' + (i % 26));
   ssize_t w = write(fd, payload, N);
   assert(w == (ssize_t)N);
   close(fd);

   dstr_t s;
   dstr_init(&s);
   assert(dstr_read_file(&s, tmp) == 0);
   assert(dstr_len(&s) == N);
   assert(memcmp(s.data, payload, N) == 0);
   /* Round-trip via equals_cstr: build a NUL-terminated copy */
   char *cp = malloc(N + 1);
   memcpy(cp, payload, N);
   cp[N] = '\0';
   assert(dstr_equals_cstr(&s, cp));
   free(cp);
   free(payload);
   dstr_free(&s);
   unlink(tmp);
   PASS("read_file_large");
}

static void test_read_file_missing(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append_str(&s, "prior");
   assert(dstr_read_file(&s, "/tmp/dstr_does_not_exist_xyz_12345") == -1);
   dstr_free(&s);
   PASS("read_file_missing");
}

static void test_read_file_reset(void)
{
   /* Existing contents should be cleared on successful read. */
   char tmp[256];
   snprintf(tmp, sizeof tmp, "%s/dstr_test_XXXXXX", platform_tmpdir());
   int fd = mkstemp(tmp);
   assert(fd >= 0);
   ssize_t w = write(fd, "fresh", 5);
   assert(w == 5);
   close(fd);

   dstr_t s;
   dstr_init(&s);
   dstr_append_str(&s, "stale content that should disappear");
   assert(dstr_read_file(&s, tmp) == 0);
   assert(dstr_equals_cstr(&s, "fresh"));
   dstr_free(&s);
   unlink(tmp);
   PASS("read_file_reset");
}

int main(void)
{
   printf("dstr:\n");
   test_init_free();
   test_append_str();
   test_append_char();
   test_append_raw();
   test_appendf();
   test_reset();
   test_steal();
   test_reserve();
   test_large_append();
   test_appendf_large();
   test_equals_cstr();
   test_read_file_large();
   test_read_file_missing();
   test_read_file_reset();
   printf("all dstr tests passed\n");
   return 0;
}
