/* evidence_replay.c: deterministic replay of structured review evidence over the
 * read-only code index. See headers/evidence_replay.h. */
#include "evidence_replay.h"

#include "code_index.h" /* db2_code_index_project_count (real prototype) */
#include "index.h"

#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_MAX_HITS 256

const char *replay_status_str(replay_status_t s)
{
   switch (s)
   {
   case REPLAY_MATCH:
      return "match";
   case REPLAY_CORRECTED:
      return "corrected";
   case REPLAY_CONTRADICTED:
      return "contradicted";
   case REPLAY_NO_EVIDENCE:
      return "no-evidence";
   case REPLAY_VACUOUS:
      return "vacuous";
   case REPLAY_INDEX_UNAVAILABLE:
      return "index-unavailable";
   }
   return "unknown";
}

static void sha256_hex_of(const char *data, size_t len, char out[REPLAY_IDKEY_HEX])
{
   unsigned char dg[EVP_MAX_MD_SIZE];
   unsigned int dlen = 0;
   out[0] = '\0';
   EVP_MD_CTX *c = EVP_MD_CTX_new();
   if (!c)
      return;
   if (EVP_DigestInit_ex(c, EVP_sha256(), NULL) == 1 && EVP_DigestUpdate(c, data, len) == 1 &&
       EVP_DigestFinal_ex(c, dg, &dlen) == 1 && dlen >= 32)
   {
      static const char hx[] = "0123456789abcdef";
      for (int i = 0; i < 32; i++)
      {
         out[i * 2] = hx[dg[i] >> 4];
         out[i * 2 + 1] = hx[dg[i] & 0x0f];
      }
      out[64] = '\0';
   }
   EVP_MD_CTX_free(c);
}

static int cmp_str(const void *a, const void *b)
{
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void evidence_idkey(const char (*files)[MAX_PATH_LEN], const int *lines, int n,
                    char out[REPLAY_IDKEY_HEX])
{
   out[0] = '\0';
   if (n <= 0 || !files || !lines)
      return;
   if (n > REPLAY_MAX_HITS)
      n = REPLAY_MAX_HITS;

   /* Build "file:line" tokens, sort ascending, join with '\n', hash. */
   char **toks = calloc((size_t)n, sizeof(*toks));
   if (!toks)
      return;
   size_t total = 0;
   int built = 0;
   for (int i = 0; i < n; i++)
   {
      char buf[MAX_PATH_LEN + 24];
      int m = snprintf(buf, sizeof(buf), "%s:%d", files[i], lines[i]);
      if (m < 0 || m >= (int)sizeof(buf))
         continue; /* error or truncation -> skip (never memcpy past buf) */
      toks[built] = malloc((size_t)m + 1);
      if (!toks[built])
         continue;
      memcpy(toks[built], buf, (size_t)m + 1);
      total += (size_t)m + 1; /* token + separator/NUL */
      built++;
   }
   if (built > 0)
   {
      qsort(toks, (size_t)built, sizeof(*toks), cmp_str);
      /* Collapse duplicate "file:line" tokens so the key is a SET, not a multiset
       * (two callers on one line must not change the hash). */
      int uniq = 0;
      for (int i = 0; i < built; i++)
      {
         if (i > 0 && strcmp(toks[i], toks[uniq - 1]) == 0)
         {
            free(toks[i]);
            continue;
         }
         toks[uniq++] = toks[i];
      }
      char *joined = malloc(total + 1);
      if (joined)
      {
         size_t off = 0;
         for (int i = 0; i < uniq; i++)
         {
            if (i)
               joined[off++] = '\n';
            size_t l = strlen(toks[i]);
            memcpy(joined + off, toks[i], l);
            off += l;
         }
         joined[off] = '\0';
         sha256_hex_of(joined, off, out);
         free(joined);
      }
      built = uniq; /* only the surviving tokens remain to free */
   }
   for (int i = 0; i < built; i++)
      free(toks[i]);
   free(toks);
}

/* Default backend wraps the real read-only index. */
static const replay_backend_t g_real_backend = {
    .find_symbol = index_find,
    .find_callers = index_find_callers,
    .code_search = index_code_search,
    .project_count = db2_code_index_project_count,
};

static void trim_copy(const char *src, char *dst, size_t cap)
{
   dst[0] = '\0';
   if (!src || cap == 0)
      return;
   while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r')
      src++;
   size_t n = strlen(src);
   while (n > 0 &&
          (src[n - 1] == ' ' || src[n - 1] == '\t' || src[n - 1] == '\n' || src[n - 1] == '\r'))
      n--;
   if (n >= cap)
      n = cap - 1;
   memcpy(dst, src, n);
   dst[n] = '\0';
}

/* Decide the verdict from the reproduced count + idkey vs. the claim.
 * `claimed <= 0` means the panelist asserted existence without a specific count
 * (EV_SYMBOL, or a count-less EV_REFS/EV_SEARCH): any reproduction is a MATCH and
 * the reduced record re-grounds the real count, so an under/negative claim cannot
 * inflate anything (the verifier always sees `actual`). */
static replay_status_t grade(int actual, int claimed, const char *claimed_idkey,
                             const char *actual_idkey)
{
   if (actual <= 0)
      return REPLAY_CONTRADICTED; /* claim asserts something the index does not show */
   int idkey_set = claimed_idkey && claimed_idkey[0];
   int idkey_ok = !idkey_set || strcmp(claimed_idkey, actual_idkey) == 0;
   if (claimed <= 0)
      return idkey_ok ? REPLAY_MATCH : REPLAY_CORRECTED;
   if (actual == claimed && idkey_ok)
      return REPLAY_MATCH;
   return REPLAY_CORRECTED; /* reproduced, but count/set differs -> re-ground to actual */
}

replay_status_t evidence_replay_with(const replay_backend_t *be, const review_evidence_t *ev,
                                     reduced_record_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!be || !ev || !out)
      return REPLAY_VACUOUS;
   if (ev->kind == EV_NONE)
      return REPLAY_NO_EVIDENCE;

   char target[sizeof(ev->target)];
   trim_copy(ev->target, target, sizeof(target));
   if (!target[0])
      return REPLAY_VACUOUS;

   char project[sizeof(ev->project)];
   trim_copy(ev->project, project, sizeof(project)); /* bound the project scope too */

   /* If the whole index is empty we cannot ground anything: DEGRADE before any
    * backend call (enabling the gate on an unindexed server must not drop every
    * item, and there is no work to do). A populated index that returns 0 for a
    * specific project is still graded as a claim failure, not a system one. */
   int pc = be->project_count ? be->project_count() : 0;
   if (pc <= 0)
      return REPLAY_INDEX_UNAVAILABLE;

   int n = -1;
   /* Collect file paths + lines for the idkey from whichever hit type applies. */
   char(*files)[MAX_PATH_LEN] = NULL;
   int *lines = NULL;

   if (ev->kind == EV_SYMBOL)
   {
      term_hit_t *hits = calloc(REPLAY_MAX_HITS, sizeof(*hits));
      if (!hits)
         return REPLAY_INDEX_UNAVAILABLE;
      n = be->find_symbol ? be->find_symbol(target, hits, REPLAY_MAX_HITS) : -1;
      if (n > 0)
      {
         files = calloc((size_t)n, sizeof(*files));
         lines = calloc((size_t)n, sizeof(*lines));
         if (files && lines)
            for (int i = 0; i < n; i++)
            {
               snprintf(files[i], MAX_PATH_LEN, "%s", hits[i].file_path);
               lines[i] = hits[i].line;
            }
      }
      free(hits);
   }
   else if (ev->kind == EV_REFS)
   {
      caller_hit_t *hits = calloc(REPLAY_MAX_HITS, sizeof(*hits));
      if (!hits)
         return REPLAY_INDEX_UNAVAILABLE;
      n = be->find_callers ? be->find_callers(project, target, hits, REPLAY_MAX_HITS) : -1;
      if (n > 0)
      {
         files = calloc((size_t)n, sizeof(*files));
         lines = calloc((size_t)n, sizeof(*lines));
         if (files && lines)
            for (int i = 0; i < n; i++)
            {
               snprintf(files[i], MAX_PATH_LEN, "%s", hits[i].file_path);
               lines[i] = hits[i].line;
            }
      }
      free(hits);
   }
   else if (ev->kind == EV_SEARCH)
   {
      code_search_hit_t *hits = calloc(REPLAY_MAX_HITS, sizeof(*hits));
      if (!hits)
         return REPLAY_INDEX_UNAVAILABLE;
      n = be->code_search ? be->code_search(target, project, hits, REPLAY_MAX_HITS) : -1;
      if (n > 0)
      {
         files = calloc((size_t)n, sizeof(*files));
         lines = calloc((size_t)n, sizeof(*lines));
         if (files && lines)
            for (int i = 0; i < n; i++)
            {
               snprintf(files[i], MAX_PATH_LEN, "%s", hits[i].file_path);
               lines[i] = 0; /* search hits are file-level; line 0 keeps the set stable */
            }
      }
      free(hits);
   }
   else
   {
      return REPLAY_VACUOUS; /* unknown kind */
   }

   if (n < 0)
   {
      free(files);
      free(lines);
      return REPLAY_INDEX_UNAVAILABLE; /* DB / connection error -> degrade */
   }

   out->count = n;
   if (files && lines)
      evidence_idkey((const char(*)[MAX_PATH_LEN])files, lines, n, out->idkey);
   free(files);
   free(lines);

   return grade(n, ev->count, ev->idkey, out->idkey);
}

replay_status_t evidence_replay(const review_evidence_t *ev, reduced_record_t *out)
{
   return evidence_replay_with(&g_real_backend, ev, out);
}
