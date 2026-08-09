/* sandbox_learned.c: reach the learned-toolchain module over the event bus.
 * See sandbox_learned.h.
 *
 * The apt parser and the JSON sidecar used to live here. They are feature work --
 * lexing an untrusted shell command and owning a store -- and the C core carries
 * messages rather than performing feature work, so they moved to
 * server-go/modules/sandbox and are reached as bus stages.
 *
 * What remains is the part that is genuinely C's: the cheap prefilter that keeps
 * an incidental "apt" substring free, the config gate, and resolving a working
 * directory's git root. */

#include "sandbox_learned.h"

#include "aimee.h" /* MAX_PATH_LEN */
#include "cJSON.h"
#include "config.h"     /* config_delegate_sandbox_learn_packages */
#include "guardrails.h" /* git_repo_root */

#include "headers/module_json_call.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/sandbox/module_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Learning is best-effort and off the critical path; a module that is slow must
 * not hold a delegate turn open. */
#define SANDBOX_CALL_TIMEOUT_MS 5000
/* A learned set is at most SBX_LEARN_MAX short names, so the reply is small.
 * The request carries one shell command. */
#define SANDBOX_CALL_MAX_BODY (256u * 1024u)

/* One request/response round trip. Returns a parsed reply the caller must delete,
 * or NULL on any failure (unattached module, transport error, bad reply).
 *
 * Learning is best-effort, so every failure is the same non-event here: the
 * outcome is deliberately not inspected. Consumers that must tell an unreachable
 * module from a failed one pass a result pointer instead. */
static cJSON *sandbox_call(uint32_t event_kind, uint32_t stage_id, cJSON *payload)
{
   return aimee_module_json_call(event_kind, stage_id, payload, SANDBOX_CALL_MAX_BODY,
                                 SANDBOX_CALL_TIMEOUT_MS, NULL);
}

int sandbox_learned_load(const char *git_root, char out[][SBX_PKG_MAX], int max)
{
   if (!git_root || !git_root[0] || !out || max <= 0)
      return 0;
   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return 0;
   cJSON_AddStringToObject(payload, "git_root", git_root);

   cJSON *reply = sandbox_call(AIMEE_SANDBOX_EVENT_LOAD, AIMEE_SANDBOX_STAGE_LOAD, payload);
   if (!reply)
      return 0;

   int n = 0;
   const cJSON *packages = cJSON_GetObjectItemCaseSensitive(reply, "packages");
   if (cJSON_IsArray(packages))
   {
      const cJSON *item;
      cJSON_ArrayForEach(item, packages)
      {
         if (n >= max)
            break;
         if (cJSON_IsString(item) && item->valuestring[0])
            snprintf(out[n++], SBX_PKG_MAX, "%s", item->valuestring);
      }
   }
   cJSON_Delete(reply);
   return n;
}

void sandbox_learned_observe(const char *cwd, const char *cmd)
{
   if (!cwd || !cwd[0] || !cmd || !cmd[0])
      return;
   /* Cheap pre-filter, and it has to earn its keep here more than it did before.
    *
    * The old in-process version parsed FIRST -- the parse was pure and cheap --
    * and only then paid for the config read and the git subprocess. The parser
    * lives in the module now, so this side cannot cheaply know whether a command
    * really installs anything, and resolving the git root forks a process.
    *
    * Requiring BOTH substrings restores that property without reimplementing the
    * parser: the module accepts a command only when the command word is exactly
    * apt/apt-get AND the install subcommand is present, so anything this rejects
    * the parser would also have rejected. An incidental "adapter" no longer forks
    * git. */
   if (!strstr(cmd, "apt") || !strstr(cmd, "install"))
      return;
   if (!config_delegate_sandbox_learn_packages())
      return;

   char git_root[MAX_PATH_LEN];
   if (git_repo_root(cwd, git_root, sizeof(git_root)) != 0)
      return;

   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return;
   cJSON_AddStringToObject(payload, "git_root", git_root);
   cJSON_AddStringToObject(payload, "command", cmd);

   /* Fire and forget: what was learned is the module's to report, and a failure
    * here must never surface in a delegate turn. */
   cJSON *reply = sandbox_call(AIMEE_SANDBOX_EVENT_OBSERVE, AIMEE_SANDBOX_STAGE_OBSERVE, payload);
   cJSON_Delete(reply);
}
