/* cmd_agent_delegate_toolset.c: split from cmd_agent_delegate.c into a real translation unit
 * (was cmd_agent_delegate_toolset.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "cmd_agent_delegate_internal.h"
#include "aimee.h"
#include "agent.h"
#include "log.h"
#include "db1_client/db1.h"
#include "otel.h"
#include "platform_path.h"
#include "platform_process.h"
#include "commands.h"
#include "cmd_agent_delegate_impl.h"
#include "prompts.h"
#include "persona.h"
#include "role_templates.h"
#include <aimee/skills/skill.h>
#include "events.h"
#include "agent_coord.h"
#include <aimee/delegates/delegate_role.h>
#include <aimee/delegates/delegate_plan.h>
#include <aimee/delegates/delegate_launch.h>
#include <aimee/delegates/delegate_economics.h>
#include "modules/memory/memory_platform.h"
#include <aimee/workspace/workspace.h>
#include "guardrails.h"
#include <aimee/delegates/delegate_source_authority.h>
#include "toolset.h"
#include "liveness.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *delegate_toolset_override_arg(const opt_parsed_t *opts, int argc, char **argv,
                                          const char **prompt_io)
{
   const char *toolset = opt_get(opts, "tools");
   if (toolset && !toolset[0])
   {
      for (int i = 0; i + 1 < argc; i++)
      {
         if (!argv[i] || !argv[i + 1])
            continue;
         if (strcmp(argv[i], "--tools") == 0 && i + 1 < argc)
         {
            char resolved[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX], err[TOOLSET_ERROR_MAX] = "";
            if (toolset_resolve_effective(argv[i + 1], resolved, TOOLSET_MAX_TOOLS, err,
                                          sizeof(err)) >= 0)
            {
               toolset = argv[i + 1];
               break;
            }
         }
      }
   }
   if (toolset && toolset[0] && *prompt_io && strcmp(*prompt_io, toolset) == 0)
      *prompt_io = opt_pos(opts, 2);
   return toolset;
}
