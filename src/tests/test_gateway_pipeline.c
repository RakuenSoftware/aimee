/* test_gateway_pipeline.c: pure tests for the universal-gateway request-stage
 * runner (P2a). The runner sequences stage callbacks over a gw_request_t and never
 * derefs raw/driver/ag, so these tests need no cJSON, no socket, no server. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../headers/gateway_pipeline.h"

#define PASS(name) printf("  PASS: %s\n", (name))

/* A recorder: each stage appends its tag to a shared buffer (proving order) and
 * returns a configurable intervention count. */
typedef struct
{
   char order[16];
   int ret;
   char tag;
} rec_t;

static char g_trace[64];
static size_t g_trace_len;

static int rec_stage(gw_request_t *r, void *ud)
{
   rec_t *rc = (rec_t *)ud;
   (void)r;
   if (g_trace_len + 1 < sizeof(g_trace))
      g_trace[g_trace_len++] = rc->tag;
   return rc->ret;
}

static void reset_trace(void)
{
   g_trace_len = 0;
   memset(g_trace, 0, sizeof(g_trace));
}

/* Stages run in declared order and intervention counts sum. */
static void test_order_and_sum(void)
{
   rec_t a = {.ret = 2, .tag = 'a'}, b = {.ret = 3, .tag = 'b'}, c = {.ret = 0, .tag = 'c'};
   gw_request_t r = {.raw = NULL, .parity = 1};
   gw_stage_t stages[] = {{rec_stage, &a, "a"}, {rec_stage, &b, "b"}, {rec_stage, &c, "c"}};

   reset_trace();
   int total = gw_pipeline_run_request(&r, stages, 3);
   assert(total == 5);                  /* 2 + 3 + 0 */
   assert(strcmp(g_trace, "abc") == 0); /* declared order preserved */
   PASS("order_and_sum");
}

/* A stage returning <0 short-circuits: later stages do not run, and the negative
 * is propagated (a half-altered request must not proceed). */
static void test_short_circuit_on_error(void)
{
   rec_t a = {.ret = 1, .tag = 'a'}, b = {.ret = -7, .tag = 'b'}, c = {.ret = 1, .tag = 'c'};
   gw_request_t r = {.raw = NULL};
   gw_stage_t stages[] = {{rec_stage, &a, "a"}, {rec_stage, &b, "b"}, {rec_stage, &c, "c"}};

   reset_trace();
   int rv = gw_pipeline_run_request(&r, stages, 3);
   assert(rv == -7);                   /* first negative propagated */
   assert(strcmp(g_trace, "ab") == 0); /* 'c' never ran */
   PASS("short_circuit_on_error");
}

/* NULL/empty stage lists and NULL fn slots are no-ops, not crashes. */
static void test_null_and_empty(void)
{
   gw_request_t r = {.raw = NULL};
   rec_t a = {.ret = 4, .tag = 'a'};
   gw_stage_t with_hole[] = {{NULL, NULL, "hole"}, {rec_stage, &a, "a"}};

   assert(gw_pipeline_run_request(&r, NULL, 0) == 0);
   assert(gw_pipeline_run_request(NULL, with_hole, 2) == 0);

   reset_trace();
   int total = gw_pipeline_run_request(&r, with_hole, 2); /* skips the NULL fn */
   assert(total == 4);
   assert(strcmp(g_trace, "a") == 0);
   PASS("null_and_empty");
}

/* The IR carries derived state for stages to read (parity/serving_api/stream). */
static void test_ir_fields_visible(void)
{
   gw_request_t r = {.raw = NULL, .serving_api = GW_API_OPENAI, .parity = 0, .stream = 1};
   assert(r.serving_api == GW_API_OPENAI);
   assert(r.parity == 0);
   assert(r.stream == 1);
   PASS("ir_fields_visible");
}

int main(void)
{
   printf("test_gateway_pipeline:\n");
   test_order_and_sum();
   test_short_circuit_on_error();
   test_null_and_empty();
   test_ir_fields_visible();
   printf("ALL PASS\n");
   return 0;
}
