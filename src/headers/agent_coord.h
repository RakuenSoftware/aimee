#ifndef DEC_AGENT_COORD_H
#define DEC_AGENT_COORD_H 1

#include "agent_types.h"
#include "agent_tasks.h"
#include "db1.h"

/* File reference resolution: @path/to/file syntax in delegate prompts */
#define FILE_REF_MAX_REFS 3           /* max references resolved per prompt */
#define FILE_REF_MAX_SIZE (10 * 1024) /* max bytes read per file (10 KB) */
#define FILE_REF_PATH_MAX 512         /* max path length in an @reference */

/* Resolve @path/to/file references in a delegate prompt string.
 * Each @path is replaced with the file's content in a fenced block.
 * Paths outside project_root or that do not exist produce inline error markers.
 * At most FILE_REF_MAX_REFS references are resolved; excess get explicit
 * unresolved markers.
 * Files larger than FILE_REF_MAX_SIZE are truncated with a [TRUNCATED] marker.
 * Caller must free() the returned string. Returns NULL only on allocation failure. */
char *resolve_file_references(const char *prompt, const char *project_root);

/* Delegate token budget: limit system prompt size before dispatching.
 * Default budget is 20K tokens (~80K chars). Progressive truncation priority:
 *   1. injected file content ("--- @path ---" blocks from resolve_file_references)
 *   2. context/background sections ("# Relevant Context", "# Pre-loaded" blocks)
 *   3. injected skill content (after "### ACTIVE SKILL:" marker)
 *   4. generic tail truncation
 * Core task description is always preserved (minimum 2K tokens). */
#define DELEGATE_TOKEN_BUDGET_DEFAULT  20000
#define DELEGATE_TOKEN_BUDGET_MIN_TASK 2000

/* Apply a token budget to a delegate system prompt.
 * Returns a newly allocated string (caller must free), or NULL on failure.
 * Adds [TRUNCATED] markers where content is cut. If prompt fits within
 * budget, returns a copy unchanged. Uses progressive truncation: file
 * references first, then context sections, then skill content, then tail. */
char *delegate_prompt_limit(const char *prompt, int token_budget);

/* Load delegate_token_budget from the global per-project config:
 *   ~/.config/aimee/projects/<name>/project.yaml
 * Supports per-role overrides via "delegate_token_budget_<role>: N" keys.
 * If role is non-NULL, checks for a role-specific budget first.
 * Returns the configured value, or DELEGATE_TOKEN_BUDGET_DEFAULT if not set. */
int delegate_token_budget_load(const char *project_root, const char *role);

int agent_coordinate(agent_config_t *cfg, const char *task, agent_result_t *out);
int agent_vote(agent_config_t *cfg, const char *role, const char *prompt, int n_voters,
               agent_result_t *out);
/* Check whether a tool call would violate any active hard directive in DB2.
 * Returns -1 with reason_out filled on a likely violation, 0 otherwise. Reads
 * go through db2_rules_list. */
int directive_check_tool(const char *tool_name, const char *args_json, char *reason_out,
                         size_t reason_len);

#endif /* DEC_AGENT_COORD_H */
