/* server_seed_config.c: split from server.c into a real translation unit
 * (was server_seed_config.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_internal.h"
#include "aimee.h"
#include "harness_memory_audit.h"  /* hmem_audit */
#include "harness_memory_common.h" /* hmem_resolve_project / hmem_project_key_ok */
#include "harness_memory_scope.h"  /* hmem_scope_for_client */
#include "json_fluent.h"           /* jo_ok */
#include "memory_redirect.h"       /* memory_redirect_classify / _bash_targets / _rematerialize */
#include "server.h"
#include "turn_registry.h"
#include "server_http.h" /* server_http_api_status_report */
#include "config.h"      /* config_t / config_load for api.status, api.enable */
#include "delegate_backend_docker.h"
#include "delegate_backend_local.h"
#include "delegate_backend_ssh.h"
#include "server_delegate_monitor.h"
#include "server_coord_dispatcher.h"
#include "server_skill.h"
#include "server_compute_impl.h"
#include "skill_review.h"
#include "trigger_scheduler.h"
#include "wfe_live_delegate.h"
#include "wfe_scheduler.h"
#include "server_trigger.h"
#include "server_cron.h"
#include "server_pipeline.h" /* roundtable authoring pipeline (pipeline.*) */
#include "commands.h"
#include "agent.h"
#include "agent_exec.h"     /* agent_audit_async_flush — drain audit queue at shutdown */
#include "webuser_editor.h" /* webuser_editor_shutdown — reap editors at shutdown (WP-I) */
#include "agent_config.h"
#include "provider_catalog.h"
#include "delegate_credentials.h"
#include "model_registry.h"
#include "model_provider.h"
#include "model_registry.h"
#include "db1.h"
#include "token_audit.h"
#include "dashboard.h"
#include "log.h"
#include "hud.h"
#include "platform_event.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "platform_process.h"
#include "util.h"
#include "workspace.h"
#include "worktree_gc.h"
#include "git_verify.h"
#include "toolset.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include "persona.h"        /* persona_install_defaults */
#include "role_templates.h" /* role_template_install_defaults */

/* Seed the built-in personas and delegate role templates into user config as
 * editable files. This is what makes config — not code — the source of truth:
 * once seeded, every persona/role lookup reads the on-disk file (the built-in
 * prose in code only populates the default and is the fallback when a file is
 * missing). Idempotent: existing files are never overwritten, so user edits
 * survive restarts. */
void server_seed_config_defaults(void)
{
   char pdir[MAX_PATH_LEN];
   snprintf(pdir, sizeof(pdir), "%s/personas", config_default_dir());
   if (persona_install_defaults(pdir) < 0)
      LOG_WARN("server", "could not seed default personas in %s", pdir);
   char rdir[MAX_PATH_LEN];
   snprintf(rdir, sizeof(rdir), "%s/role_templates", config_default_dir());
   if (role_template_install_defaults(rdir) < 0)
      LOG_WARN("server", "could not seed default role templates in %s", rdir);
}
