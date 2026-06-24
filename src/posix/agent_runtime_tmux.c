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
         /* Bound session, no delegation: a persistent per-session pane keyed
          * "<sid>-cli". Honor the agent's session_reuse: a webchat primary turn
          * (session_reuse=1) keeps the conversation pane alive across turns; a
          * stateless caller that binds a unique per-request session id with
          * session_reuse=0 (the /v1/messages CLI ingress) gets a one-shot pane
          * that is torn down on completion, so unique-id panes don't accumulate. */
         sess_name = cli_session_make_name(aimee_sid, "cli");
         reuse = agent->session_reuse;
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

   /* Honour the agent's model: append `--model <model>` (claude and codex both
    * accept it) unless the launch command already pins one. The model is the
    * config default or the per-request override the chat worker wrote onto the
    * agent — without this the CLI would launch with its own built-in default. */
   const char *base_cmd = agent->cli_cmd[0] ? agent->cli_cmd : "claude";
   const char *kind = agent->cli_kind[0] ? agent->cli_kind : agent->name;
   /* Exact "claude" or a "claude-*" variant (claude-code / claude-oauth) — NOT a
    * substring match: this gates --dangerously-skip-permissions and the config
    * seeding, so it must not fire for an unrelated agent named e.g. "claudette". */
   int is_claude = strcmp(kind, "claude") == 0 || strncmp(kind, "claude-", 7) == 0;
   char cli_cmd_buf[CLI_SESSION_CMD_MAX];
   const char *cli_cmd = base_cmd;
   int need_model = agent->model[0] && !strstr(base_cmd, "--model") && !strstr(base_cmd, " -m ");
   /* Autonomous = bypass claude's interactive permission prompts (there is no
    * human at the detached tmux pane to answer them; aimee's own guardrails are
    * the safety layer). Driven by the global `autonomous` config — EXCEPT a
    * primary webchat turn (a real bound session, not a delegate), which is
    * always autonomous: it is the interactive UI a user is waiting on, and a
    * non-autonomous claude would wedge forever on its first tool prompt. */
   int deleg = deleg_id && deleg_id[0];
   int webchat_primary = have_session && !deleg;
   int autonomous = agent->autonomous || webchat_primary;
   /* Pass --dangerously-skip-permissions only when autonomous, and only if the
    * launch command doesn't already select a permission mode. */
   int need_skip = is_claude && autonomous && !strstr(base_cmd, "--dangerously-skip-permissions") &&
                   !strstr(base_cmd, "--permission-mode");
   if (need_model || need_skip)
   {
      snprintf(cli_cmd_buf, sizeof(cli_cmd_buf), "%s%s%s%s", base_cmd,
               need_model ? " --model " : "", need_model ? agent->model : "",
               need_skip ? " --dangerously-skip-permissions" : "");
      cli_cmd = cli_cmd_buf;
   }

   /* Prefer the turn's bound cwd (the client workspace root on a detached
    * thin-client turn, where the tmux session actually runs); fall back to the
    * server process cwd for a co-located turn that did not bind one. */
   char cwd[MAX_PATH_LEN] = {0};
   const char *turn_cwd = run_cmd_get_cwd();
   if (turn_cwd && turn_cwd[0])
      snprintf(cwd, sizeof(cwd), "%s", turn_cwd);
   else if (getcwd(cwd, sizeof(cwd)) == NULL)
      cwd[0] = '\0';

   /* Seed claude-code's first-run gates (onboarding / per-folder trust for this
    * worktree, + the bypass warning when autonomous) so its interactive TUI
    * starts at the prompt instead of wedging the pane. Best-effort; no-op for
    * other CLIs. */
   if (is_claude)
      cli_session_prepare_claude(cwd, autonomous);

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
      if (recv_rc == -3 || recv_rc == -4)
      {
         /* -3 cancelled (steering/interrupt); -4 provider error/retry past the
          * grace. In both, recv already sent the interrupt key, so the CLI stopped
          * with the conversation intact — KEEP a reused pane alive (a later turn
          * reuses it); only free this turn's scratch. A one-shot pane is torn
          * down. -3 ends quietly (the worker sees agent_request_cancelled); -4 is
          * a real failure → surface a clear provider-error message. */
         if (reuse)
         {
            free(sess.baseline);
            sess.baseline = NULL;
            free(sess.stream_emitted);
            sess.stream_emitted = NULL;
         }
         else
            cli_session_destroy(&sess);
         if (recv_rc == -4)
            snprintf(out->error, sizeof(out->error),
                     "%s CLI hit a provider error and kept failing on retry (try again)",
                     agent->name);
         else
            snprintf(out->error, sizeof(out->error), "turn cancelled");
         return -1;
      }
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
