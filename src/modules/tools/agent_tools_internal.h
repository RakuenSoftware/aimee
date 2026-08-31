/* posix/agent_tools_internal.h: helpers shared between the POSIX tool
 * implementations (agent_tools.c) and the tool dispatcher
 * (agent_tools_dispatch.c). Not a public API. */
#ifndef DEC_POSIX_AGENT_TOOLS_INTERNAL_H
#define DEC_POSIX_AGENT_TOOLS_INTERNAL_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "slop_detect.h"

struct cJSON;

void agent_tools_effect_reset(void);
int agent_tools_effect_classification(const char *name, int (*classifier)(const char *, int *));
int agent_tools_effect_mcp_failure_is_timeout(const char *error);
void agent_tools_effect_propose(const char *name, struct cJSON *args, int classification);
int agent_tools_effect_validate_and_execute(const char *name, struct cJSON *args,
                                            int classification);
int agent_tools_effect_postcondition_pending(void);
int agent_tools_effect_result_claims_success(const char *result);
int agent_tools_effect_verify_file_postcondition(const char *name, struct cJSON *args,
                                                 const char *dispatch_cwd);
void agent_tools_effect_record_postcondition(int passed, const char *detail);
void agent_tools_effect_finish(const char *verdict, const char *reason);

static inline int agent_tools_cmd_refers_to_readonly_root(const char *cmd, const char *ro,
                                                          const char *rw)
{
   if (!cmd || !ro || !ro[0])
      return 0;

   size_t ro_len = strlen(ro);
   size_t rw_len = rw ? strlen(rw) : 0;
   for (const char *p = cmd; (p = strstr(p, ro)); p++)
   {
      int before_ok = (p == cmd) || p[-1] == '\'' || p[-1] == '"' || p[-1] == '=' || p[-1] == ':' ||
                      p[-1] == ',' || p[-1] == '(' || p[-1] == '[' || p[-1] == '{' ||
                      p[-1] == '<' || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n';
      int after_ok = p[ro_len] == '/' || p[ro_len] == '\'' || p[ro_len] == '"' ||
                     p[ro_len] == ':' || p[ro_len] == ',' || p[ro_len] == ')' || p[ro_len] == ']' ||
                     p[ro_len] == '}' || p[ro_len] == '>' || p[ro_len] == ' ' ||
                     p[ro_len] == '\t' || p[ro_len] == '\n' || p[ro_len] == '\0';
      int under_rw =
          rw_len && strncmp(p, rw, rw_len) == 0 && (p[rw_len] == '/' || p[rw_len] == '\0');
      if (before_ok && after_ok && !under_rw)
         return 1;
   }
   return 0;
}

/* Record a write to `path` in the current turn's file_snapshot, if any.
 * Returns the snapshot id on success, 0 if no snapshot is active. */
int64_t auto_snapshot_record(const char *path);

/* Append a markdown-formatted slop advisory to `result` and return the
 * combined string. Caller owns the returned buffer; `result` may be NULL.
 * When `slop` is empty, returns a copy of `result` unchanged. */
char *append_write_slop_advisory(const char *result, const slop_finding_t *slop, int nslop);

/* Resolve `path` against the current thread's run cwd: absolute paths pass
 * through, relative paths are prefixed with run_cmd_get_cwd() into `buf`.
 * Returns `path` or `buf`. Defined in agent_tools.c. */
const char *path_in_thread_cwd(const char *path, char *buf, size_t buf_len);

#endif /* DEC_POSIX_AGENT_TOOLS_INTERNAL_H */
