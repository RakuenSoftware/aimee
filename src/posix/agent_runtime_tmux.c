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

/* delegation_active_id is provided by server_compute.c at link time (a
 * thread-local: the id of the delegate running on THIS thread, or NULL for a
 * primary turn). agent_tools_dispatch.c supplies a weak NULL stub so binaries
 * that don't link the server (CLI, tests) still resolve it. Read it here — not
 * the AIMEE_PARENT_DELEGATION_ID env var, which is process-global and races
 * across concurrent delegate threads. */
const char *delegation_active_id(void);

int agent_execute_cli_session(const agent_t *agent, const agent_network_t *network,
                              const char *system_prompt, const char *user_prompt, int max_tokens,
                              double temperature, agent_result_t *out)
{
   (void)network;
   (void)max_tokens;
   (void)temperature;

   /* Key the tmux session on the aimee session id so concurrent sessions can
    * never paste into, capture from, or kill (on a recv timeout) each other's
    * pane. The bound session id is the webchat/turn session via
    * session_id_set_override; cli_session_make_name embeds it literally and
    * sanitizes chars tmux rejects.
    *
    * A delegate runs concurrently with its siblings under the SAME session id
    * (delegate_run_ctx_enter binds the originating session for all of them), so
    * session id alone would collapse a fan-out back onto one pane. Add the
    * delegation id: primary turn -> one persistent pane per session
    * ("<sid>-cli"); each delegate -> its own pane ("<sid>-<deleg>"). The
    * primary pane reuses (the conversation persists across turns); a delegate
    * pane is one-shot and torn down on completion (isolation is the unique
    * name, not reuse) so unique-per-delegation panes don't accumulate — there
    * is no idle reaper for these sessions.
    *
    * The per-session pane is used ONLY when a real session id is bound on this
    * thread (session_id_override_active). Without an override, session_id()
    * returns the process-wide PPID fallback — the SAME value for every
    * override-less turn in the server — which would collapse them all onto one
    * "<ppid>-cli" pane and cross-contaminate. In that case fall through to the
    * agent-keyed / unique-per-turn names below. */
   const char *aimee_sid = session_id();
   const char *deleg_id = delegation_active_id();
   int have_session = aimee_sid && aimee_sid[0] && session_id_override_active();
   int reuse = agent->session_reuse;
   char *sess_name;
   if (have_session)
   {
      if (deleg_id && deleg_id[0])
      {
         sess_name = cli_session_make_name(aimee_sid, deleg_id);
         reuse = 0;
      }
      else
      {
         sess_name = cli_session_make_name(aimee_sid, "cli");
         reuse = 1;
      }
   }
   else if (agent->session_reuse)
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
   int rc = cli_session_create(&sess, sess_name, cli_cmd, cwd, reuse);
   free(sess_name);
   if (rc != 0)
   {
      snprintf(out->error, sizeof(out->error), "failed to create tmux session for %s", agent->name);
      return -1;
   }
   /* cli_kind drives the TUI response parser (claude ●/❯/✻ vs codex •/›). */
   cli_session_set_kind(&sess, agent->cli_kind[0] ? agent->cli_kind : agent->name);

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

   /* Snapshot the pane immediately before sending so recv returns ONLY this
    * turn's reply — never prior turns still visible on a reused pane. */
   cli_session_mark_baseline(&sess);

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

   /* recv already returned this turn's clean, chrome-stripped response. */
   char *clean = strdup(raw);
   free(raw);

   /* Tear the pane down for a one-shot (delegate / non-reuse) session; for a
    * reused chat pane keep it alive but free this turn's per-turn scratch
    * (baseline + stream buffer) so it does not leak across turns. */
   if (!reuse)
      cli_session_destroy(&sess);
   else
   {
      free(sess.baseline);
      sess.baseline = NULL;
      free(sess.stream_emitted);
      sess.stream_emitted = NULL;
   }

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
