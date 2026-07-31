/* config_elements.c -- copy ONE element of a config array out to the caller.
 *
 * Split out of config.c, which crossed the 2500-line hard limit when these were
 * added. They belong together: every one is the same three lines over a
 * different array, and none of them touches the loader, the snapshot, or the
 * save path that fills the rest of that file.
 */
#include <stddef.h>
#include <string.h>

#include "config.h"

/* Copy ONE element of a config array out to the caller.
 *
 * The per-member accessors (config_cron_job_id(i), ...) suit a caller that
 * wants one field. They do not suit one that passes the whole element onward —
 * the trigger scheduler hands a trigger_rule_t to source callbacks, and the
 * cron runner hands a cron_job_t to the executor. Those callers previously did
 *   const cron_job_t *job = &cfg.cron_jobs[i];
 * which required holding a config_t purely to reach the element.
 *
 * The ELEMENT types stay public on purpose: cron_job_t and trigger_rule_t are
 * shared domain types (db1/cron_jobs.h uses cron_job_t with no config
 * involved). config_t is the secret here, not them.
 *
 * Returns 0 and fills |out| on success; -1 for a bad index or unreadable
 * config, leaving |out| untouched so a caller that ignores the return value
 * gets its own zeroed struct rather than stale data. */
/* One aux-task route, copied out. Same rationale as config_cron_job_at: the
 * per-member accessors suit a caller wanting one field, not one rendering the
 * whole element. */
int config_aux_task_at(int index, config_aux_task_t *out)
{
   if (!out || index < 0 || index >= CONFIG_AUX_MAX_TASKS)
      return -1;
   return config_field_read(offsetof(config_t, aux_tasks) + (size_t)index * sizeof(*out),
                            sizeof(*out), out);
}

/* One per-model concurrency override, copied out. Named type already existed
 * (config_concurrency_entry_t), so this is the plain config_cron_job_at shape. */
int config_concurrency_per_model_at(int index, config_concurrency_entry_t *out)
{
   if (!out || index < 0 || index >= CONFIG_CONCURRENCY_MAX_ENTRIES)
      return -1;
   return config_field_read(
       offsetof(config_t, concurrency_per_model) + (size_t)index * sizeof(*out), sizeof(*out), out);
}

/* One LSP server entry, copied out. The element type is declared INSIDE config_t
 * (struct config_lsp_server) but has a name, so this works -- unlike aux_tasks,
 * which had to be named first. */
int config_lsp_server_at(int index, config_lsp_server_t *out)
{
   if (!out || index < 0 || index >= 8)
      return -1;
   return config_field_read(offsetof(config_t, lsp_servers) + (size_t)index * sizeof(*out),
                            sizeof(*out), out);
}

int config_cron_job_at(int index, cron_job_t *out)
{
   if (!out || index < 0 || index >= CRON_JOBS_MAX)
      return -1;
   return config_field_read(offsetof(config_t, cron_jobs) + (size_t)index * sizeof(*out),
                            sizeof(*out), out);
}

int config_trigger_rule_at(int index, trigger_rule_t *out)
{
   if (!out || index < 0 || index >= TRIGGER_RULES_MAX)
      return -1;
   return config_field_read(offsetof(config_t, trigger_rules) + (size_t)index * sizeof(*out),
                            sizeof(*out), out);
}

int config_mcp_client_at(int index, config_mcp_client_t *out)
{
   if (!out || index < 0 || index >= CONFIG_MCP_MAX_CLIENTS)
      return -1;
   return config_field_read(offsetof(config_t, mcp_clients) + (size_t)index * sizeof(*out),
                            sizeof(*out), out);
}

/* The generator skips struct-array members whose type is a typedef'd enum (it
 * matches base C types only), which is why dispositions[].source has no
 * generated accessor while .name and .value do. Same reason config_mcp_client_at
 * is hand-written. */
int config_disposition_source(int index)
{
   int v = 0;
   if (index < 0 || index >= CONFIG_MAX_DISPOSITIONS)
      return 0;
   config_field_read(offsetof(config_t, dispositions) +
                         (size_t)index * sizeof(((config_t *)0)->dispositions[0]) +
                         offsetof(config_disposition_t, source),
                     sizeof(v), &v);
   return v;
}
