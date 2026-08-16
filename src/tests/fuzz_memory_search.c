/* fuzz_memory_search.c: fuzz harness for memory search functions
 *
 * Creates an in-memory database with seeded memories and exercises
 * memory_find_facts with fuzzed query strings.
 *
 * Build:
 *   libFuzzer: clang -fsanitize=fuzzer,address -o fuzz_memory_search ...
 *   Standalone: gcc -DFUZZ_STANDALONE -o fuzz_memory_search ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"

static int g_db_ready = 0;

static void ensure_db(void)
{
   if (g_db_ready)
      return;
   if (db1_init(":memory:") != 0)
      return;
   db2_test_shim_open();
   g_db_ready = 1;

   /* Seed with representative memories */
   const char *facts[] = {
       "The server listens on port 9200 by default",
       "Memory tiers are L0 scratch, L1 session, L2 long-term, L3 failure",
       "Config files are stored in ~/.config/aimee/",
       "The agent network uses JSON-RPC over HTTP",
       "DB1 uses SQLite for local server-owned state",
       "Guardrails classify paths by sensitivity level",
       "Delegation dispatches work to sub-agents",
       "The dashboard provides real-time metrics",
   };
   int n = (int)(sizeof(facts) / sizeof(facts[0]));
   for (int i = 0; i < n; i++)
   {
      memory_t m;
      char key[64];
      snprintf(key, sizeof(key), "fact_%d", i);
      memory_insert(TIER_L2, KIND_FACT, key, facts[i], 0.8, "fuzz", &m);
   }
}

static void fuzz_one(const char *data, size_t size)
{
   ensure_db();
   if (!g_db_ready)
      return;

   /* Null-terminate query */
   char *query = malloc(size + 1);
   if (!query)
      return;
   memcpy(query, data, size);
   query[size] = '\0';

   /* Exercise FTS5 search */
   memory_t results[32];
   memory_find_facts(query, 10, results, 32);

   free(query);
}

#ifndef FUZZ_STANDALONE
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
   if (size == 0 || size > 4096)
      return 0;
   fuzz_one((const char *)data, size);
   return 0;
}
#else
static int fuzz_file(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
   {
      fprintf(stderr, "Cannot open: %s\n", path);
      return 1;
   }

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 10 * 1024 * 1024)
   {
      fclose(f);
      return 0;
   }

   char *data = malloc((size_t)sz);
   if (!data)
   {
      fclose(f);
      return 1;
   }
   size_t nread = fread(data, 1, (size_t)sz, f);
   fclose(f);

   fuzz_one(data, nread);
   free(data);
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      char buf[4096];
      size_t n = fread(buf, 1, sizeof(buf), stdin);
      fuzz_one(buf, n);
   }
   else
   {
      for (int i = 1; i < argc; i++)
      {
         if (fuzz_file(argv[i]) != 0)
            return 1;
      }
   }
   printf("fuzz_memory_search: %d inputs ok\n", argc > 1 ? argc - 1 : 1);

   /* Cleanup */
   if (g_db_ready)
   {
      db1_stmt_cache_clear();
      db2_test_shim_close();
      db1_shutdown();
      g_db_ready = 0;
   }
   return 0;
}
#endif
