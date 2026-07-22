/* cochange.c: pure co-change pairing policy.
 *
 * Given the code-file basenames touched by one git commit, produce the
 * canonical unordered file pairs whose co_edited edge weight the git-history
 * backfill (index.c) accumulates. Kept free of git and DB I/O so the pairing
 * rules — dedup, the bulk-commit gate, canonical ordering — are unit-testable
 * on their own. */
#include "aimee.h"
#include "index.h"

static int cochange_name_cmp(const void *a, const void *b)
{
   /* Dedicated wrapper (not a strcmp cast) so -fsanitize=function stays clean. */
   return strcmp((const char *)a, (const char *)b);
}

int cochange_is_hex_sha(const char *s)
{
   /* A git object id: lowercase hex, 4..64 chars. The co-change backfill stores
    * the synced HEAD in kb_runtime_state, which is operator-editable, and later
    * interpolates it into a `git` command — so a value that is not a plain object
    * id must never reach the shell. Reject everything else (empty, mixed case,
    * shell metacharacters). */
   if (!s || !s[0])
      return 0;
   size_t n = 0;
   for (; s[n]; n++)
   {
      char c = s[n];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return 0;
   }
   return n >= 4 && n <= 64;
}

int cochange_pairs_for_commit(char names[][128], int n, int max_files, cochange_pair_t *out,
                              int out_cap)
{
   if (n < 0)
      n = 0;

   /* Dedup in place. */
   int m = 0;
   for (int i = 0; i < n; i++)
   {
      int dup = 0;
      for (int k = 0; k < m; k++)
      {
         if (strcmp(names[k], names[i]) == 0)
         {
            dup = 1;
            break;
         }
      }
      if (!dup)
      {
         if (m != i)
            memcpy(names[m], names[i], 128);
         m++;
      }
   }

   /* Bulk-commit gate: a lone file has no pair; a sweep fabricates coupling. */
   if (m < 2 || m > max_files)
      return 0;

   qsort(names, (size_t)m, 128, cochange_name_cmp);

   int c = 0;
   for (int i = 0; i < m && c < out_cap; i++)
      for (int j = i + 1; j < m && c < out_cap; j++)
      {
         snprintf(out[c].a, sizeof(out[c].a), "%s", names[i]);
         snprintf(out[c].b, sizeof(out[c].b), "%s", names[j]);
         c++;
      }
   return c;
}
