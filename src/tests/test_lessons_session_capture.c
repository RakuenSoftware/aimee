/* test_lessons_session_capture.c: the server-driven live cite-capture session map
 * (graph-feedback §3). Verifies the auto-useful proxy across turns, per-session
 * isolation, and the >=1-re-citation trigger. kb_store writes are stubbed. */
#include "kb/lessons_session_capture.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── kb_store stubs: count the outcome/citation writes the capture triggers ── */
static int g_outcomes = 0;
static int g_citations = 0;
int64_t kb_store_lessons_record_outcome(const char *session_id, const char *turn_id,
                                        const char *project_id, int64_t generation_id,
                                        const char *answer_outcome, const char *correction_text,
                                        const char *finding_id, const char *actor_id,
                                        const char *actor_source, int confirmed)
{
   (void)session_id;
   (void)turn_id;
   (void)project_id;
   (void)generation_id;
   (void)answer_outcome;
   (void)correction_text;
   (void)finding_id;
   (void)actor_id;
   (void)actor_source;
   (void)confirmed;
   return ++g_outcomes; /* a positive oid */
}
int kb_store_lessons_record_citation(int64_t outcome_id, const char *node_id, const char *stance)
{
   (void)outcome_id;
   (void)node_id;
   (void)stance;
   g_citations++;
   return 0;
}

static const char *nodes1[] = {"src/a.c", "src/b.c"};

/* A node re-cited within the auto-useful window (default 3 turns) fires exactly once
 * on the re-citation; the first sighting does not. */
static void test_cite_again_fires(void)
{
   g_outcomes = g_citations = 0;
   int r1 = lessons_session_observe("proj", 1, "sess-A", nodes1, 2); /* turn 1: first sightings */
   assert(r1 == 0);
   int r2 = lessons_session_observe("proj", 1, "sess-A", nodes1, 2); /* turn 2: re-cited → fire */
   assert(r2 == 2);
   assert(g_outcomes == 2 && g_citations == 2);
   printf("  test_cite_again_fires: ok\n");
}

/* Two sessions are isolated: a node seen once in each never fires (no re-citation
 * within either session's own tracker). */
static void test_session_isolation(void)
{
   g_outcomes = g_citations = 0;
   const char *n[] = {"src/x.c"};
   assert(lessons_session_observe("proj", 1, "sess-B", n, 1) == 0);
   assert(lessons_session_observe("proj", 1, "sess-C", n, 1) == 0); /* different session */
   assert(g_outcomes == 0);                                         /* no cross-session fire */
   /* re-cite in B → fires; C is unaffected */
   assert(lessons_session_observe("proj", 1, "sess-B", n, 1) == 1);
   printf("  test_session_isolation: ok\n");
}

static void test_bad_args(void)
{
   const char *n[] = {"a"};
   assert(lessons_session_observe(NULL, 1, "s", n, 1) == -1);
   assert(lessons_session_observe("p", 1, NULL, n, 1) == -1);
   assert(lessons_session_observe("p", 1, "s", NULL, 1) == -1);
   assert(lessons_session_observe("p", 1, "s", n, -1) == -1);
   printf("  test_bad_args: ok\n");
}

int main(void)
{
   printf("test_lessons_session_capture:\n");
   test_cite_again_fires();
   test_session_isolation();
   test_bad_args();
   printf("ALL PASS\n");
   return 0;
}
