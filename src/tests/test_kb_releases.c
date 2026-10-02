/* test_kb_releases.c: unit tests for review/release lifecycle HTTP handlers. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "kb_http_releases.h"

/* ── kb_store_kb_release_t (mirrors kb_store/kb_releases.h) ──────────────────────────── */

typedef struct
{
   int64_t id;
   char name[128];
   char state[32];
   char promoted_at[32];
   char retired_at[32];
   char created_at[32];
} kb_store_kb_release_t;

/* ── KB_STORE stubs ───────────────────────────────────────────────────────────────── */

int kb_store_kb_doc_set_state(int64_t id, const char *state, int clear_review_needed,
                              const char *review_reason)
{
   (void)state;
   (void)clear_review_needed;
   (void)review_reason;
   return (id > 0) ? 0 : -1;
}

int64_t kb_store_kb_release_create(const char *name)
{
   (void)name;
   return 7;
}

int kb_store_kb_release_promote(int64_t id)
{
   return (id > 0) ? 0 : -1;
}

int kb_store_kb_release_rollback(int64_t target_id)
{
   (void)target_id;
   return 0;
}

int64_t kb_store_kb_release_get_active(void)
{
   return 0;
}

int kb_store_kb_release_read(int64_t id, kb_store_kb_release_t *out)
{
   if (id <= 0)
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = id;
   snprintf(out->name, sizeof(out->name), "v1.0");
   snprintf(out->state, sizeof(out->state), "active");
   snprintf(out->promoted_at, sizeof(out->promoted_at), "2026-01-01 00:00:00");
   return 0;
}

int kb_store_kb_release_add_doc(int64_t release_id, int64_t doc_id)
{
   (void)release_id;
   (void)doc_id;
   return 0;
}

/* ── Tests ───────────────────────────────────────────────────────────────────── */

static void test_review_accept_ok(void)
{
   char buf[256];
   int status = handle_post_review_accept("42", NULL, 0, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "accepted") != NULL);
}

static void test_review_accept_bad_id(void)
{
   char buf[256];
   int status = handle_post_review_accept("0", NULL, 0, buf, sizeof(buf));
   assert(status == 400);
}

static void test_review_reject_ok(void)
{
   char buf[256];
   const char *body = "{\"reason\":\"low quality\"}";
   int body_len = (int)strlen(body);
   int status = handle_post_review_reject("42", body, body_len, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "rejected") != NULL);
}

static void test_post_releases_ok(void)
{
   char buf[256];
   const char *body = "{\"name\":\"v1.0\"}";
   int body_len = (int)strlen(body);
   int status = handle_post_releases(body, body_len, buf, sizeof(buf));
   assert(status == 201);
   assert(strstr(buf, "release_id") != NULL);
}

static void test_post_releases_missing_name(void)
{
   char buf[256];
   int status = handle_post_releases("{}", 2, buf, sizeof(buf));
   assert(status == 400);
}

static void test_promote_ok(void)
{
   char buf[256];
   int status = handle_post_promote("7", buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "active") != NULL);
}

static void test_rollback_ok(void)
{
   char buf[256];
   int status = handle_post_rollback("7", NULL, 0, buf, sizeof(buf));
   assert(status == 200);
}

static void test_get_active_no_release(void)
{
   char buf[256];
   int status = handle_get_active_release(buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "null") != NULL);
}

int main(void)
{
   printf("kb_releases: ");

   test_review_accept_ok();
   test_review_accept_bad_id();
   test_review_reject_ok();
   test_post_releases_ok();
   test_post_releases_missing_name();
   test_promote_ok();
   test_rollback_ok();
   test_get_active_no_release();

   printf("ok\n");
   return 0;
}
