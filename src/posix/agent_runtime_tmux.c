/* posix/agent_runtime_tmux.c: tmux-backed CLI execution for POSIX agents. */

#include "aimee.h"
#include "agent.h"
#include "cli_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* run_cmd cwd control (util.c): the active turn binds the thread-local working
 * directory to the (client) workspace root. On a detached thin-client turn the
 * tmux session runs on the client, so the session must be created in the
 * client's cwd, not the server's process cwd. */
extern const char *run_cmd_get_cwd(void);

int agent_execute_cli_session(const agent_t *agent, const agent_network_t *network,
                              const char *system_prompt, const char *user_prompt, int max_tokens,
                              double temperature, agent_result_t *out)
{
   (void)network;
   (void)max_tokens;
   (void)temperature;

   char *sess_name;
   if (agent->session_reuse)
      sess_name = cli_session_make_name(agent->name, "shared");
   else
   {
      static volatile int s_counter = 0;
      char tmp[CLI_SESSION_NAME_MAX];
      snprintf(tmp, sizeof(tmp), "aimee-%s-%d-%d", agent->name, (int)getpid(),
               __sync_fetch_and_add(&s_counter, 1));
      sess_name = strdup(tmp);
   }
   if (!sess_name)
   {
      snprintf(out->error, sizeof(out->error), "out of memory");
      return -1;
   }

   const char *cli_cmd = agent->cli_cmd[0] ? agent->cli_cmd : "claude";

   /* Prefer the turn's bound cwd (the client workspace root on a detached
    * thin-client turn, where the tmux session actually runs); fall back to the
    * server process cwd for a co-located turn that did not bind one. */
   char cwd[MAX_PATH_LEN] = {0};
   const char *turn_cwd = run_cmd_get_cwd();
   if (turn_cwd && turn_cwd[0])
      snprintf(cwd, sizeof(cwd), "%s", turn_cwd);
   else if (getcwd(cwd, sizeof(cwd)) == NULL)
      cwd[0] = '\0';

   cli_session_t sess;
   int rc = cli_session_create(&sess, sess_name, cli_cmd, cwd, agent->session_reuse);
   free(sess_name);
   if (rc != 0)
   {
      snprintf(out->error, sizeof(out->error), "failed to create tmux session for %s", agent->name);
      return -1;
   }

   size_t plen = (system_prompt ? strlen(system_prompt) : 0) + strlen(user_prompt) + 4;
   char *full_prompt = malloc(plen);
   if (!full_prompt)
   {
      cli_session_destroy(&sess);
      snprintf(out->error, sizeof(out->error), "out of memory");
      return -1;
   }
   if (system_prompt && system_prompt[0])
      snprintf(full_prompt, plen, "%s\n\n%s", system_prompt, user_prompt);
   else
      snprintf(full_prompt, plen, "%s", user_prompt);

   if (cli_session_send(&sess, full_prompt) != 0)
   {
      free(full_prompt);
      cli_session_destroy(&sess);
      snprintf(out->error, sizeof(out->error), "failed to send prompt to tmux session");
      return -1;
   }
   free(full_prompt);

   char *raw = malloc(CLI_SESSION_BUF_MAX);
   if (!raw)
   {
      cli_session_destroy(&sess);
      snprintf(out->error, sizeof(out->error), "out of memory");
      return -1;
   }
   /* Bound the receive: a CLI stuck in a provider retry loop (e.g. an Anthropic
    * outage) animates its pane forever without the session dying, so the
    * stability heuristic alone would hang indefinitely. Prefer the explicit
    * per-CLI response timeout, then the agent timeout, then the default. */
   int recv_timeout_ms =
       agent->cli_idle_timeout_ms > 0
           ? agent->cli_idle_timeout_ms
           : (agent->timeout_ms > 0 ? agent->timeout_ms : AGENT_DEFAULT_TIMEOUT_MS);
   int recv_rc = cli_session_recv(&sess, raw, CLI_SESSION_BUF_MAX, recv_timeout_ms);
   if (recv_rc != 0)
   {
      free(raw);
      cli_session_destroy(&sess); /* kill the (possibly wedged) session */
      if (recv_rc == -2)
         snprintf(out->error, sizeof(out->error),
                  "%s CLI did not respond within %ds (provider may be unavailable)", agent->name,
                  recv_timeout_ms / 1000);
      else
         snprintf(out->error, sizeof(out->error), "tmux session closed before %s responded",
                  agent->name);
      return -1;
   }

   char *clean = cli_session_strip_ansi(raw);
   free(raw);

   if (!agent->session_reuse)
      cli_session_destroy(&sess);

   if (!clean || !clean[0])
   {
      free(clean);
      snprintf(out->error, sizeof(out->error), "empty response from tmux session");
      return -1;
   }

   out->response = clean;
   out->success = 1;
   out->turns = 1;
   return 0;
}
