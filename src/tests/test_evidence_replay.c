/* Unit tests for the structured-evidence replay engine (Part A).
 * Pure: drives the logic through an injected fake backend; the real index_*
 * symbols are stubbed so the link needs no db2. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "evidence_replay.h"

/* --- stubs for the real backend's referenced symbols (never exercised here:
 *     all logic tests inject a fake backend explicitly) --- */
int index_find(const char *id, term_hit_t *out, int max)
{
   (void)id;
   (void)out;
   (void)max;
   return -1;
}
int index_find_callers(const char *project, const char *symbol, caller_hit_t *out, int max)
{
   (void)project;
   (void)symbol;
   (void)out;
   (void)max;
   return -1;
}
int index_code_search(const char *query, const char *project, code_search_hit_t *out, int max)
{
   (void)query;
   (void)project;
   (void)out;
   (void)max;
   return -1;
}
int db2_code_index_project_count(void)
{
   return 0;
}

/* --- fake backend, scriptable per test --- */
static int g_pc;         /* project_count return */
static int g_caller_ret; /* find_callers return (count or -1) */
static int g_caller_n;   /* how many caller rows to synthesize */
static int g_symbol_ret; /* find_symbol return */
static int g_symbol_n;   /* symbol rows to synthesize */
static int g_search_ret; /* code_search return */
static int g_search_n;   /* search rows to synthesize */

static int fake_pc(void)
{
   return g_pc;
}
static int fake_find_symbol(const char *id, term_hit_t *out, int max)
{
   (void)id;
   if (g_symbol_ret < 0)
      return -1;
   int n = g_symbol_n < max ? g_symbol_n : max;
   for (int i = 0; i < n; i++)
   {
      snprintf(out[i].file_path, sizeof(out[i].file_path), "src/foo_%d.c", i);
      out[i].line = 10 + i;
   }
   return g_symbol_ret;
}
static int fake_find_callers(const char *project, const char *symbol, caller_hit_t *out, int max)
{
   (void)project;
   (void)symbol;
   if (g_caller_ret < 0)
      return -1;
   int n = g_caller_n < max ? g_caller_n : max;
   for (int i = 0; i < n; i++)
   {
      snprintf(out[i].file_path, sizeof(out[i].file_path), "src/caller_%d.c", i);
      out[i].line = 100 + i;
   }
   return g_caller_ret;
}
static int fake_code_search(const char *q, const char *project, code_search_hit_t *out, int max)
{
   (void)q;
   (void)project;
   if (g_search_ret < 0)
      return -1;
   int n = g_search_n < max ? g_search_n : max;
   for (int i = 0; i < n; i++)
      snprintf(out[i].file_path, sizeof(out[i].file_path), "src/hit_%d.c", i);
   return g_search_ret;
}

static replay_backend_t fake_backend(void)
{
   replay_backend_t be;
   be.find_symbol = fake_find_symbol;
   be.find_callers = fake_find_callers;
   be.code_search = fake_code_search;
   be.project_count = fake_pc;
   return be;
}

static void test_idkey_order_independence(void)
{
   char files1[3][MAX_PATH_LEN] = {"src/b.c", "src/a.c", "src/c.c"};
   int lines1[3] = {3, 1, 2};
   char files2[3][MAX_PATH_LEN] = {"src/a.c", "src/c.c", "src/b.c"};
   int lines2[3] = {1, 2, 3};
   char k1[REPLAY_IDKEY_HEX], k2[REPLAY_IDKEY_HEX];
   evidence_idkey(files1, lines1, 3, k1);
   evidence_idkey(files2, lines2, 3, k2);
   assert(strlen(k1) == 64);
   assert(strcmp(k1, k2) == 0); /* same set, different order -> same key */

   /* a different set must differ */
   char files3[2][MAX_PATH_LEN] = {"src/a.c", "src/b.c"};
   int lines3[2] = {1, 3};
   char k3[REPLAY_IDKEY_HEX];
   evidence_idkey(files3, lines3, 2, k3);
   assert(strcmp(k1, k3) != 0);
}

static void test_no_evidence(void)
{
   replay_backend_t be = fake_backend();
   review_evidence_t ev;
   memset(&ev, 0, sizeof(ev)); /* EV_NONE */
   reduced_record_t r;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_NO_EVIDENCE);
}

static void test_vacuous(void)
{
   replay_backend_t be = fake_backend();
   g_pc = 1;
   review_evidence_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.kind = EV_REFS; /* non-NONE but empty target */
   reduced_record_t r;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_VACUOUS);

   /* whitespace-only target is also vacuous */
   snprintf(ev.target, sizeof(ev.target), "   ");
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_VACUOUS);
}

static void test_index_unavailable_degrades(void)
{
   replay_backend_t be = fake_backend();
   review_evidence_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.kind = EV_REFS;
   snprintf(ev.target, sizeof(ev.target), "some_symbol");
   ev.count = 5;
   reduced_record_t r;

   /* empty index (0 projects) -> degrade, never reject */
   g_pc = 0;
   g_caller_ret = 0;
   g_caller_n = 0;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_INDEX_UNAVAILABLE);

   /* DB error (-1) -> degrade */
   g_pc = 1;
   g_caller_ret = -1;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_INDEX_UNAVAILABLE);
}

static void test_refs_match_correct_contradict(void)
{
   replay_backend_t be = fake_backend();
   g_pc = 1;
   review_evidence_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.kind = EV_REFS;
   snprintf(ev.target, sizeof(ev.target), "sym");
   reduced_record_t r;

   /* claim 3, reproduce 3 -> MATCH, count re-grounded */
   ev.count = 3;
   g_caller_ret = 3;
   g_caller_n = 3;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_MATCH);
   assert(r.count == 3);
   assert(strlen(r.idkey) == 64);

   /* claim 5, reproduce 3 -> CORRECTED (re-ground to actual 3) */
   ev.count = 5;
   g_caller_ret = 3;
   g_caller_n = 3;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_CORRECTED);
   assert(r.count == 3);

   /* claim 5, reproduce 0 (populated index) -> CONTRADICTED */
   ev.count = 5;
   g_caller_ret = 0;
   g_caller_n = 0;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_CONTRADICTED);
}

static void test_symbol_exists_or_not(void)
{
   replay_backend_t be = fake_backend();
   g_pc = 1;
   review_evidence_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.kind = EV_SYMBOL;
   snprintf(ev.target, sizeof(ev.target), "thing");
   reduced_record_t r;

   g_symbol_ret = 2;
   g_symbol_n = 2;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_MATCH);
   assert(r.count == 2);

   g_symbol_ret = 0;
   g_symbol_n = 0;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_CONTRADICTED);
}

static void test_search_positive_and_dberror(void)
{
   replay_backend_t be = fake_backend();
   g_pc = 1;
   review_evidence_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.kind = EV_SEARCH;
   snprintf(ev.target, sizeof(ev.target), "needle");
   reduced_record_t r;

   ev.count = 0; /* existence-only */
   g_search_ret = 4;
   g_search_n = 4;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_MATCH);
   assert(r.count == 4);
   assert(strlen(r.idkey) == 64); /* file-level hits still produce a stable key */

   g_search_ret = -1; /* DB error -> degrade (parity with EV_REFS) */
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_INDEX_UNAVAILABLE);
}

static void test_symbol_dberror(void)
{
   replay_backend_t be = fake_backend();
   g_pc = 1;
   review_evidence_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.kind = EV_SYMBOL;
   snprintf(ev.target, sizeof(ev.target), "thing");
   reduced_record_t r;
   g_symbol_ret = -1;
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_INDEX_UNAVAILABLE);
}

static void test_claimed_idkey_match_and_mismatch(void)
{
   replay_backend_t be = fake_backend();
   g_pc = 1;
   review_evidence_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.kind = EV_REFS;
   snprintf(ev.target, sizeof(ev.target), "sym");
   ev.count = 2;
   g_caller_ret = 2;
   g_caller_n = 2;
   reduced_record_t r;

   /* learn the actual idkey, then assert it back -> MATCH */
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_MATCH);
   snprintf(ev.idkey, sizeof(ev.idkey), "%s", r.idkey);
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_MATCH);

   /* a wrong idkey (right count) -> CORRECTED (set differs) */
   memset(ev.idkey, 'a', 64);
   ev.idkey[64] = '\0';
   assert(evidence_replay_with(&be, &ev, &r) == REPLAY_CORRECTED);
}

static void test_idkey_cap_and_dedup(void)
{
   /* > REPLAY_MAX_HITS is clamped; duplicate file:line collapses to a set. */
   static char files[300][MAX_PATH_LEN];
   static int lines[300];
   for (int i = 0; i < 300; i++)
   {
      snprintf(files[i], MAX_PATH_LEN, "src/x.c");
      lines[i] = 1; /* all identical -> set of size 1 */
   }
   char kbig[REPLAY_IDKEY_HEX];
   evidence_idkey(files, lines, 300, kbig);

   char one_file[1][MAX_PATH_LEN] = {"src/x.c"};
   int one_line[1] = {1};
   char kone[REPLAY_IDKEY_HEX];
   evidence_idkey(one_file, one_line, 1, kone);
   assert(strcmp(kbig, kone) == 0); /* dedup: 300 copies == the single token */
}

static void test_real_backend_degrades(void)
{
   /* the real backend routes to the stubs above (project_count 0) -> degrade */
   review_evidence_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.kind = EV_SYMBOL;
   snprintf(ev.target, sizeof(ev.target), "x");
   reduced_record_t r;
   assert(evidence_replay(&ev, &r) == REPLAY_INDEX_UNAVAILABLE);
}

static void test_status_strings(void)
{
   assert(strcmp(replay_status_str(REPLAY_MATCH), "match") == 0);
   assert(strcmp(replay_status_str(REPLAY_CONTRADICTED), "contradicted") == 0);
   assert(strcmp(replay_status_str(REPLAY_INDEX_UNAVAILABLE), "index-unavailable") == 0);
}

int main(void)
{
   test_idkey_order_independence();
   test_no_evidence();
   test_vacuous();
   test_index_unavailable_degrades();
   test_refs_match_correct_contradict();
   test_symbol_exists_or_not();
   test_search_positive_and_dberror();
   test_symbol_dberror();
   test_claimed_idkey_match_and_mismatch();
   test_idkey_cap_and_dedup();
   test_real_backend_degrades();
   test_status_strings();
   printf("evidence_replay: all tests passed\n");
   return 0;
}
