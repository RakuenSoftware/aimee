/* test_memory_fact_gate.c: the typed-fact write gate's pure type validation
 * (typed-fact §1 / P1). */
#include "memory_fact_gate.h"
#include <assert.h>
#include <stdio.h>

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

int main(void)
{
   test_accept_valid_triple();
   test_reject_kind_mismatch();
   test_novel_and_badarg();
   printf("memory_fact_gate: all tests passed\n");
   return 0;
}
