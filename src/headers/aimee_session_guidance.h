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

/* Fills a code gap THROUGH aimee -- symbol-scoped and graph-aware -- instead of
 * raw-grepping the tree.
 *
 * NAMING THE TOOLS WAS NOT ENOUGH. This used to be a bare comma-separated list,
 * `explore-with: find_symbol, lsp_references, ...`. Measured on CT 403 with the
 * list demonstrably delivered (the model quoted it back on request): a gateway
 * cell still made ZERO aimee calls by MCP or CLI and did all eight of its steps
 * with find/cat/sed/grep. A list of unfamiliar names loses to a shell the model
 * already knows how to drive.
 *
 * So each tool is stated as the SUBSTITUTION it makes, against the exact command
 * it displaces. The agent is not being asked to learn a toolbox before it starts;
 * it is being told which of its existing reflexes has a better answer here. The
 * pairing is the whole point -- "find_symbol" means nothing to a model reaching
 * for grep, whereas "grep for a definition -> find_symbol" is actionable at the
 * moment the reflex fires. */
/* EVERY NAME HERE MUST BE IN MCP_CORE_TOOLS. scripts/check_guidance_tool_parity.py
 * fails the build otherwise, because this drifted and nothing noticed.
 *
 * It named lsp_references, get_context_block and memory_get. All three ARE
 * registered in mcp_tool_table -- the command table covers them -- but none is in
 * the PRESENTATION core, so reaching one costs find_tools -> describe_tool ->
 * call_tool. mcp_tool_profile.c records the measurement twenty lines from that
 * list: agents handed a tool at that price used a recursive text search instead.
 * A tool the agent cannot afford to reach is a tool it does not have.
 *
 * Two lists are both called "core", which is how it hid: get_context_block is
 * marked native="core,review_indexed" in mcp_tool_table -- aimee's OWN agents'
 * toolset -- while absent from MCP_CORE_TOOLS, what an external client is shown.
 *
 * Confirmed behaviourally before the fix: told to use aimee, the model called
 * memory_recall -- the name that IS shown -- not the memory_get named here. It was
 * routing around the advice.
 *
 * get_context_block is the omission that matters and is NOT fixed by editing this
 * string: it is the only tool returning CODE rather than a file:line pointer, so
 * without it a "read this file and change it" task needs a shell read regardless.
 * Promoting it into the shown surface changes what aimee presents by default and
 * has to be decided as such. */
#define AIMEE_GUIDANCE_EXPLORE_WITH_LINE                                                           \
   "explore-with: for CODE questions call these INSTEAD of a shell command -- "                    \
   "grep/rg for a definition -> " AIMEE_CODE_TOOL_FIND_SYMBOL                                      \
   "; grep for a pattern or a repeated shape -> " AIMEE_CODE_TOOL_AST_GREP_SEARCH                  \
   "; find/ls, or any search that is not a symbol name -> " AIMEE_CODE_TOOL_INDEX                  \
   " command=" AIMEE_CODE_INDEX_COMMAND_HYBRID "; what else depends on this -> "                   \
   AIMEE_CODE_TOOL_PREVIEW_BLAST_RADIUS "; what was decided before -> memory_recall. "             \
   "Shell stays right for building, running tests, and editing.\n"

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
