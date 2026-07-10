/* test_workflow.c -- W1 unit tests: block catalog, definition parse, typed
 * validator (every reject path), canonical-form determinism + version hash. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wfe_def.h"
#include "wfe_iface.h"

/* A small but complete valid workflow: author -> gate -> pr -> human -> merge,
 * with a loop-back from the gate to the author. */
static const char *GOOD = "name: mini\n"
                          "start: draft\n"
                          "nodes:\n"
                          "  - id: draft\n"
                          "    block: author.proposal\n"
                          "    next: gate\n"
                          "  - id: gate\n"
                          "    block: gate.roundtable\n"
                          "    in:\n"
                          "      src: draft.out\n"
                          "    params:\n"
                          "      panel:\n"
                          "        required:\n"
                          "          - security\n"
                          "          - architect\n"
                          "    on_pass: pr\n"
                          "    on_fail: draft\n"
                          "  - id: pr\n"
                          "    block: pr.open\n"
                          "    in:\n"
                          "      src: draft.out\n"
                          "    next: approve\n"
                          "  - id: approve\n"
                          "    block: gate.human\n"
                          "    in:\n"
                          "      pr: pr.out\n"
                          "    params:\n"
                          "      policy: pr_review\n"
                          "    next: done\n"
                          "  - id: done\n"
                          "    block: merge\n"
                          "    in:\n"
                          "      pr: pr.out\n";

static int validates(const char *yaml, char *errout, size_t errcap)
{
   char err[256];
   wfe_def_t *d = wfe_def_parse(yaml, err, sizeof err);
   if (!d)
   {
      if (errout)
         snprintf(errout, errcap, "parse: %s", err);
      return 0;
   }
   int rc = wfe_def_validate(d, err, sizeof err);
   if (rc != 0 && errout)
      snprintf(errout, errcap, "%s", err);
   wfe_def_free(d);
   return rc == 0;
}

int main(void)
{
   printf("workflow: ");

   /* --- block catalog --- */
   assert(wfe_block_from_name("author.proposal") == WFE_BLK_AUTHOR_PROPOSAL);
   assert(wfe_block_from_name("gate.roundtable") == WFE_BLK_GATE_ROUNDTABLE);
   assert(wfe_block_from_name("nonsense") == WFE_BLK_UNKNOWN);
   /* the composable documentation block: branch -> branch */
   assert(wfe_block_from_name("document") == WFE_BLK_DOCUMENT);
   assert(wfe_block_output(WFE_BLK_DOCUMENT) == WFE_ART_BRANCH);
   assert(wfe_block_accepts_input(WFE_BLK_DOCUMENT, WFE_ART_BRANCH) == 1);
   assert(wfe_block_accepts_input(WFE_BLK_DOCUMENT, WFE_ART_PROPOSAL) == 0);
   assert(wfe_block_output(WFE_BLK_AUTHOR_PROPOSAL) == WFE_ART_PROPOSAL);
   assert(wfe_block_output(WFE_BLK_MERGE) == WFE_ART_NONE);
   assert(wfe_block_accepts_input(WFE_BLK_AUTHOR_PLAN, WFE_ART_PROPOSAL) == 1);
   assert(wfe_block_accepts_input(WFE_BLK_AUTHOR_PLAN, WFE_ART_PR) == 0);
   assert(wfe_block_requires_input(WFE_BLK_AUTHOR_PROPOSAL) == 0);
   assert(wfe_block_requires_input(WFE_BLK_MERGE) == 1);

   /* --- a good workflow validates --- */
   {
      char err[256] = "";
      int ok = validates(GOOD, err, sizeof err);
      if (!ok)
         printf("\n  GOOD rejected: %s\n", err);
      assert(ok);
   }

   /* --- version determinism + canonical stability under key reordering --- */
   {
      char err[256];
      wfe_def_t *a = wfe_def_parse(GOOD, err, sizeof err);
      assert(a);
      char va[65] = "", vb[65] = "";
      assert(wfe_def_compute_version(a, va) == 0);
      assert(strlen(va) == 64);

      /* Same content, node order shuffled + a key reordered: same version. */
      static const char *SHUF = "name: mini\n"
                                "start: draft\n"
                                "nodes:\n"
                                "  - id: done\n"
                                "    block: merge\n"
                                "    in:\n"
                                "      pr: pr.out\n"
                                "  - id: gate\n"
                                "    block: gate.roundtable\n"
                                "    on_fail: draft\n"
                                "    on_pass: pr\n"
                                "    in:\n"
                                "      src: draft.out\n"
                                "    params:\n"
                                "      panel:\n"
                                "        required:\n"
                                "          - security\n"
                                "          - architect\n"
                                "  - id: pr\n"
                                "    block: pr.open\n"
                                "    in:\n"
                                "      src: draft.out\n"
                                "    next: approve\n"
                                "  - id: approve\n"
                                "    block: gate.human\n"
                                "    in:\n"
                                "      pr: pr.out\n"
                                "    params:\n"
                                "      policy: pr_review\n"
                                "    next: done\n"
                                "  - id: draft\n"
                                "    block: author.proposal\n"
                                "    next: gate\n";
      wfe_def_t *b = wfe_def_parse(SHUF, err, sizeof err);
      assert(b);
      assert(wfe_def_compute_version(b, vb) == 0);
      assert(strcmp(va, vb) == 0); /* canonical form is order-independent */
      wfe_def_free(a);
      wfe_def_free(b);
   }

   /* --- reject paths --- */
   {
      char e[256];
      /* unknown block */
      assert(!validates("name: x\nnodes:\n  - id: a\n    block: bogus\n", e, sizeof e));
      /* dangling edge */
      assert(!validates(
          "name: x\nstart: a\nnodes:\n  - id: a\n    block: author.proposal\n    next: ghost\n", e,
          sizeof e));
      /* no terminal: a -> b -> a (every node has an outgoing edge) */
      assert(!validates("name: x\nstart: a\nnodes:\n"
                        "  - id: a\n    block: author.proposal\n    next: b\n"
                        "  - id: b\n    block: author.plan\n    in:\n      p: a.out\n"
                        "    next: a\n",
                        e, sizeof e));
      /* type mismatch: author.plan fed a pr */
      assert(!validates("name: x\nstart: a\nnodes:\n"
                        "  - id: pr\n    block: pr.open\n    in:\n      s: a.out\n"
                        "  - id: a\n    block: author.proposal\n    next: bad\n"
                        "  - id: bad\n    block: author.plan\n    in:\n      p: pr.out\n",
                        e, sizeof e));
      /* single-lens gate (required < 2) */
      assert(!validates("name: x\nstart: a\nnodes:\n"
                        "  - id: a\n    block: author.proposal\n    next: g\n"
                        "  - id: g\n    block: gate.roundtable\n    in:\n      s: a.out\n"
                        "    params:\n      panel:\n        required:\n          - security\n",
                        e, sizeof e));
      /* gate.human optional:true — forbidden (a human gate is inviolable) */
      assert(!validates("name: x\nstart: a\nnodes:\n"
                        "  - id: a\n    block: author.proposal\n    next: h\n"
                        "  - id: h\n    block: gate.human\n    in:\n      s: a.out\n"
                        "    params:\n      optional: true\n",
                        e, sizeof e));
      /* gate.human policy:preauthorized — forbidden (must not be auto-satisfiable) */
      assert(!validates("name: x\nstart: a\nnodes:\n"
                        "  - id: a\n    block: author.proposal\n    next: h\n"
                        "  - id: h\n    block: gate.human\n    in:\n      s: a.out\n"
                        "    params:\n      policy: preauthorized\n",
                        e, sizeof e));
      /* a plain gate.human (no auto-satisfy params) is valid */
      assert(validates("name: x\nstart: a\nnodes:\n"
                       "  - id: a\n    block: author.proposal\n    next: h\n"
                       "  - id: h\n    block: gate.human\n    in:\n      s: a.out\n    next: p\n"
                       "  - id: p\n    block: pr.open\n    in:\n      s: a.out\n",
                       e, sizeof e));
      /* missing required input: author.plan with no input binding */
      assert(!validates("name: x\nstart: a\nnodes:\n"
                        "  - id: a\n    block: author.plan\n",
                        e, sizeof e));
      /* unreachable node */
      assert(!validates("name: x\nstart: a\nnodes:\n"
                        "  - id: a\n    block: author.proposal\n"
                        "  - id: orphan\n    block: author.proposal\n",
                        e, sizeof e));
   }

   /* --- executor vtable (W2 will register real ones; here: register/lookup) --- */
   {
      wfe_reset_block_executors();
      assert(wfe_lookup_block_executor(WFE_BLK_MERGE) == NULL);
   }

   /* --- shipped default build.yaml validates (when run from src/) --- */
   {
      char err[256];
      wfe_def_t *d = wfe_def_load_file("../config/workflows/build.yaml", err, sizeof err);
      if (d)
      {
         int rc = wfe_def_validate(d, err, sizeof err);
         if (rc != 0)
            printf("\n  build.yaml invalid: %s\n", err);
         assert(rc == 0);
         char v[65] = "";
         assert(wfe_def_compute_version(d, v) == 0);
         wfe_def_free(d);
      }
      else
      {
         printf("(skip build.yaml: %s) ", err);
      }
   }

   /* --- shipped manual-review.yaml (the sweep's filing target) validates --- */
   {
      char err[256];
      wfe_def_t *d = wfe_def_load_file("../config/workflows/manual-review.yaml", err, sizeof err);
      if (d)
      {
         int rc = wfe_def_validate(d, err, sizeof err);
         if (rc != 0)
            printf("\n  manual-review.yaml invalid: %s\n", err);
         assert(rc == 0);
         /* security invariant: the sweep's untrusted output must NOT auto-implement —
          * manual-review contains no implement/merge node, only a human gate. */
         for (int i = 0; i < d->n_nodes; i++)
         {
            assert(d->nodes[i].block != WFE_BLK_IMPLEMENT);
            assert(d->nodes[i].block != WFE_BLK_MERGE);
         }
         wfe_def_free(d);
      }
      else
      {
         printf("(skip manual-review.yaml: %s) ", err);
      }
   }

   printf("ok\n");
   return 0;
}
