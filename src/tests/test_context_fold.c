/* test_context_fold.c: unit tests for the rolling fold (§1, P2a). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../headers/context_fold.h"

#define PASS(name) printf("  PASS: %s\n", name)

static int contains(const char *hay, const char *needle)
{
   return hay && needle && strstr(hay, needle) != NULL;
}

static void add_user_text(cJSON *msgs, const char *text)
{
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "user");
   cJSON_AddStringToObject(m, "content", text);
   cJSON_AddItemToArray(msgs, m);
}

static void add_assistant_text(cJSON *msgs, const char *text)
{
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "assistant");
   cJSON_AddStringToObject(m, "content", text);
   cJSON_AddItemToArray(msgs, m);
}

static void add_assistant_tool_use(cJSON *msgs, const char *id, const char *name, const char *arg)
{
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "assistant");
   cJSON *content = cJSON_AddArrayToObject(m, "content");
   cJSON *b = cJSON_CreateObject();
   cJSON_AddStringToObject(b, "type", "tool_use");
   cJSON_AddStringToObject(b, "id", id);
   cJSON_AddStringToObject(b, "name", name);
   cJSON *input = cJSON_AddObjectToObject(b, "input");
   cJSON_AddStringToObject(input, "arg", arg);
   cJSON_AddItemToArray(content, b);
   cJSON_AddItemToArray(msgs, m);
}

static void add_user_tool_result(cJSON *msgs, const char *id, const char *result)
{
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "user");
   cJSON *content = cJSON_AddArrayToObject(m, "content");
   cJSON *b = cJSON_CreateObject();
   cJSON_AddStringToObject(b, "type", "tool_result");
   cJSON_AddStringToObject(b, "tool_use_id", id);
   cJSON_AddStringToObject(b, "content", result);
   cJSON_AddItemToArray(content, b);
   cJSON_AddItemToArray(msgs, m);
}

/* 14-message conversation with tool_use/tool_result pairs. */
static cJSON *build_convo(void)
{
   cJSON *m = cJSON_CreateArray();
   add_user_text(m, "start job 7fd5835b-1a2b-4c3d-8e9f-0123456789ab"); /* 0 */
   add_assistant_tool_use(m, "toolu_1", "read", "/etc/hosts");         /* 1 */
   add_user_tool_result(m, "toolu_1", "ok; bound on port=3002");       /* 2 */
   add_assistant_text(m, "read complete");                             /* 3 */
   add_user_text(m, "next please");                                    /* 4 (clean) */
   add_assistant_tool_use(m, "toolu_2", "bash", "ls -la");             /* 5 */
   add_user_tool_result(m, "toolu_2", "file listing here");            /* 6 */
   add_assistant_text(m, "listing done");                              /* 7 */
   add_user_text(m, "keep going");                                     /* 8 */
   add_assistant_tool_use(m, "toolu_3", "grep", "needle");             /* 9 */
   add_user_tool_result(m, "toolu_3", "3 hits");                       /* 10 */
   add_assistant_text(m, "found them");                                /* 11 */
   add_user_text(m, "almost there");                                   /* 12 */
   add_assistant_text(m, "done");                                      /* 13 */
   return m;
}

static int msg_is_clean_user(const cJSON *m)
{
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)m, "role"));
   if (!role || strcmp(role, "user") != 0)
      return 0;
   cJSON *c = cJSON_GetObjectItem((cJSON *)m, "content");
   if (cJSON_IsString(c))
      return 1;
   cJSON *b;
   cJSON_ArrayForEach(b, c)
   {
      const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
      if (t && strcmp(t, "tool_result") == 0)
         return 0;
   }
   return 1;
}

static void test_basic_fold(void)
{
   cJSON *msgs = build_convo();
   int orig_count = cJSON_GetArraySize(msgs);
   fold_config_t cfg = {.enabled = 1, .closet = {.enabled = 1}};
   fold_result_t r;
   assert(context_fold_view(msgs, &cfg, NULL, &r) == 0);
   assert(r.folded == 1);
   assert(r.messages != NULL);

   /* desired split = 14 - 8 = 6 (a tool_result) -> backs off to the clean user
    * turn at index 4; folds 4 messages. */
   assert(r.folded_msgs == 4);

   cJSON *m0 = cJSON_GetArrayItem(r.messages, 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "role")), "user") == 0);
   const char *c0 = cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content"));
   assert(contains(c0, "[folded 4 earlier"));
   assert(contains(c0, "Coordinate Closet"));
   /* identifiers from the folded region are conserved verbatim */
   assert(contains(c0, "7fd5835b-1a2b-4c3d-8e9f-0123456789ab"));
   assert(contains(c0, "3002"));
   /* skeleton mentions the folded tool calls */
   assert(contains(c0, "read") && contains(c0, "$"));

   cJSON *m1 = cJSON_GetArrayItem(r.messages, 1);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(m1, "role")), "assistant") == 0);

   /* retained tail begins at original index 4 ("next please") — a clean user turn,
    * proving no tool pair was straddled. */
   cJSON *m2 = cJSON_GetArrayItem(r.messages, 2);
   assert(msg_is_clean_user(m2));
   assert(contains(cJSON_GetStringValue(cJSON_GetObjectItem(m2, "content")), "next please"));

   /* synthetic count = 2 synthetic + (14 - 4) retained */
   assert(cJSON_GetArraySize(r.messages) == 2 + (orig_count - 4));

   /* original array is untouched */
   assert(cJSON_GetArraySize(msgs) == orig_count);

   fold_result_free(&r);
   cJSON_Delete(msgs);
   PASS("basic_fold");
}

static void test_nondestructive_and_deterministic(void)
{
   cJSON *a = build_convo();
   cJSON *b = build_convo();
   fold_config_t cfg = {.enabled = 1, .closet = {.enabled = 1}};
   fold_result_t ra, rb;
   assert(context_fold_view(a, &cfg, NULL, &ra) == 0 && ra.folded);
   assert(context_fold_view(b, &cfg, NULL, &rb) == 0 && rb.folded);
   char *sa = cJSON_PrintUnformatted(ra.messages);
   char *sb = cJSON_PrintUnformatted(rb.messages);
   assert(sa && sb && strcmp(sa, sb) == 0); /* identical input -> identical fold */
   free(sa);
   free(sb);
   fold_result_free(&ra);
   fold_result_free(&rb);
   cJSON_Delete(a);
   cJSON_Delete(b);
   PASS("nondestructive_and_deterministic");
}

static void test_too_short_no_fold(void)
{
   cJSON *m = cJSON_CreateArray();
   add_user_text(m, "hello");
   add_assistant_text(m, "hi");
   fold_config_t cfg = {.enabled = 1, .closet = {.enabled = 1}};
   fold_result_t r;
   assert(context_fold_view(m, &cfg, NULL, &r) == 0);
   assert(r.folded == 0 && r.messages == NULL);
   fold_result_free(&r);
   cJSON_Delete(m);
   PASS("too_short_no_fold");
}

static void test_disabled_no_fold(void)
{
   cJSON *m = build_convo();
   fold_config_t off = {.enabled = 0};
   fold_result_t r;
   assert(context_fold_view(m, &off, NULL, &r) == 0);
   assert(r.folded == 0 && r.messages == NULL);
   fold_result_free(&r);
   cJSON_Delete(m);
   PASS("disabled_no_fold");
}

static void test_small_retained_band(void)
{
   /* retained=2, min_fold=4: desired split = 12, walk down to a clean user turn. */
   cJSON *m = build_convo();
   fold_config_t cfg = {
       .enabled = 1, .retained_msgs = 2, .min_fold_msgs = 4, .closet = {.enabled = 1}};
   fold_result_t r;
   assert(context_fold_view(m, &cfg, NULL, &r) == 0);
   assert(r.folded == 1);
   /* retained tail still begins with a clean user turn */
   assert(msg_is_clean_user(cJSON_GetArrayItem(r.messages, 2)));
   fold_result_free(&r);
   cJSON_Delete(m);
   PASS("small_retained_band");
}

static void test_retained_ge_count(void)
{
   cJSON *m = build_convo(); /* 14 messages */
   fold_config_t cfg = {.enabled = 1, .retained_msgs = 100, .closet = {.enabled = 1}};
   fold_result_t r;
   assert(context_fold_view(m, &cfg, NULL, &r) == 0);
   assert(r.folded == 0 && r.messages == NULL); /* nothing to fold below the band */
   fold_result_free(&r);
   cJSON_Delete(m);
   PASS("retained_ge_count");
}

static void test_odd_shapes_no_crash(void)
{
   /* a message with no "content" key, and a tool_result whose content is an
    * array of blocks — must fold without crashing and still conserve ids. */
   cJSON *m = cJSON_CreateArray();
   add_user_text(m, "begin path /home/u/app.c"); /* 0 */
   cJSON *noc = cJSON_CreateObject();            /* 1: missing content */
   cJSON_AddStringToObject(noc, "role", "assistant");
   cJSON_AddItemToArray(m, noc);
   /* 2: tool_result with array content */
   cJSON *tr = cJSON_CreateObject();
   cJSON_AddStringToObject(tr, "role", "user");
   cJSON *content = cJSON_AddArrayToObject(tr, "content");
   cJSON *blk = cJSON_CreateObject();
   cJSON_AddStringToObject(blk, "type", "tool_result");
   cJSON_AddStringToObject(blk, "tool_use_id", "toolu_9");
   cJSON *inner = cJSON_AddArrayToObject(blk, "content");
   cJSON *part = cJSON_CreateObject();
   cJSON_AddStringToObject(part, "type", "text");
   cJSON_AddStringToObject(part, "text", "deadbeefcafe1234 on port=7700");
   cJSON_AddItemToArray(inner, part);
   cJSON_AddItemToArray(content, blk);
   cJSON_AddItemToArray(m, tr);
   add_assistant_text(m, "noted"); /* 3 */
   add_user_text(m, "go on");      /* 4 clean */
   add_assistant_text(m, "a");     /* 5 */
   add_user_text(m, "b");          /* 6 */
   add_assistant_text(m, "c");     /* 7 */
   add_user_text(m, "d");          /* 8 */
   add_assistant_text(m, "e");     /* 9 */

   fold_config_t cfg = {
       .enabled = 1, .retained_msgs = 4, .min_fold_msgs = 4, .closet = {.enabled = 1}};
   fold_result_t r;
   assert(context_fold_view(m, &cfg, NULL, &r) == 0);
   assert(r.folded == 1 && r.messages != NULL);
   const char *c0 =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(r.messages, 0), "content"));
   assert(contains(c0, "deadbeefcafe1234")); /* id from array-content tool_result conserved */
   assert(contains(c0, "/home/u/app.c"));
   fold_result_free(&r);
   cJSON_Delete(m);
   PASS("odd_shapes_no_crash");
}

/* Append a user-text + assistant-text round to simulate a new turn. */
static void add_round(cJSON *m, const char *u, const char *a)
{
   add_user_text(m, u);
   add_assistant_text(m, a);
}

static void test_freeze_boundary_stable(void)
{
   cJSON *m = build_convo(); /* 14 msgs */
   fold_config_t cfg = {.enabled = 1, .closet = {.enabled = 1}};
   fold_freeze_t fz;
   memset(&fz, 0, sizeof(fz));
   fz.tail_cap_msgs = 12;

   /* turn 1: first fold pins the boundary */
   fold_result_t r1;
   assert(context_fold_view(m, &cfg, &fz, &r1) == 0 && r1.folded);
   assert(fz.active && r1.reused_boundary == 0);
   int pinned = fz.frozen_split;
   char *prefix1 =
       cJSON_PrintUnformatted(cJSON_GetArrayItem(r1.messages, 0)); /* synthetic fold msg */

   /* turn 2: append a round; prefix unchanged + tail within cap -> reuse boundary,
    * byte-identical synthetic prefix (warm cache) */
   add_round(m, "another request", "another answer");
   fold_result_t r2;
   assert(context_fold_view(m, &cfg, &fz, &r2) == 0 && r2.folded);
   assert(r2.reused_boundary == 1);
   assert(fz.frozen_split == pinned);
   char *prefix2 = cJSON_PrintUnformatted(cJSON_GetArrayItem(r2.messages, 0));
   assert(strcmp(prefix1, prefix2) == 0); /* byte-identical folded prefix */

   /* keep appending until the tail exceeds the cap -> epoch advance */
   int epochs_before = fz.epochs;
   for (int i = 0; i < 12; i++)
      add_round(m, "more", "ok");
   fold_result_t r3;
   assert(context_fold_view(m, &cfg, &fz, &r3) == 0 && r3.folded);
   assert(r3.reused_boundary == 0);        /* re-epoched */
   assert(fz.epochs == epochs_before + 1); /* advanced once */
   assert(fz.frozen_split > pinned);       /* boundary moved forward */

   free(prefix1);
   free(prefix2);
   fold_result_free(&r1);
   fold_result_free(&r2);
   fold_result_free(&r3);
   cJSON_Delete(m);
   PASS("freeze_boundary_stable");
}

static void test_freeze_prefix_mutation_breaks_reuse(void)
{
   /* If the folded prefix is mutated mid-run (as compaction could), the digest
    * check must force an epoch instead of a false byte-identical "reuse". */
   cJSON *m = build_convo();
   fold_config_t cfg = {.enabled = 1, .closet = {.enabled = 1}};
   fold_freeze_t fz;
   memset(&fz, 0, sizeof(fz));
   fz.tail_cap_msgs = 12;

   fold_result_t r1;
   assert(context_fold_view(m, &cfg, &fz, &r1) == 0 && r1.folded);
   int epochs_before = fz.epochs;

   /* mutate a message inside the folded prefix (index 0) */
   cJSON *folded0 = cJSON_GetArrayItem(m, 0);
   cJSON_ReplaceItemInObjectCaseSensitive(folded0, "content",
                                          cJSON_CreateString("MUTATED prefix content"));
   add_round(m, "carry on", "sure");

   fold_result_t r2;
   assert(context_fold_view(m, &cfg, &fz, &r2) == 0 && r2.folded);
   assert(r2.reused_boundary == 0);        /* mutation forced an epoch, not a reuse */
   assert(fz.epochs == epochs_before + 1); /* genuine advance */

   fold_result_free(&r1);
   fold_result_free(&r2);
   cJSON_Delete(m);
   PASS("freeze_prefix_mutation_breaks_reuse");
}

int main(void)
{
   printf("context_fold tests:\n");
   test_basic_fold();
   test_nondestructive_and_deterministic();
   test_too_short_no_fold();
   test_disabled_no_fold();
   test_small_retained_band();
   test_retained_ge_count();
   test_odd_shapes_no_crash();
   test_freeze_boundary_stable();
   test_freeze_prefix_mutation_breaks_reuse();
   printf("ALL PASS\n");
   return 0;
}
