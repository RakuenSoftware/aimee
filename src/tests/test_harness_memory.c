/* Unit tests for harness_memory_common (sha256 + content-hash + project-key
 * resolve/validate) and the DB1 per-user memory store + recall merge
 * (Proposal 2). The legacy harness_memory .md-mirror table was retired. */

#include "db1/db1.h"
#include "db1/user_memory.h"
#include "harness_memory_common.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_sha(void)
{
   /* FIPS 180-4 known-answer vectors, incl. length-mod-64 boundaries. */
   char h[65];
   hmem_sha256_hex("", 0, h);
   assert(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
   hmem_sha256_hex("abc", 3, h);
   assert(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
   char a[120];
   memset(a, 'a', sizeof(a));
   hmem_sha256_hex(a, 55, h); /* rem==55 (one pad block) */
   assert(strcmp(h, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318") == 0);
   hmem_sha256_hex(a, 56, h); /* rem==56 (forces a second pad block) */
   assert(strcmp(h, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a") == 0);
   hmem_sha256_hex(a, 64, h); /* exact block boundary */
   assert(strcmp(h, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb") == 0);
   hmem_sha256_hex(a, 119, h); /* multi-block */
   assert(strcmp(h, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb") == 0);
}

static void test_hash(void)
{
   char a[65], b[65], c[65];
   assert(hmem_content_hash("fact", "n", "desc", "body", "{}", a) == 0);
   assert(strlen(a) == 64);

   /* NULL description == "" description */
   assert(hmem_content_hash("fact", "n", NULL, "body", "{}", b) == 0);
   assert(hmem_content_hash("fact", "n", "", "body", "{}", c) == 0);
   assert(strcmp(b, c) == 0);

   /* meta canonicalization: key order irrelevant; null/empty values omitted */
   char m1[65], m2[65];
   assert(hmem_content_hash("fact", "n", "d", "b", "{\"a\":1,\"b\":2}", m1) == 0);
   assert(hmem_content_hash("fact", "n", "d", "b", "{\"b\":2,\"a\":1,\"z\":null,\"e\":\"\"}", m2) ==
          0);
   assert(strcmp(m1, m2) == 0);

   /* different body => different hash */
   char x[65];
   assert(hmem_content_hash("fact", "n", "d", "other", "{}", x) == 0);
   assert(strcmp(m1, x) != 0);
}

static void test_resolve(void)
{
   char id[256], root[1024];
   /* $AIMEE_PROJECT_ID (opaque, non-path) becomes the id; root stays a real path */
   setenv("AIMEE_PROJECT_ID", "proj-xyz", 1);
   assert(hmem_resolve_project(".", id, sizeof(id), root, sizeof(root)) == 0);
   assert(strcmp(id, "proj-xyz") == 0);
   assert(root[0] == '/');
   unsetenv("AIMEE_PROJECT_ID");
   /* no env: id == resolved root */
   char id2[256], root2[1024];
   assert(hmem_resolve_project(".", id2, sizeof(id2), root2, sizeof(root2)) == 0);
   assert(strcmp(id2, root2) == 0);
}

static void test_project_key_ok(void)
{
   /* path-shaped keys (the common case) and opaque ids are accepted */
   assert(hmem_project_key_ok("/home/u/dev/aimee") == 1);
   assert(hmem_project_key_ok("proj-xyz") == 1);
   /* empty / NULL rejected */
   assert(hmem_project_key_ok(NULL) == 0);
   assert(hmem_project_key_ok("") == 0);
   /* control chars / newlines rejected (would corrupt audit/JSON lines) */
   assert(hmem_project_key_ok("a\nb") == 0);
   assert(hmem_project_key_ok("a\tb") == 0);
   assert(hmem_project_key_ok("a\x7f"
                              "b") == 0);
   /* length: 255 ok, 256 rejected (store buffer is char[256]) */
   char big[300];
   memset(big, 'x', sizeof(big));
   big[255] = '\0'; /* 255 chars */
   assert(hmem_project_key_ok(big) == 1);
   big[255] = 'x';
   big[256] = '\0'; /* 256 chars */
   assert(hmem_project_key_ok(big) == 0);
}

/* Proposal 2 Phase 1: db1 user-memory store + recall selectors. */
static void test_user_memory(void)
{
   /* db1 already init'd (:memory:) by main. */
   assert(db1_user_memory_upsert("fact", "L2", "identity:operator", "JBailes is the operator", 1.0,
                                 "t") == 0);
   assert(db1_user_memory_upsert("preference", "L2", "pref:no-attr", "No Claude attribution", 1.0,
                                 "t") == 0);
   /* Tier gate: an L1 identity fact must NOT surface (recall requires L2+). */
   assert(db1_user_memory_upsert("fact", "L1", "identity:ignored", "low tier", 1.0, "t") == 0);
   /* Key-prefix gate: an L2 fact without an identity-ish key must NOT surface. */
   assert(db1_user_memory_upsert("fact", "L2", "misc:thing", "not identity", 1.0, "t") == 0);

   db1_user_memory_row_t rows[16];
   int n = db1_user_memory_list_recall(DB1_USER_RECALL_IDENTITY, rows, 16);
   assert(n == 1); /* only identity:operator (L2 + identity: prefix) */
   assert(strcmp(rows[0].key, "identity:operator") == 0);

   int np = db1_user_memory_list_recall(DB1_USER_RECALL_PREFERENCES, rows, 16);
   assert(np == 1);
   assert(strcmp(rows[0].key, "pref:no-attr") == 0);

   /* Upsert idempotency: same (kind,key) updates in place, no duplicate row. */
   assert(db1_user_memory_upsert("preference", "L2", "pref:no-attr", "updated", 1.0, "t") == 0);
   np = db1_user_memory_list_recall(DB1_USER_RECALL_PREFERENCES, rows, 16);
   assert(np == 1);
   assert(strcmp(rows[0].content, "updated") == 0);

   /* Merge into a synthetic org (db2/kb) recall array: db1 wins on key
    * collision (org dup removed, db1 row first), org-only rows survive. */
   cJSON *arr = cJSON_CreateArray();
   cJSON *org1 = cJSON_CreateObject();
   cJSON_AddStringToObject(org1, "key", "identity:operator"); /* collides with db1 */
   cJSON_AddStringToObject(org1, "text", "stale org version");
   cJSON_AddStringToObject(org1, "scope", "org");
   cJSON_AddItemToArray(arr, org1);
   cJSON *org2 = cJSON_CreateObject();
   cJSON_AddStringToObject(org2, "key", "identity:orgonly");
   cJSON_AddStringToObject(org2, "scope", "org");
   cJSON_AddItemToArray(arr, org2);

   db1_user_memory_merge_into_array(arr, DB1_USER_RECALL_IDENTITY, "user identity");

   assert(cJSON_GetArraySize(arr) == 2); /* db1 operator + org-only; org dup replaced */
   cJSON *first = cJSON_GetArrayItem(arr, 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(first, "key")), "identity:operator") ==
          0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(first, "scope")), "user") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(first, "text")),
                 "JBailes is the operator") == 0); /* db1 content, not the stale org one */
   int found_orgonly = 0;
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, arr)
   {
      const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(it, "key"));
      if (k && strcmp(k, "identity:orgonly") == 0)
         found_orgonly = 1;
   }
   assert(found_orgonly); /* org-only row survives the merge */
   cJSON_Delete(arr);

   /* Preferences section merges by the SAME db1-wins rule (production merges
    * both identity AND preferences — kb_client_memory_recall_json_ex). A db1
    * preference overrides a same-key soft org default (R2 lattice: user capture
    * outranks a soft org default), and an org-only preference survives. */
   cJSON *parr = cJSON_CreateArray();
   cJSON *porg1 = cJSON_CreateObject();
   cJSON_AddStringToObject(porg1, "key", "pref:no-attr"); /* collides with db1 */
   cJSON_AddStringToObject(porg1, "text", "org default: attribution allowed");
   cJSON_AddStringToObject(porg1, "scope", "org");
   cJSON_AddItemToArray(parr, porg1);
   cJSON *porg2 = cJSON_CreateObject();
   cJSON_AddStringToObject(porg2, "key", "pref:org-style");
   cJSON_AddStringToObject(porg2, "scope", "org");
   cJSON_AddItemToArray(parr, porg2);

   db1_user_memory_merge_into_array(parr, DB1_USER_RECALL_PREFERENCES, "user preference");

   assert(cJSON_GetArraySize(parr) == 2); /* db1 pref + org-only; org dup replaced */
   cJSON *pfirst = cJSON_GetArrayItem(parr, 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(pfirst, "key")), "pref:no-attr") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(pfirst, "scope")), "user") == 0);
   /* db1 preference wins over the conflicting soft org default. */
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(pfirst, "text")), "updated") == 0);
   int found_orgstyle = 0;
   cJSON *pit = NULL;
   cJSON_ArrayForEach(pit, parr)
   {
      const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(pit, "key"));
      if (k && strcmp(k, "pref:org-style") == 0)
         found_orgstyle = 1;
   }
   assert(found_orgstyle); /* org-only preference survives the merge */
   cJSON_Delete(parr);

   printf("test_user_memory: PASS\n");
}

int main(void)
{
   test_sha();
   test_hash();
   test_resolve();
   test_project_key_ok();
   assert(db1_init(":memory:") == 0);
   test_user_memory();
   db1_shutdown();
   printf("test_harness_memory: OK\n");
   return 0;
}
