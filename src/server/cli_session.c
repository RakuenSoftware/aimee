/* PTY session lifecycle and credential-free native runtime homes. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "aimee_home.h"
#include <ctype.h>
#include "cJSON.h"
#include "cli_session.h"
#include "platform_path.h"
#include "util.h"
#include "modules/workspace/workspace_provider.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char *sess_run(const char *cmd, int *exit_code)
{
   const workspace_provider_t *ws = workspace_provider_active();
   if (ws && ws->kind == WS_PROVIDER_DETACHED && ws->exec_shell)
      return ws->exec_shell(ws, cmd, exit_code);
   return run_cmd(cmd, exit_code);
}

static unsigned long djb2(const char *s)
{
   unsigned long h = 5381;
   while (*s)
      h = h * 31 + (unsigned char)*s++;
   return h;
}

static void msleep_ms(int ms)
{
   struct timespec ts;
   ts.tv_sec = ms / 1000;
   ts.tv_nsec = (ms % 1000) * 1000000L;
   nanosleep(&ts, NULL);
}

void cli_session_set_isolated_home(cli_session_t *s, const char *home)
{
   if (!s)
      return;
   free(s->iso_home);
   s->iso_home = (home && home[0]) ? strdup(home) : NULL;
}

int cli_session_is_alive(const cli_session_t *s)
{
   char cmd[CLI_SESSION_NAME_MAX + 64];
   snprintf(cmd, sizeof(cmd), "tmux has-session -t '%s' 2>/dev/null", s->session_name);
   int rc;
   char *out = sess_run(cmd, &rc);
   free(out);
   return rc == 0;
}

static int cli_runtime_vendor_base(const char *vendor, char *out, size_t n)
{
   const char *base = getenv("AIMEE_OAUTH_RUNTIME_DIR");
   int written;
   if (base && base[0] == '/')
      written = snprintf(out, n, "%s/sessions/%s", base, vendor);
   else if ((base = getenv("XDG_RUNTIME_DIR")) && base[0] == '/')
      written = snprintf(out, n, "%s/aimee/oauth-sessions/%s", base, vendor);
   else
      written = snprintf(out, n, "/tmp/aimee-runtime-%lu/oauth-sessions/%s",
                         (unsigned long)getuid(), vendor);
   if (written < 0 || (size_t)written >= n)
      return -1;
   if (platform_mkdir_p(out, 0700) != 0 && access(out, F_OK) != 0)
      return -1;
   return chmod(out, 0700);
}

static int cli_write_file_atomic(const char *path, const char *data)
{
   char tmp[PATH_MAX];
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.aimee.tmp.%d", path, (int)getpid()) >= sizeof(tmp))
      return -1;
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
   if (fd < 0)
      return -1;
   FILE *f = fdopen(fd, "wb");
   if (!f)
   {
      close(fd);
      unlink(tmp);
      return -1;
   }
   size_t len = strlen(data);
   int ok = (fwrite(data, 1, len, f) == len);
   if (fclose(f) != 0)
      ok = 0;
   if (!ok || rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

static int cli_home_unlink_cb(const char *p, const struct stat *st, int type, struct FTW *ftw)
{
   (void)st;
   (void)type;
   (void)ftw;
   remove(p);
   return 0;
}

static void cli_rmrf(const char *path)
{
   nftw(path, cli_home_unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
}

int cli_session_create(cli_session_t *s, const char *session_name, const char *cli_cmd,
                       const char *work_dir, int reuse)
{
   memset(s, 0, sizeof(*s));
   snprintf(s->session_name, CLI_SESSION_NAME_MAX, "%s", session_name);
   snprintf(s->cli_cmd, CLI_SESSION_CMD_MAX, "%s", cli_cmd);
   s->reuse = reuse;

   /* If reuse is set and session exists, attach to it */
   if (reuse && cli_session_is_alive(s))
   {
      s->active = 1;
      s->last_activity = time(NULL);
      return 0;
   }

   /* Create new tmux session running the CLI directly rather than via an
    * interactive shell prompt. That avoids a race where the first user
    * message lands before the CLI is fully attached and also keeps shell
    * prompts out of captured output.
    *
    * Non-login interactive Bash loads ~/.bashrc on every new seat without
    * /etc/profile replacing the inherited PATH (including npm-installed CLIs).
    * -c executes the CLI directly, so no prompt can race the first message. */
   /* shell_quote() returns a complete shell token. Leaving the command unquoted
    * worked only while cli_cmd was a single
    * word ("claude"): a multi-word command (the AIMEE_SESSION_ID=<sid> stamp, or
    * --model/--dangerously-skip-permissions flags) gets word-split by the outer
    * shell, tmux re-joins the pieces, and the pane's `sh -c` ends up executing
    * just the leading env assignment — exiting 0 instantly and taking the tmux
    * server down with it ("failed to send prompt to tmux session"). */
   char create_cmd[1024];
   char *esc_cli = shell_quote(cli_cmd && cli_cmd[0] ? cli_cmd : "claude");
   if (work_dir && work_dir[0])
   {
      char *esc_dir = shell_quote(work_dir);
      snprintf(create_cmd, sizeof(create_cmd),
               "tmux new-session -d -s '%s' -x 220 -y 50 -c %s /bin/bash -ic %s 2>/dev/null",
               session_name, esc_dir, esc_cli);
      free(esc_dir);
   }
   else
   {
      snprintf(create_cmd, sizeof(create_cmd),
               "tmux new-session -d -s '%s' -x 220 -y 50 /bin/bash -ic %s 2>/dev/null",
               session_name, esc_cli);
   }
   free(esc_cli);
   int rc;
   char *out = sess_run(create_cmd, &rc);
   free(out);
   if (rc != 0)
      return -1;

   /* Wait for the CLI to render AND settle — not just for the first byte. claude's
    * fresh-session welcome box renders progressively and rotates its placeholder
    * tips; pasting the first prompt mid-render races the input handler, so the
    * keystrokes get eaten and the turn comes back as the (static) welcome banner —
    * the user has to resend until the session warms up. Waiting for two identical
    * captures means the baseline snapshot and the first submit both land on a
    * settled, input-ready composer. Capped (~10s) so a CLI that animates a startup
    * spinner forever still proceeds. */
   unsigned long prev_hash = 0;
   int stable = 0;
   for (int i = 0; i < 20; i++)
   {
      msleep_ms(500);
      char cap_cmd[256];
      snprintf(cap_cmd, sizeof(cap_cmd), "tmux capture-pane -p -t '%s' 2>/dev/null", session_name);
      char *cap = sess_run(cap_cmd, &rc);
      if (cap && strlen(cap) > 2)
      {
         unsigned long h = djb2(cap);
         if (h == prev_hash)
         {
            if (++stable >= 2) /* two consecutive identical captures = settled */
            {
               free(cap);
               break;
            }
         }
         else
         {
            stable = 0;
            prev_hash = h;
         }
      }
      free(cap);
   }

   s->active = 1;
   s->last_activity = time(NULL);
   return 0;
}

void cli_session_destroy(cli_session_t *s)
{
   /* Reclaim the isolated claude HOME the moment the seat tears down, so homes
    * track the delegate lifecycle instead of piling up until the 1h age sweep.
    * Unconditional (before the active/alive early-returns): a home may have been
    * minted for a seat whose tmux session already died. cli_rmrf uses FTW_PHYS, so
    * the symlinked shared credential/settings targets are never followed. */
   if (s->iso_home)
   {
      cli_rmrf(s->iso_home);
      free(s->iso_home);
      s->iso_home = NULL;
   }
   if (!s->active)
      return;
   if (!cli_session_is_alive(s))
   {
      s->active = 0;
      return;
   }
   char cmd[CLI_SESSION_NAME_MAX + 64];
   snprintf(cmd, sizeof(cmd), "tmux kill-session -t '%s' 2>/dev/null", s->session_name);
   int rc;
   char *out = sess_run(cmd, &rc);
   free(out);
   s->active = 0;
}

char *cli_session_make_name(const char *agent_name, const char *role)
{
   unsigned long h = djb2(role ? role : "default");
   char *name = malloc(CLI_SESSION_NAME_MAX);
   if (!name)
      return NULL;
   snprintf(name, CLI_SESSION_NAME_MAX, "aimee-%s-%08lx", agent_name, h & 0xffffffff);
   /* Sanitize: the session name is interpolated into single-quoted tmux shell
    * commands ('%s') throughout this file, so beyond the chars tmux rejects in a
    * session name we must also neutralize anything that could break out of the
    * quoting (single quote, backslash, control/shell metacharacters). The name is
    * derived from the aimee session id, which can be client-influenced, so this
    * is the single chokepoint that keeps every tmux sink injection-safe. Keep
    * only [A-Za-z0-9_-]; map everything else to '-'. */
   for (char *p = name; *p; p++)
   {
      unsigned char c = (unsigned char)*p;
      int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
      if (!ok)
         *p = '-';
   }
   return name;
}

int cli_session_native_home(const char *vendor, const char *cwd, const char *sid,
                            const char *binding, const char *sdk_identity, char *home,
                            size_t capacity, cli_session_t *cleanup)
{
   if (!vendor || (strcmp(vendor, "claude") && strcmp(vendor, "codex")) ||
       (!strcmp(vendor, "claude") && (!sdk_identity || strlen(sdk_identity) != 64 ||
                                      strspn(sdk_identity, "0123456789abcdef") != 64)) ||
       (!strcmp(vendor, "codex") && sdk_identity) || !sid || !sid[0] || strlen(sid) >= 128 ||
       strspn(sid, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
           strlen(sid) ||
       !binding || strlen(binding) != 64 || strspn(binding, "0123456789abcdef") != 64 || !home ||
       !capacity || !cleanup)
      return -1;
   home[0] = 0;
   char base[PATH_MAX], candidate[PATH_MAX], directory[PATH_MAX], history[PATH_MAX], link[PATH_MAX];
   if (cli_runtime_vendor_base(vendor, base, sizeof base) ||
       snprintf(candidate, sizeof candidate, "%s/n.XXXXXX", base) >= (int)sizeof candidate ||
       strlen(candidate) >= capacity || !mkdtemp(candidate))
      return -1;
   snprintf(home, capacity, "%s", candidate);
   cli_session_set_isolated_home(cleanup, home);
   const char *state = !strcmp(vendor, "claude") ? "projects" : "sessions";
   if (snprintf(directory, sizeof directory, "%s/.%s", home, vendor) >= (int)sizeof directory ||
       mkdir(directory, 0700) ||
       snprintf(history, sizeof history, "%s/browser-%s/%s/%s/%s", aimee_home(), vendor, sid,
                binding, state) >= (int)sizeof history ||
       platform_mkdir_p(history, 0700) ||
       snprintf(link, sizeof link, "%s/%s", directory, state) >= (int)sizeof link ||
       symlink(history, link))
      goto failed;
   if (!strcmp(vendor, "claude"))
   {
      cJSON *root = cJSON_CreateObject();
      cJSON *projects = root ? cJSON_AddObjectToObject(root, "projects") : NULL;
      cJSON *project = projects && cwd && cwd[0] ? cJSON_AddObjectToObject(projects, cwd) : NULL;
      if (!root || !projects || !cJSON_AddStringToObject(root, "userID", sdk_identity) ||
          !cJSON_AddBoolToObject(root, "hasCompletedOnboarding", 1) ||
          (cwd && cwd[0] &&
           (!project || !cJSON_AddBoolToObject(project, "hasTrustDialogAccepted", 1))))
      {
         cJSON_Delete(root);
         goto failed;
      }
      char *json = cJSON_PrintUnformatted(root);
      cJSON_Delete(root);
      int rc = snprintf(link, sizeof link, "%s/.claude.json", directory) >= (int)sizeof link ||
               !json || cli_write_file_atomic(link, json);
      free(json);
      if (rc)
         goto failed;
   }
   return 0;
failed:
   cli_session_destroy(cleanup);
   home[0] = 0;
   return -1;
}
