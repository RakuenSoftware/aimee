/* test_tool_output_cap.c: unit tests for the configurable tool-output cap.
 *
 * Exercises agent_tool_output_cap_clamp() — the pure, header-inline clamp half
 * of agent_tool_output_cap() that maps a configured tool_output_max_bytes value
 * to an effective per-result MODEL-VISIBLE cap. This is the single source of
 * truth for the clamp, so testing it pins the resolver semantics with zero
 * linkage. */
#include <assert.h>
#include <stdio.h>
#include "aimee.h"
#include "agent_exec.h"

#define PASS(name) printf("  PASS: %s\n", name)

static void test_constants(void)
{
   /* Default raised to 32 KB; never exceeds the raw capture safety buffer. */
   assert(AGENT_TOOL_OUTPUT_MAX == 32 * 1024);
   assert(AGENT_TOOL_OUTPUT_RAW_MAX == 32 * 1024);
   PASS("constants");
}

static void test_unset_is_default(void)
{
   /* 0 == unset -> built-in default (32768). */
   assert(agent_tool_output_cap_clamp(0) == (size_t)AGENT_TOOL_OUTPUT_MAX);
   assert(agent_tool_output_cap_clamp(0) == 32768);
   PASS("unset_is_default");
}

static void test_negative_is_default(void)
{
   /* Defensive: a negative value also falls back to the default. */
   assert(agent_tool_output_cap_clamp(-1) == 32768);
   assert(agent_tool_output_cap_clamp(-100000) == 32768);
   PASS("negative_is_default");
}

static void test_lower_value_honored(void)
{
   /* An operator may set the cap LOWER than the default. */
   assert(agent_tool_output_cap_clamp(4096) == 4096);
   assert(agent_tool_output_cap_clamp(16384) == 16384);
   assert(agent_tool_output_cap_clamp(1) == 1);
   PASS("lower_value_honored");
}

static void test_clamp_to_raw_max(void)
{
   /* A value above the 32 KB raw safety buffer clamps down to it. */
   assert(agent_tool_output_cap_clamp(32768) == 32768);
   assert(agent_tool_output_cap_clamp(32769) == 32768);
   assert(agent_tool_output_cap_clamp(1000000) == (size_t)AGENT_TOOL_OUTPUT_RAW_MAX);
   PASS("clamp_to_raw_max");
}

int main(void)
{
   printf("test_tool_output_cap:\n");
   test_constants();
   test_unset_is_default();
   test_negative_is_default();
   test_lower_value_honored();
   test_clamp_to_raw_max();
   printf("All tool-output-cap tests passed.\n");
   return 0;
}
