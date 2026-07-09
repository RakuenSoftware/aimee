/* test_wfe_gate_reject.c -- wfe_gate_reject_target: how an operator reject at a
 * human gate routes, decided purely from the parsed workflow def. Reject is
 * TERMINAL by default; a gate opts into loop-back via params.retry_on_reject:true
 * following its on_fail edge, but only if that on_fail target still resolves. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wfe_def.h"

/* A gate `g` that opts into retry and loops back to `impl` (which exists). */
static const char *WF_RETRY = "name: t\nstart: g\nnodes:\n"
                              "  - id: g\n    block: gate.human\n    params:\n"
                              "      retry_on_reject: true\n    on_fail: impl\n    on_pass: m\n"
                              "  - id: impl\n    block: implement\n    next: g\n"
                              "  - id: m\n    block: merge\n";

/* Same, but the gate does NOT opt in -> terminal. */
static const char *WF_NOOPT =
    "name: t\nstart: g\nnodes:\n"
    "  - id: g\n    block: gate.human\n    on_fail: impl\n    on_pass: m\n"
    "  - id: impl\n    block: implement\n    next: g\n"
    "  - id: m\n    block: merge\n";

/* Opts in but has NO on_fail edge -> nowhere to loop back -> terminal. */
static const char *WF_NOEDGE = "name: t\nstart: g\nnodes:\n"
                               "  - id: g\n    block: gate.human\n    params:\n"
                               "      retry_on_reject: true\n    on_pass: m\n"
                               "  - id: m\n    block: merge\n";

/* Opts in with an on_fail edge naming a node that does NOT exist -> degrade to
 * terminal (a removed/renamed target never becomes a dangling stage). */
static const char *WF_DANGLING = "name: t\nstart: g\nnodes:\n"
                                 "  - id: g\n    block: gate.human\n    params:\n"
                                 "      retry_on_reject: true\n    on_fail: gone\n    on_pass: m\n"
                                 "  - id: m\n    block: merge\n";

/* ---- Generic loop cap (max_iters / on_max) fixtures ----
 * Built from author.proposal nodes (which require no input binding) so the ONLY
 * thing under test is the loop-cap validation, not unrelated input/gate rules.
 * The cap params + on_fail/on_pass edges are generic across node types. */
/* Node g carries an explicit cap + policy; the rest is a valid loop workflow. */
static const char *WF_CAP_OK = "name: t\nstart: g\nnodes:\n"
                               "  - id: g\n    block: author.proposal\n    params:\n"
                               "      max_iters: 5\n      on_max: fail\n"
                               "    on_fail: impl\n    on_pass: m\n"
                               "  - id: impl\n    block: author.proposal\n    next: g\n"
                               "  - id: m\n    block: author.proposal\n";
/* on_max:pass WITH a forward edge (on_pass) -> valid. */
static const char *WF_CAP_PASS_OK = "name: t\nstart: g\nnodes:\n"
                                    "  - id: g\n    block: author.proposal\n    params:\n"
                                    "      on_max: pass\n    on_fail: impl\n    on_pass: m\n"
                                    "  - id: impl\n    block: author.proposal\n    next: g\n"
                                    "  - id: m\n    block: author.proposal\n";
/* max_iters <= 0 -> invalid. */
static const char *WF_CAP_BADITERS = "name: t\nstart: g\nnodes:\n"
                                     "  - id: g\n    block: author.proposal\n    params:\n"
                                     "      max_iters: 0\n    on_fail: impl\n    on_pass: m\n"
                                     "  - id: impl\n    block: author.proposal\n    next: g\n"
                                     "  - id: m\n    block: author.proposal\n";
/* on_max not in the enum -> invalid. */
static const char *WF_CAP_BADONMAX = "name: t\nstart: g\nnodes:\n"
                                     "  - id: g\n    block: author.proposal\n    params:\n"
                                     "      on_max: bogus\n    on_fail: impl\n    on_pass: m\n"
                                     "  - id: impl\n    block: author.proposal\n    next: g\n"
                                     "  - id: m\n    block: author.proposal\n";
/* on_max:pass but NO on_pass/next forward edge -> invalid (nowhere to route). */
static const char *WF_CAP_PASS_NOEDGE = "name: t\nstart: g\nnodes:\n"
                                        "  - id: g\n    block: author.proposal\n    params:\n"
                                        "      on_max: pass\n    on_fail: impl\n"
                                        "  - id: impl\n    block: author.proposal\n    next: g\n";

static wfe_def_t *parse(const char *yaml)
{
   char err[256] = "";
   wfe_def_t *d = wfe_def_parse(yaml, err, sizeof err);
   assert(d && "parse failed");
   return d;
}

/* Validate `yaml`, returning 0/-1 and copying the error message into `err`. */
static int validate_yaml(const char *yaml, char *err, size_t errlen)
{
   wfe_def_t *d = parse(yaml);
   int rc = wfe_def_validate(d, err, errlen);
   wfe_def_free(d);
   return rc;
}

int main(void)
{
   printf("wfe-gate-reject: ");
   char target[WFE_ID_LEN];

   /* (a) opt-in + valid on_fail -> RETRY, target filled. */
   wfe_def_t *d = parse(WF_RETRY);
   target[0] = '\0';
   assert(wfe_gate_reject_target(d, "g", target, sizeof target) == WFE_GATE_REJECT_RETRY);
   assert(strcmp(target, "impl") == 0);
   wfe_def_free(d);

   /* (b) no opt-in -> TERMINAL, target untouched. */
   d = parse(WF_NOOPT);
   target[0] = '\0';
   assert(wfe_gate_reject_target(d, "g", target, sizeof target) == WFE_GATE_REJECT_TERMINAL);
   assert(target[0] == '\0');
   wfe_def_free(d);

   /* (c) opt-in but no on_fail edge -> TERMINAL. */
   d = parse(WF_NOEDGE);
   assert(wfe_gate_reject_target(d, "g", target, sizeof target) == WFE_GATE_REJECT_TERMINAL);
   wfe_def_free(d);

   /* (d) opt-in but on_fail target does not resolve -> TERMINAL (no dangling stage). */
   d = parse(WF_DANGLING);
   assert(wfe_gate_reject_target(d, "g", target, sizeof target) == WFE_GATE_REJECT_TERMINAL);
   wfe_def_free(d);

   /* (e) unknown gate id -> TERMINAL (safe default). */
   d = parse(WF_RETRY);
   assert(wfe_gate_reject_target(d, "nope", target, sizeof target) == WFE_GATE_REJECT_TERMINAL);

   /* (f) NULL def / NULL gate id -> ERR. */
   assert(wfe_gate_reject_target(NULL, "g", target, sizeof target) == WFE_GATE_REJECT_ERR);
   assert(wfe_gate_reject_target(d, NULL, target, sizeof target) == WFE_GATE_REJECT_ERR);

   /* (g) out_target too small to hold "impl" -> ERR (refuse rather than mis-route). */
   char tiny[3];
   assert(wfe_gate_reject_target(d, "g", tiny, sizeof tiny) == WFE_GATE_REJECT_ERR);
   wfe_def_free(d);

   /* ---- Generic loop cap: accessors ---- */
   /* (h) explicit params are read back; the default node has no params -> defaults. */
   d = parse(WF_CAP_OK);
   const wfe_node_t *g = wfe_def_node(d, "g");
   const wfe_node_t *impl = wfe_def_node(d, "impl");
   assert(g && impl);
   assert(wfe_node_max_iters(g) == 5);
   assert(wfe_node_on_max(g) == WFE_ON_MAX_FAIL);
   assert(wfe_node_max_iters(impl) == WFE_DEFAULT_MAX_ITERS); /* no params -> default */
   assert(wfe_node_on_max(impl) == WFE_ON_MAX_HUMAN);         /* no params -> default */
   assert(wfe_node_max_iters(NULL) == WFE_DEFAULT_MAX_ITERS); /* NULL-safe */
   assert(wfe_node_on_max(NULL) == WFE_ON_MAX_HUMAN);
   wfe_def_free(d);

   /* (i) on_max "pass" parses to WFE_ON_MAX_PASS. */
   d = parse(WF_CAP_PASS_OK);
   assert(wfe_node_on_max(wfe_def_node(d, "g")) == WFE_ON_MAX_PASS);
   wfe_def_free(d);

   /* ---- Generic loop cap: validator ---- */
   char verr[256];
   /* (j) a good cap validates. */
   assert(validate_yaml(WF_CAP_OK, verr, sizeof verr) == 0);
   assert(validate_yaml(WF_CAP_PASS_OK, verr, sizeof verr) == 0);
   /* (k) max_iters <= 0 rejected. */
   assert(validate_yaml(WF_CAP_BADITERS, verr, sizeof verr) == -1);
   assert(strstr(verr, "max_iters") != NULL);
   /* (l) unknown on_max rejected. */
   assert(validate_yaml(WF_CAP_BADONMAX, verr, sizeof verr) == -1);
   assert(strstr(verr, "on_max") != NULL);
   /* (m) on_max:pass without a forward edge rejected. */
   assert(validate_yaml(WF_CAP_PASS_NOEDGE, verr, sizeof verr) == -1);
   assert(strstr(verr, "on_max:pass") != NULL);

   printf("ok\n");
   return 0;
}
