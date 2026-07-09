/* memory_redirect.h: intercept an agent's local memory-file writes and redirect
 * them into the central aimee-server store (P3 of central agent-memory
 * interception). Called as a stage in pre_tool_check after tool-name
 * canonicalization. See docs/proposals/pending/central-agent-memory-interception.md.
 */
#ifndef DEC_MEMORY_REDIRECT_H
#define DEC_MEMORY_REDIRECT_H 1

#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      MR_ALLOW = 0,    /* not a memory operation — let it proceed */
      MR_REDIRECT = 1, /* memory write: store centrally, deny the raw tool */
      MR_REJECT = 2    /* MEMORY.md / unsupported: deny, change nothing */
   } mr_verdict_t;

   /* PURE classification (no I/O) — testable. Given the hook client (NULL/""
    * => "claude"), the canonical tool name ("Write"/"Edit"/...), the target file
    * path, and the HOME dir (the path is anchored under <home>/.claude/projects/),
    * decide the verdict. On MR_REDIRECT, out_name receives the memory entry name
    * (relpath under the memory dir, no ".md", no ".." segments). out_reason
    * (optional) receives a static human message for MR_REJECT. */
   mr_verdict_t memory_redirect_classify(const char *client, const char *tool, const char *path,
                                         const char *home, char *out_name, size_t name_cap,
                                         const char **out_reason);

   /* PURE (no I/O) — testable. Best-effort: does the Bash `command` WRITE to a
    * file under the client's memory surface? Looks for a memory path (under
    * <home>/<projects_root>/.../<memory_seg>/...md) that is the target of a write
    * operator (>, >>, tee, sed -i, dd of=, truncate, cp/mv) in the same simple
    * command. A read of a memory file (no preceding write op) returns 0. v1
    * limit (best-effort): an interpreter writing the file (python -c, node -e,
    * ruby -e), process substitution, eval, or var-indirection can evade — the
    * deferred inotify/fanotify backstop is the complete fix. */
   int memory_redirect_bash_targets_memory(const char *client, const char *command,
                                           const char *home);

   /* Full interception stage for pre_tool_check. Inspects a parsed tool-input
    * object for a memory write/edit; on a memory op performs the redirect (stores
    * the content into aimee's db1 as a private, non-recallable archive row — the
    * .md is retired and never materialized) and returns a pre_tool_check verdict:
    * 0 = allow, 2 = deny (msg holds the agent-facing reason). Fail-open: if the
    * store is unreachable it spills for reconcile and returns 0 (allow) so the
    * agent is never blocked by our outage.
    *
    * project_hint: when non-empty, the project key to store under, resolved by
    * the caller (the thin client, where the real cwd / git repo / AIMEE_PROJECT_ID
    * live). This is REQUIRED for a remote server, whose filesystem has neither the
    * client's cwd nor its git repo; when NULL/empty the project is resolved from
    * cwd as a local-server fallback. */
   int memory_redirect_check(const char *tool, cJSON *root, const char *cwd,
                             const char *project_hint, char *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_REDIRECT_H */
