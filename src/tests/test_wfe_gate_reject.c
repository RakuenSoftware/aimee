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

static wfe_def_t *parse(const char *yaml)
{
   char err[256] = "";
   wfe_def_t *d = wfe_def_parse(yaml, err, sizeof err);
   assert(d && "parse failed");
   return d;
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

   printf("ok\n");
   return 0;
}
