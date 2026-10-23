/* Curator judge business parsing through a canonical mock provider. */
#include "kb_curator_llm.h"
#include "support/curator_model_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kb_curator_judge.h"

static void test_no_command(void)
{
   /* Empty command -> error, *out_same untouched. */
   int same = 42;
   char err[256];
   int rc =
       kb_curator_judge_same_entity("", "Acme", "ctx", "Acme Corp", 0.78, &same, err, sizeof(err));
   assert(rc == -1);
   assert(same == 42); /* left untouched */
   printf("  no-command -> error OK\n");
}

static void test_same_true(void)
{
   int same = -1;
   char err[256];
   /* The sidecar ignores stdin and emits the canonical affirmative response. */
   int rc = kb_curator_judge_same_entity(curator_fixture_reply("{\"same_entity\": true}"), "Acme",
                                         "the vendor", "Acme Corp", 0.80, &same, err, sizeof(err));
   assert(rc == 0);
   assert(same == 1);
   printf("  same_entity=true parsed OK\n");
}

static void test_same_false(void)
{
   int same = -1;
   char err[256];
   int rc = kb_curator_judge_same_entity(curator_fixture_reply("{\"same_entity\": false}"), "Apple",
                                         "the fruit", "Apple Inc", 0.72, &same, err, sizeof(err));
   assert(rc == 0);
   assert(same == 0);
   printf("  same_entity=false parsed OK\n");
}

static void test_prose_wrapped(void)
{
   /* A chatty model may prefix prose before the JSON object; we scan to '{'. */
   int same = -1;
   char err[256];
   int rc = kb_curator_judge_same_entity(
       curator_fixture_reply("Sure! Here is my verdict:\\n{\"same_entity\": true}\\n"), "K8s",
       "orchestrator", "Kubernetes", 0.83, &same, err, sizeof(err));
   assert(rc == 0);
   assert(same == 1);
   printf("  prose-wrapped JSON parsed OK\n");
}

static void test_garbage_output(void)
{
   int same = 7;
   char err[256];
   int rc = kb_curator_judge_same_entity(curator_fixture_reply("not-json-at-all"), "x", "", "y",
                                         0.75, &same, err, sizeof(err));
   assert(rc == -1);
   assert(same == 7);
   printf("  garbage output -> error OK\n");
}

static void test_nonzero_exit(void)
{
   int same = 9;
   char err[256];
   int rc = kb_curator_judge_same_entity(curator_fixture_failure(), "x", "", "y", 0.75, &same, err,
                                         sizeof(err));
   assert(rc == -1);
   assert(same == 9);
   printf("  non-zero exit -> error OK\n");
}

int main(void)
{
   test_no_command();
   test_same_true();
   test_same_false();
   test_prose_wrapped();
   test_garbage_output();
   test_nonzero_exit();
   printf("curator_judge: all tests passed\n");
   return 0;
}
