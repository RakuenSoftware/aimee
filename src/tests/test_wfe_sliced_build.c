/* test_wfe_sliced_build.c -- the sliced-lifecycle "build" workflow: the new
 * branch.open / foreach.workflow blocks type-check, split accepts a plan, and the
 * parent + child ("slice") graphs validate and version stably. The embedded graphs
 * mirror config/workflows/{build,slice}.yaml (the shipped files are additionally
 * validated by `aimee workflow validate`). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wfe_def.h"
#include "wfe_iface.h"

/* The parent "build" spine: author.plan -> roundtable(plan+proposal) -> branch.open
 * -> split -> foreach.workflow -> freeze -> acceptance roundtable -> PR -> ci ->
 * human -> merge. */
/* NB: block-style `in:`/`params:` (not inline flow maps) -- the aimee YAML reader
 * expands bindings from block mappings only; these mirror the shipped
 * config/workflows build.yaml and slice.yaml. */
static const char *BUILD =
    "name: build\n"
    "intent_tags:\n"
    "  - proposal\n"
    "  - full-lifecycle\n"
    "start: draft\n"
    "nodes:\n"
    "  - id: draft\n"
    "    block: author.proposal\n"
    "    params:\n"
    "      with_user: true\n"
    "    next: plan\n"
    "    on_fail: draft\n"
    "  - id: plan\n"
    "    block: author.plan\n"
    "    in:\n"
    "      proposal: draft.out\n"
    "    next: plan_gate\n"
    "    on_fail: plan\n"
    "  - id: plan_gate\n"
    "    block: gate.roundtable\n"
    "    in:\n"
    "      src: plan.out\n"
    "    params:\n"
    "      panel:\n"
    "        required:\n"
    "          - security\n"
    "          - architect\n"
    "          - qa\n"
    "          - reviewer\n"
    "        eligible:\n"
    "          - contrarian\n"
    "      quorum: 4\n"
    "      max_rounds: 6\n"
    "      focus: does this implementation plan fully satisfy the proposal/request?\n"
    "    on_pass: feature\n"
    "    on_fail: plan\n"
    "  - id: feature\n"
    "    block: branch.open\n"
    "    in:\n"
    "      src: plan.out\n"
    "    next: split\n"
    "  - id: split\n"
    "    block: split\n"
    "    in:\n"
    "      plan: plan.out\n"
    "    next: slices\n"
    "  - id: slices\n"
    "    block: foreach.workflow\n"
    "    in:\n"
    "      packets: split.out\n"
    "      feature: feature.out\n"
    "    params:\n"
    "      workflow: slice\n"
    "    next: accept_freeze\n"
    "  - id: accept_freeze\n"
    "    block: freeze\n"
    "    in:\n"
    "      branch: slices.out\n"
    "    next: accept_gate\n"
    "  - id: accept_gate\n"
    "    block: gate.roundtable\n"
    "    in:\n"
    "      src: accept_freeze.out\n"
    "    params:\n"
    "      panel:\n"
    "        required:\n"
    "          - security\n"
    "          - architect\n"
    "          - qa\n"
    "          - reviewer\n"
    "      quorum: 4\n"
    "      max_rounds: 6\n"
    "      focus: was the proposal completed? assess code quality and any missing tests.\n"
    "    on_pass: final_pr\n"
    "    on_fail: split\n"
    "  - id: final_pr\n"
    "    block: pr.open\n"
    "    in:\n"
    "      src: accept_freeze.out\n"
    "    next: final_ci\n"
    "  - id: final_ci\n"
    "    block: gate.ci\n"
    "    in:\n"
    "      pr: final_pr.out\n"
    "    on_pass: human\n"
    "    on_fail: split\n"
    "  - id: human\n"
    "    block: gate.human\n"
    "    in:\n"
    "      pr: final_pr.out\n"
    "    params:\n"
    "      policy: pr_review\n"
    "    on_pass: merge\n"
    "    on_fail: split\n"
    "  - id: merge\n"
    "    block: merge\n"
    "    in:\n"
    "      pr: final_pr.out\n";

/* The child "slice" spine: understand -> implement -> freeze -> roundtable -> PR ->
 * green CI -> merge into the feature branch. */
static const char *SLICE = "name: slice\n"
                           "intent_tags:\n"
                           "  - slice\n"
                           "start: scope\n"
                           "nodes:\n"
                           "  - id: scope\n"
                           "    block: understand\n"
                           "    params:\n"
                           "      with_user: false\n"
                           "    next: impl\n"
                           "    on_fail: scope\n"
                           "  - id: impl\n"
                           "    block: implement\n"
                           "    in:\n"
                           "      plan: scope.out\n"
                           "    next: freeze\n"
                           "  - id: freeze\n"
                           "    block: freeze\n"
                           "    in:\n"
                           "      branch: impl.out\n"
                           "    next: rt_gate\n"
                           "  - id: rt_gate\n"
                           "    block: gate.roundtable\n"
                           "    in:\n"
                           "      src: freeze.out\n"
                           "    params:\n"
                           "      panel:\n"
                           "        required:\n"
                           "          - security\n"
                           "          - architect\n"
                           "          - qa\n"
                           "          - reviewer\n"
                           "      quorum: 4\n"
                           "      max_rounds: 6\n"
                           "    on_pass: pr\n"
                           "    on_fail: impl\n"
                           "  - id: pr\n"
                           "    block: pr.open\n"
                           "    in:\n"
                           "      src: freeze.out\n"
                           "    next: ci\n"
                           "  - id: ci\n"
                           "    block: gate.ci\n"
                           "    in:\n"
                           "      pr: pr.out\n"
                           "    on_pass: merge\n"
                           "    on_fail: impl\n"
                           "  - id: merge\n"
                           "    block: merge\n"
                           "    in:\n"
                           "      pr: pr.out\n";

static void check_valid(const char *yaml, const char *label, char ver[65])
{
   char err[256] = "";
   wfe_def_t *d = wfe_def_parse(yaml, err, sizeof err);
   assert(d && "parse");
   if (wfe_def_validate(d, err, sizeof err) != 0)
   {
      fprintf(stderr, "\n%s: validate failed: %s\n", label, err);
      assert(0);
   }
   assert(wfe_def_compute_version(d, ver) == 0);
   assert(ver[0] && "version");
   wfe_def_free(d);
}

int main(void)
{
   printf("wfe-sliced-build: ");
   (void)wfe_custom_registry_ensure(NULL, 0); /* built-ins only; idempotent */

   /* --- new block catalog typing --- */
   assert(wfe_block_from_name("branch.open") == WFE_BLK_BRANCH_OPEN);
   assert(wfe_block_from_name("foreach.workflow") == WFE_BLK_FOREACH_WORKFLOW);
   assert(wfe_block_output(WFE_BLK_BRANCH_OPEN) == WFE_ART_BRANCH);
   assert(wfe_block_output(WFE_BLK_FOREACH_WORKFLOW) == WFE_ART_BRANCH);
   assert(wfe_block_requires_input(WFE_BLK_BRANCH_OPEN) == 0);      /* source-like */
   assert(wfe_block_requires_input(WFE_BLK_FOREACH_WORKFLOW) == 1); /* needs packets */
   assert(wfe_block_accepts_input(WFE_BLK_BRANCH_OPEN, WFE_ART_PLAN) == 1);
   assert(wfe_block_accepts_input(WFE_BLK_FOREACH_WORKFLOW, WFE_ART_PLAN) == 1);
   assert(wfe_block_accepts_input(WFE_BLK_FOREACH_WORKFLOW, WFE_ART_BRANCH) == 1);
   /* split now accepts a PLAN directly (decompose the implementation plan). */
   assert(wfe_block_accepts_input(WFE_BLK_SPLIT, WFE_ART_PLAN) == 1);
   assert(wfe_block_accepts_input(WFE_BLK_SPLIT, WFE_ART_INTENT) == 1);

   /* --- both graphs validate --- */
   char vb1[65] = "", vb2[65] = "", vs[65] = "";
   check_valid(BUILD, "build", vb1);
   check_valid(SLICE, "slice", vs);

   /* --- version is deterministic (stable across a re-parse) --- */
   check_valid(BUILD, "build#2", vb2);
   assert(strcmp(vb1, vb2) == 0 && "build version stable");

   printf("ok\n");
   return 0;
}
