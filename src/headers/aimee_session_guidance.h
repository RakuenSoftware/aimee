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
 * READING CODE was the gap this line failed to close, and it turned out to be a
 * naming problem rather than a missing capability. `index command=span` resolves
 * to code_span_get, which returns the span CONTENT plus a source_version hash --
 * actual code, not a file:line pointer -- and `index` is on the shown surface, so
 * it costs ONE call. The guidance instead named get_context_block, which is not
 * callable by that name at all (it is a member of the `recall` family, and
 * `recall` is not shown either), so the one substitution that matters for "read
 * this file and change it" was the one an agent could not act on. */
/* THE SURFACE MATTERS MORE THAN THE TOOL NAME, and this line used to get that
 * backwards. It said "call these INSTEAD of a shell command" and named MCP tools
 * only. Every other channel said the same: SKILL.md, and the MCP server
 * instructions ("call them directly, by name"). So an agent did exactly as asked
 * and spent one round trip per lookup.
 *
 * Measured on the benchmark: 13.3 MCP tool calls per cell, 29 total calls against
 * plain Codex's 8.7, and ZERO invocations of the aimee CLI across 13 cells --
 * despite /usr/local/bin/aimee being on PATH the whole time. Round trips are the
 * 3.3x term in the cost gap; tokens per call are only 1.27x.
 *
 * An MCP call cannot be chained: one call, one turn, the whole conversation
 * re-sent. `aimee ...` is an ordinary command, so N of them chain with && inside
 * ONE shell call the agent was already making. Same answers, ~1 round trip
 * instead of N. That is why the CLI leads here and MCP is named as the fallback:
 * not preference, arithmetic. */
#define AIMEE_GUIDANCE_EXPLORE_WITH_LINE                                                           \
   "explore-with: aimee answers CODE questions from its index. When a lookup is "                  \
   "available as a COMMAND, chain it: several `aimee ...` commands joined with && "                \
   "cost ONE round trip, where the same lookups as separate tool calls cost one "                  \
   "turn each. Fold them into a shell call you are already making. "                               \
   "definition -> aimee index find <symbol>; callers of a symbol -> aimee index "                  \
   "callers <symbol>; what else depends on this -> aimee index blast-radius "                      \
   "<file>; a file's shape -> aimee index structure <file>; what was decided "                     \
   "before -> aimee memory search <terms>. "                                                       \
   "read a file or line range -> aimee index span <file> <start> <end>; where do "                 \
   "I even start -> aimee index investigate \"<question>\" (pass several "                         \
   "questions to answer them in one call). "                                                       \
   "a phrase rather than a symbol -> aimee index hybrid \"<phrase>\" (several "                    \
   "phrases in one call; --scope all to widen). "                                                  \
   "Only this one has no command form: a pattern or repeated shape "                               \
   "-> " AIMEE_CODE_TOOL_AST_GREP_SEARCH ". "                                                      \
   "These commands take PLURAL arguments -- several spans, questions or phrases "                  \
   "in one invocation -- so ask once rather than repeating the call. "                             \
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
   "blast-radius will correctly report nothing; match the shape "                                  \
   "with " AIMEE_CODE_TOOL_AST_GREP_SEARCH " before reporting done\n"

/* The whole standing block, in the order an agent reads it. */
#define AIMEE_GUIDANCE_BLOCK AIMEE_GUIDANCE_EXPLORE_WITH_LINE AIMEE_GUIDANCE_FIX_SCOPE_LINE

#endif /* DEC_AIMEE_SESSION_GUIDANCE_H */
