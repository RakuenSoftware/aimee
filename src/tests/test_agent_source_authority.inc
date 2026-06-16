#include "agent_source_authority.h"
#include <pthread.h>

/* Regression test for the concurrent-delegate source-authority race: each
 * delegate installs its source context via agent_source_authority_tls_set
 * (thread-local), and the code_search overlay must read THIS thread's context,
 * never a neighbor's. Pre-fix the context lived in process-global env vars, so
 * 8 threads each setting a different worktree/paths clobbered each other and the
 * overlay resolved against the wrong tree (flakiness that varied with timing).
 *
 * Two thread groups (A/B) each own a distinct file whose unique symbol exists
 * ONLY in their file. Each thread, looping, sets its TLS paths and asserts the
 * overlay finds ITS symbol (>=1 hit) and NOT the other group's symbol (0 hits).
 * Any cross-thread TLS leak flips one of those assertions. */
typedef struct
{
   const char *path;      /* this group's source file */
   const char *own_sym;   /* symbol present only in `path` */
   const char *other_sym; /* the other group's symbol, absent from `path` */
   const char *root;
   int iters;
   int failed;
} sa_tls_worker_arg_t;

static void *sa_tls_worker(void *argp)
{
   sa_tls_worker_arg_t *a = (sa_tls_worker_arg_t *)argp;
   for (int i = 0; i < a->iters; i++)
   {
      agent_source_authority_tls_set(1, a->root, a->path);

      cJSON *own = cJSON_CreateArray();
      int own_hits = agent_source_append_overlay_code_hits(own, a->own_sym, NULL, 5);
      cJSON *other = cJSON_CreateArray();
      int other_hits = agent_source_append_overlay_code_hits(other, a->other_sym, NULL, 5);

      if (own_hits < 1 || other_hits != 0)
         a->failed = 1; /* TLS leaked from another thread */
      cJSON_Delete(own);
      cJSON_Delete(other);
   }
   /* Release the heap-held paths for this thread (zeroed snapshot frees + clears). */
   agent_source_authority_snapshot_t z;
   memset(&z, 0, sizeof(z));
   agent_source_authority_tls_restore(&z);
   return NULL;
}

static void test_source_authority_tls_thread_isolation(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-sa-tls-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char path_a[768], path_b[768];
   snprintf(path_a, sizeof(path_a), "%s/group_a.c", tmpdir);
   snprintf(path_b, sizeof(path_b), "%s/group_b.c", tmpdir);
   FILE *fa = fopen(path_a, "w");
   assert(fa);
   fputs("int alpha_unique_marker(void) { return 1; }\n", fa);
   fclose(fa);
   FILE *fb = fopen(path_b, "w");
   assert(fb);
   fputs("int beta_unique_marker(void) { return 2; }\n", fb);
   fclose(fb);

   enum
   {
      NW = 8,
      ITERS = 150
   };
   pthread_t th[NW];
   sa_tls_worker_arg_t args[NW];
   for (int i = 0; i < NW; i++)
   {
      int is_a = (i % 2 == 0);
      args[i].path = is_a ? path_a : path_b;
      args[i].own_sym = is_a ? "alpha_unique_marker" : "beta_unique_marker";
      args[i].other_sym = is_a ? "beta_unique_marker" : "alpha_unique_marker";
      args[i].root = tmpdir;
      args[i].iters = ITERS;
      args[i].failed = 0;
      assert(pthread_create(&th[i], NULL, sa_tls_worker, &args[i]) == 0);
   }
   for (int i = 0; i < NW; i++)
   {
      pthread_join(th[i], NULL);
      assert(args[i].failed == 0); /* no cross-thread source-authority leak */
   }

   unlink(path_a);
   unlink(path_b);
   rmdir(tmpdir);
}

static void test_source_authority_overlay_tools(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-source-overlay-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char path[768];
   snprintf(path, sizeof(path), "%s/current_source.c", tmpdir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("int overlay_unique_symbol(void) { return 42; }\n", f);
   fclose(f);

   assert(platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", "1") == 0);
   assert(platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", path) == 0);
   assert(platform_setenv("AIMEE_DELEGATE_WORKTREE_ROOT", tmpdir) == 0);

   char *result = tool_code_search("overlay_unique_symbol", NULL, 5);
   assert(result != NULL);
   cJSON *arr = parse_json_or_die(result);
   assert(cJSON_IsArray(arr));
   cJSON *first = cJSON_GetArrayItem(arr, 0);
   assert(first != NULL);
   assert(strcmp(cJSON_GetObjectItem(first, "project")->valuestring, "current_overlay") == 0);
   assert(strcmp(cJSON_GetObjectItem(first, "freshness")->valuestring, "source_packet_current") ==
          0);
   assert(strcmp(cJSON_GetObjectItem(first, "authority")->valuestring, "current_source") == 0);
   cJSON_Delete(arr);
   free(result);

   result = tool_find_symbol("overlay_unique_symbol");
   assert(result != NULL);
   assert(strstr(result, "source_packet_current") != NULL);
   assert(strstr(result, "authority=current_source") != NULL);
   free(result);

   assert(platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", "") == 0);
   assert(platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", "") == 0);
   assert(platform_setenv("AIMEE_DELEGATE_WORKTREE_ROOT", "") == 0);
   unlink(path);
   rmdir(tmpdir);
}
