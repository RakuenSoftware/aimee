/* test_memory_fact_gate.c: the typed-fact write gate's pure type validation
 * (typed-fact §1 / P1). */
#include "modules/memory/memory_fact_gate.h"
#include <aimee/memory/module_api.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_accept_valid_triple(void)
{
   const rel_type_def_t *m = NULL;
   /* PERSON works_for ORG — valid. */
   assert(memory_fact_gate_check(NODE_PERSON, "works_for", NODE_ORG, &m) == FACT_GATE_ACCEPT);
   assert(m != NULL && m->correction_behavior == CORR_SUPERSEDE);
   /* DEVICE device_has_ip IP — valid. */
   assert(memory_fact_gate_check(NODE_DEVICE, "device_has_ip", NODE_IP, NULL) == FACT_GATE_ACCEPT);
   /* PERSON age SCALAR — value-typed object valid. */
   assert(memory_fact_gate_check(NODE_PERSON, "age", NODE_SCALAR, NULL) == FACT_GATE_ACCEPT);
   /* PERSON spouse PERSON — symmetric valid; normalization applies. */
   assert(memory_fact_gate_check(NODE_PERSON, "Spouse", NODE_PERSON, NULL) == FACT_GATE_ACCEPT);
   printf("  PASS: test_accept_valid_triple\n");
}

static void test_reject_kind_mismatch(void)
{
   /* "the printer works_for the kernel" — DEVICE works_for ORG: head kind wrong. */
   assert(memory_fact_gate_check(NODE_DEVICE, "works_for", NODE_ORG, NULL) ==
          FACT_GATE_REJECT_KIND);
   /* PERSON works_for PERSON: tail kind wrong. */
   assert(memory_fact_gate_check(NODE_PERSON, "works_for", NODE_PERSON, NULL) ==
          FACT_GATE_REJECT_KIND);
   /* PERSON device_has_ip IP: head should be DEVICE. */
   assert(memory_fact_gate_check(NODE_PERSON, "device_has_ip", NODE_IP, NULL) ==
          FACT_GATE_REJECT_KIND);
   printf("  PASS: test_reject_kind_mismatch\n");
}

static void test_novel_and_badarg(void)
{
   const rel_type_def_t *m = (const rel_type_def_t *)0x1;
   assert(memory_fact_gate_check(NODE_PERSON, "frobnicates", NODE_ORG, &m) == FACT_GATE_NOVEL);
   assert(m == NULL); /* matched cleared on novel */
   assert(memory_fact_gate_check(NODE_PERSON, "", NODE_ORG, NULL) == FACT_GATE_BADARG);
   assert(memory_fact_gate_check(NODE_PERSON, NULL, NODE_ORG, NULL) == FACT_GATE_BADARG);
   printf("  PASS: test_novel_and_badarg\n");
}

/* --- the module seam ------------------------------------------------------
 * With a checker registered the verdict comes from the memory module, not the
 * local seed table. These tests stand in for that module with a recorder, so
 * they prove the seam forwards the triple unchanged and reports the answer it
 * gets back — including refusing to invent one when the module cannot answer. */

static struct
{
   int calls;
   memory_node_kind_t head_kind;
   memory_node_kind_t tail_kind;
   char rel_type[64];
   int null_rel_type;
   int verdict;
   int fail;
} g_checker_state;

static int recording_checker(memory_node_kind_t head_kind, const char *rel_type,
                             memory_node_kind_t tail_kind, int *verdict)
{
   g_checker_state.calls++;
   g_checker_state.head_kind = head_kind;
   g_checker_state.tail_kind = tail_kind;
   g_checker_state.null_rel_type = rel_type == NULL;
   snprintf(g_checker_state.rel_type, sizeof(g_checker_state.rel_type), "%s",
            rel_type ? rel_type : "");
   if (g_checker_state.fail)
      return -1;
   *verdict = g_checker_state.verdict;
   return 0;
}

static void test_registered_checker_decides(void)
{
   memset(&g_checker_state, 0, sizeof(g_checker_state));
   memory_fact_gate_register_checker(recording_checker);

   /* A triple the local table would ACCEPT: the checker's answer wins, so a
    * seam that quietly kept deciding locally cannot pass this. */
   g_checker_state.verdict = FACT_GATE_REJECT_KIND;
   const rel_type_def_t *m = (const rel_type_def_t *)0x1;
   assert(memory_fact_gate_check(NODE_PERSON, "works_for", NODE_ORG, &m) == FACT_GATE_REJECT_KIND);
   assert(g_checker_state.calls == 1);
   assert(g_checker_state.head_kind == NODE_PERSON && g_checker_state.tail_kind == NODE_ORG);
   assert(strcmp(g_checker_state.rel_type, "works_for") == 0);
   /* matched still comes from the local seed table — the module cannot return
    * a pointer into it, and the commit path reads it. */
   assert(m != NULL && m->correction_behavior == CORR_SUPERSEDE);

   /* NOVEL/BADARG leave matched NULL, exactly as the local path does. */
   g_checker_state.verdict = FACT_GATE_NOVEL;
   m = (const rel_type_def_t *)0x1;
   assert(memory_fact_gate_check(NODE_PERSON, "works_for", NODE_ORG, &m) == FACT_GATE_NOVEL);
   assert(m == NULL);

   /* An empty or missing label still reaches the checker; the module returns
    * BADARG for it, so the gate must not short-circuit and skip the call. */
   g_checker_state.verdict = FACT_GATE_BADARG;
   assert(memory_fact_gate_check(NODE_PERSON, NULL, NODE_ORG, NULL) == FACT_GATE_BADARG);
   assert(g_checker_state.null_rel_type == 1);

   memory_fact_gate_register_checker(NULL);
   printf("  PASS: test_registered_checker_decides\n");
}

static void test_checker_failure_defers(void)
{
   memset(&g_checker_state, 0, sizeof(g_checker_state));
   g_checker_state.fail = 1;
   memory_fact_gate_register_checker(recording_checker);
   /* A triple the local table would ACCEPT: DEFER proves the gate did not fall
    * back to deciding it locally, which would make a broken module invisible. */
   assert(memory_fact_gate_check(NODE_PERSON, "works_for", NODE_ORG, NULL) == FACT_GATE_DEFER);
   assert(g_checker_state.calls == 1);

   /* Unregistering restores the local seed table. */
   memory_fact_gate_register_checker(NULL);
   assert(memory_fact_gate_check(NODE_PERSON, "works_for", NODE_ORG, NULL) == FACT_GATE_ACCEPT);
   assert(g_checker_state.calls == 1);
   printf("  PASS: test_checker_failure_defers\n");
}

/* The Go stage decodes these offsets independently; pinning them here is what
 * makes the two halves one contract rather than two guesses. */
static void test_gate_wire_layout(void)
{
   uint8_t buf[AIMEE_MEMORY_GATE_REQUEST_LEN];
   assert(aimee_memory_gate_request_encode(NODE_PERSON, "works_for", NODE_ORG, buf, sizeof(buf)) ==
          0);
   assert(aimee_memory_get_u32(buf) == AIMEE_MEMORY_GATE_REQUEST_MAGIC);
   assert(aimee_memory_get_u32(buf + 4) == AIMEE_MEMORY_WIRE_VERSION);
   assert(aimee_memory_get_u32(buf + 8) == (uint32_t)NODE_PERSON);
   assert(aimee_memory_get_u32(buf + 12) == (uint32_t)NODE_ORG);
   assert(buf[16] == 9 && buf[17] == 0 && buf[18] == 0 && buf[19] == 0);
   assert(memcmp(buf + 20, "works_for", 9) == 0);

   /* A NULL label encodes as length 0 rather than failing, so the module can
    * answer BADARG for it instead of the transport reporting a failure. */
   assert(aimee_memory_gate_request_encode(NODE_PERSON, NULL, NODE_ORG, buf, sizeof(buf)) == 0);
   assert(buf[16] == 0 && buf[17] == 0);

   /* Over-long labels are refused, never truncated: a truncated label would
    * normalize to a different name and could match a different seed row. */
   char over_long[AIMEE_MEMORY_REL_TYPE_MAX + 2];
   memset(over_long, 'a', sizeof(over_long) - 1);
   over_long[sizeof(over_long) - 1] = '\0';
   assert(aimee_memory_gate_request_encode(NODE_PERSON, over_long, NODE_ORG, buf, sizeof(buf)) ==
          -1);
   over_long[AIMEE_MEMORY_REL_TYPE_MAX] = '\0'; /* exactly at the bound: accepted */
   assert(aimee_memory_gate_request_encode(NODE_PERSON, over_long, NODE_ORG, buf, sizeof(buf)) ==
          0);

   uint8_t response[AIMEE_MEMORY_GATE_RESPONSE_LEN];
   aimee_memory_fact_verdict_t verdict;
   aimee_memory_put_u32(response, AIMEE_MEMORY_GATE_RESPONSE_MAGIC);
   aimee_memory_put_u32(response + 4, FACT_GATE_NOVEL);
   assert(aimee_memory_gate_response_decode(response, sizeof(response), &verdict) == 0);
   assert((int)verdict == FACT_GATE_NOVEL);
   /* A verdict outside the pure-gate range is a broken module, not a DEFER to
    * pass along: rejected here so the adapter reports failure. */
   aimee_memory_put_u32(response + 4, FACT_GATE_DEFER);
   assert(aimee_memory_gate_response_decode(response, sizeof(response), &verdict) == -1);
   aimee_memory_put_u32(response, AIMEE_MEMORY_GATE_RESPONSE_MAGIC + 1);
   aimee_memory_put_u32(response + 4, FACT_GATE_ACCEPT);
   assert(aimee_memory_gate_response_decode(response, sizeof(response), &verdict) == -1);
   printf("  PASS: test_gate_wire_layout\n");
}

int main(void)
{
   test_accept_valid_triple();
   test_reject_kind_mismatch();
   test_novel_and_badarg();
   test_registered_checker_decides();
   test_checker_failure_defers();
   test_gate_wire_layout();
   printf("memory_fact_gate: all tests passed\n");
   return 0;
}
