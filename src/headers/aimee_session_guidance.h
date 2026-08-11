/* aimee_session_guidance.h: THE standing guidance aimee gives an agent.
 *
 * ONE definition. Every transport -- CLI SessionStart, MCP, and the gateway --
 * injects exactly this, once, at the start of a session. There is no per-transport
 * variant and no switch to turn it off: an agent that cannot see aimee's tools
 * does not use them, so disabling this disables aimee.
 *
 * WHY THIS FILE EXISTS. The text used to be written out twice -- once in
 * cli_session_start.c and once in ingress_preinject.c -- and the two copies had
 * already drifted apart. The CLI copy was missing memory_get AND the entire
 * fix-scope line, so a CLI session got strictly weaker guidance than a gateway
 * one, silently. Two copies of a "standing policy" is a contradiction in terms;
 * this is the one copy.
 *
 * Header-only on purpose. A .c file would have to be added to BOTH build systems
 * that describe the client (src/Makefile's CLIENT_SUPPORT_OBJS and the CMake
 * `aimee` target), and forgetting the second one fails only on Windows CI -- a
 * trap this repo has already been caught by. String literals in a header cost
 * nothing and cannot be half-linked.
 */
#ifndef DEC_AIMEE_SESSION_GUIDANCE_H
#define DEC_AIMEE_SESSION_GUIDANCE_H 1

#include "agent_code_capabilities.h"

/* Names aimee's retrieval tools so a co-registered agent fills a gap THROUGH
 * aimee -- symbol-scoped and graph-aware -- instead of raw-grepping the tree. */
#define AIMEE_GUIDANCE_EXPLORE_WITH_LINE                                                           \
   "explore-with: " AIMEE_CODE_TOOL_FIND_SYMBOL ", lsp_references, "                               \
   AIMEE_CODE_TOOL_AST_GREP_SEARCH ", " AIMEE_CODE_TOOL_INDEX                                      \
   " command=" AIMEE_CODE_INDEX_COMMAND_HYBRID ", get_context_block, memory_get\n"

/* The scope policy. explore-with names the tools; it does not say WHEN one
 * matters, and a list alone does not get reached for. Measured on t08_traversal
 * at n=3, every arm 0/3: the ticket names one function, the hidden test asserts
 * on two SIBLING functions carrying the identical unsafe join. aimee called index
 * investigate and preview_blast_radius and got a correct "dependents: []" --
 * siblings are not callers, so no dependency tool can reach them.
 * ast_grep_search was in explore-with the whole time and was called in zero
 * cells. Naming the situation is what makes the tool reachable. */
#define AIMEE_GUIDANCE_FIX_SCOPE_LINE                                                              \
   "fix-scope: a defect that is a PATTERN (unsafe join, missing check, raw "                       \
   "concatenation) usually repeats where nothing calls it -- callers and "                         \
   "blast-radius will correctly report nothing; match the shape with "                             \
   AIMEE_CODE_TOOL_AST_GREP_SEARCH " before reporting done\n"

/* The whole standing block, in the order an agent reads it. */
#define AIMEE_GUIDANCE_BLOCK AIMEE_GUIDANCE_EXPLORE_WITH_LINE AIMEE_GUIDANCE_FIX_SCOPE_LINE

#endif /* DEC_AIMEE_SESSION_GUIDANCE_H */
