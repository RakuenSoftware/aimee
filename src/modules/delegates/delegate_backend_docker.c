/* delegate_backend_docker.c: see delegate_backend_docker.h.
 *
 * acquire() runs `docker create --name <container> -v <workspace>:
 * /workspace -w /workspace <image> sleep infinity`, then `docker
 * start <container>`. exec() rides `docker exec -i <container> bash
 * -c '<b64-wrapped>'`, captures stdout/stderr/exit_code through pipes.
 * release() does `docker stop` (hibernate=1) or `docker rm -f`
 * (hibernate=0).
 *
 * File ops (read_file/write_file/list_dir) all route through
 * docker_exec — read does `cat <path>` then C-side slices for
 * offset/limit, write b64-encodes content and decodes inside the
 * container, list does `ls -1A` and splits lines. All paths are
 * workspace-relative (anchored at /workspace); absolute paths and
 * '..' segments are rejected.
 *
 * Cwd persistence is partial: set_cwd writes to the local state
 * struct and exec() prefixes `cd <cwd> &&` to subsequent commands
 * when set. */

#include <aimee/delegates/delegate_backend_docker.h>
#include "util.h"

#include "aimee.h"      /* MAX_PATH_LEN */
#include "aimee_home.h" /* aimee_home() — resolves the server's UDS path */

#include <assert.h>

/* Fixed in-container path for the bind-mounted aimee-server UDS. The delegate's
 * only outward channel: `--network none` cuts every IP route, and AIMEE_API_ENDPOINT
 * points the baked-in `aimee` CLI at this socket, so `aimee git_commit`,
 * `aimee web_search`, `aimee memory ...` reach the server (and nothing else). */
#define DELEGATE_SOCK_PATH "/run/aimee/aimee-http.sock"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <aimee/delegates/delegate_launch_args.h>
#include "log.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* RFC 4648 base64 — same as the SSH backend uses. Kept local so this
 * source file stays standalone (no shared b64 header yet). */
static int docker_build_exec_command(const char *container_name, const char *wrapped_script,
                                     int timeout_ms, char **out_cmd)
{
   if (out_cmd)
      *out_cmd = NULL;
   if (!container_name || !container_name[0] || !wrapped_script || !out_cmd)
      return -1;

   size_t script_len = strlen(wrapped_script);
   size_t b64_cap = ((script_len + 2) / 3) * 4 + 1;
   char *b64 = malloc(b64_cap);
   if (!b64)
      return -1;
   if (util_b64_encode(wrapped_script, script_len, b64, b64_cap) < 0)
   {
      free(b64);
      return -1;
   }

   /* Put the deadline inside the container as well as around the docker client.
    * Killing only `sh -c docker exec ...` leaves the exec'd process alive in the
    * container, which is exactly how a cancelled Python tool kept consuming a CPU
    * after its durable job was terminal. GNU timeout owns the inner bash process
    * group and terminates its descendants. Fire it 100ms before the outer backend
    * deadline so docker has time to return the bounded result. The delegate image
    * is Ubuntu and already requires coreutils for the base64 transport. */
   int inner_timeout_ms = timeout_ms > 100 ? timeout_ms - 100 : timeout_ms;
   size_t need = strlen(container_name) + b64_cap + 256;
   char *cmd = malloc(need);
   if (!cmd)
   {
      free(b64);
      return -1;
   }
   int n;
   if (inner_timeout_ms > 0)
      n = snprintf(cmd, need,
                   "docker exec -i %s bash -c \"echo '%s' | base64 -d | "
                   "timeout --signal=TERM --kill-after=1s %d.%03ds bash\"",
                   container_name, b64, inner_timeout_ms / 1000, inner_timeout_ms % 1000);
   else
      n = snprintf(cmd, need, "docker exec -i %s bash -c \"echo '%s' | base64 -d | bash\"",
                   container_name, b64);
   free(b64);
   if (n < 0 || (size_t)n >= need)
   {
      free(cmd);
      return -1;
   }
   *out_cmd = cmd;
   return 0;
}

int delegate_backend_docker_build_exec_command(const char *container_name,
                                               const char *wrapped_script, char **out_cmd)
{
   return docker_build_exec_command(container_name, wrapped_script, 0, out_cmd);
}

/* --- container lifecycle --- */

typedef struct
{
   char container_name[128];
   char image[256];
   char workspace_host[MAX_PATH_LEN]; /* host-side mount point */
   char cwd[MAX_PATH_LEN];            /* in-container cwd, defaults to /workspace */
   /* 1 when workspace_host is the CALLER's real tree rather than our own scratch
    * dir. It changes who the container must run as: files written into a scratch
    * dir we own are ours to clean up, but files written into the user's checkout
    * must not land root-owned. */
   int mount_host_tree;
   int mount_read_only; /* :ro — the tree is not this delegate's to change */
   /* The in-container working directory, and what relative tool paths resolve
    * against. Per-state, not the global resolve_docker_workdir(): a caller-provided
    * tree is mounted at its OWN absolute host path (see docker_build_mounts), so
    * /workspace is not where its files live. Anchoring resolution on a global while
    * the mount moved is how a tool would read the wrong tree and report it as
    * missing. */
   char workdir[MAX_PATH_LEN];
   /* Bind mounts for `docker create`, already in "<src>:<dst>[:ro]" form. */
   char mounts[3][MAX_PATH_LEN * 2 + 8];
   int mount_count;
} docker_state_t;

/* The in-container forwarder the package proxy bridges through. */
#define DELEGATE_PKG_PROXY_URL "http://127.0.0.1:3129"
/* The module's response buffer. The argv points into it, and the last entry is
 * NUL-terminated one byte past the response, so it carries slack. */
#define DELEGATE_LAUNCH_BUF (1u << 18)

#define DOCKER_DEFAULT_IMAGE    "ubuntu:22.04"
#define DOCKER_WORKDIR_DEFAULT  "/workspace"
#define DOCKER_PROBE_TIMEOUT_MS 15000

/* Test seam: production workdir is /workspace inside the container.
 * Unit tests substitute AIMEE_DOCKER_WORKDIR so file ops + mkdir
 * happen under a writable temp dir on the host (the fake-docker
 * fixture runs commands locally, not in a real container). */
static const char *resolve_docker_workdir(void)
{
   const char *override = getenv("AIMEE_DOCKER_WORKDIR");
   if (override && override[0])
      return override;
   return DOCKER_WORKDIR_DEFAULT;
}

/* Pick the docker binary. Tests substitute a fake-docker fixture
 * via AIMEE_DOCKER_BIN. Production runs default to "docker". */
static const char *resolve_docker_bin(void)
{
   const char *override = getenv("AIMEE_DOCKER_BIN");
   if (override && override[0])
      return override;
   return "docker";
}

int delegate_backend_docker_translate_mount_path(const char *container_path,
                                                 const char *mount_table, char *out, size_t outsz)
{
   if (!container_path || container_path[0] != '/' || !mount_table || !out || outsz == 0)
      return -1;

   size_t best_len = 0;
   const char *best_source = NULL;
   size_t best_source_len = 0;
   const char *line = mount_table;
   while (*line)
   {
      const char *end = strchr(line, '\n');
      if (!end)
         end = line + strlen(line);
      const char *tab = memchr(line, '\t', (size_t)(end - line));
      if (tab)
      {
         size_t destination_len = (size_t)(tab - line);
         const char *source = tab + 1;
         size_t source_len = (size_t)(end - source);
         if (source_len && source[source_len - 1] == '\r')
            source_len--;
         if (destination_len > 0 && source_len > 0 && line[0] == '/' && source[0] == '/' &&
             destination_len > best_len && strncmp(container_path, line, destination_len) == 0 &&
             (destination_len == 1 || container_path[destination_len] == '\0' ||
              container_path[destination_len] == '/'))
         {
            best_len = destination_len;
            best_source = source;
            best_source_len = source_len;
         }
      }
      line = *end ? end + 1 : end;
   }

   if (!best_source)
   {
      int n = snprintf(out, outsz, "%s", container_path);
      return n < 0 || (size_t)n >= outsz ? -1 : 0;
   }
   const char *suffix = best_len == 1 ? container_path : container_path + best_len;
   int n = snprintf(out, outsz, "%.*s%s", (int)best_source_len, best_source, suffix);
   return n < 0 || (size_t)n >= outsz ? -1 : 1;
}

/* Docker bind sources are resolved in the daemon's filesystem namespace. When
 * aimee-server itself runs in Docker, aimee_home() names the path INSIDE this
 * container; passing it straight back to the daemon makes a named-volume path
 * look absent, and Docker silently creates a directory at the requested socket
 * source. Inspect this container's mounts and translate the socket to its host
 * source. A host-native server has no inspectable self container, so it keeps the
 * original path. */
static int docker_host_path_for_container(const char *container, const char *container_path,
                                          char *out, size_t outsz)
{
   if (!container || !container[0])
   {
      int n = snprintf(out, outsz, "%s", container_path);
      return n < 0 || (size_t)n >= outsz ? -1 : 0;
   }

   const char *argv[] = {resolve_docker_bin(),
                         "inspect",
                         "--format",
                         "{{range .Mounts}}{{printf \"%s\\t%s\\n\" .Destination .Source}}{{end}}",
                         container,
                         NULL};
   char *mounts = NULL;
   if (safe_exec_capture_cwd_env_timeout(argv, NULL, NULL, &mounts, 1 << 20,
                                         DOCKER_PROBE_TIMEOUT_MS) != 0)
   {
      free(mounts);
      int n = snprintf(out, outsz, "%s", container_path);
      return n < 0 || (size_t)n >= outsz ? -1 : 0;
   }
   int rc = delegate_backend_docker_translate_mount_path(container_path, mounts ? mounts : "", out,
                                                         outsz);
   free(mounts);
   return rc;
}

static int docker_host_path_for_self(const char *container_path, char *out, size_t outsz)
{
   char self[128];
   if (gethostname(self, sizeof(self)) != 0)
      self[0] = '\0';
   self[sizeof(self) - 1] = '\0';
   return docker_host_path_for_container(self, container_path, out, outsz);
}

static int server_runs_in_container(void)
{
   return access("/.dockerenv", F_OK) == 0 || access("/run/.containerenv", F_OK) == 0;
}

/* Build the host-side workspace path: $XDG_CACHE_HOME/aimee/delegate/
 * <task_id>/ (falls back to $HOME/.cache/...). The dir is mounted
 * read/write into the container at DOCKER_WORKDIR so files survive
 * `docker rm` on hibernate=0. */
static int compute_workspace_host(const char *task_id, char *out, size_t outsz)
{
   const char *xdg = getenv("XDG_CACHE_HOME");
   int n;
   if (xdg && xdg[0])
      n = snprintf(out, outsz, "%s/aimee/delegate/%s", xdg, task_id);
   else
   {
      const char *home = getenv("HOME");
      if (!home || !home[0])
         return -1;
      n = snprintf(out, outsz, "%s/.cache/aimee/delegate/%s", home, task_id);
   }
   return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

/* mkdir -p — same shape as the local backend. */
static int docker_mkdir_p(const char *path)
{
   char buf[MAX_PATH_LEN];
   snprintf(buf, sizeof(buf), "%s", path);
   for (char *p = buf + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            return -1;
         *p = '/';
      }
   }
   if (mkdir(buf, 0700) != 0 && errno != EEXIST)
      return -1;
   return 0;
}

/* Run `docker <args>` synchronously, returning the child's exit code.
 * argv must be NULL-terminated. -1 on fork/wait failure. */
static int run_docker(const char *const argv[])
{
   pid_t pid = fork();
   if (pid < 0)
      return -1;
   if (pid == 0)
   {
      /* Replace argv[0] with the resolved docker binary. */
      const char *bin = resolve_docker_bin();
      /* Build a copy of argv with argv[0] set to bin so the test
       * fixture (which checks argv[0] for "docker") still routes
       * through the override. execvp uses the bin path for lookup
       * but argv[0] for the process name. */
      execvp(bin, (char *const *)argv);
      _exit(127);
   }
   int status = 0;
   if (waitpid(pid, &status, 0) != pid)
      return -1;
   return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* True when `name` is one of ours. The container-name prefix is matched HERE
 * rather than with `docker ps --filter name=^aimee-delegate-`: the anchored regex
 * is a Docker-ism, and runtimes that treat the name filter as a literal substring
 * (e.g. the LXC-backed docker shim) match nothing at all, silently turning both
 * cleanup paths into no-ops and leaking a container per stale turn. Filtering in C
 * keeps the strict prefix the anchor was there to enforce, on every runtime. */
static int is_delegate_container_name(const char *name)
{
   return name && strncmp(name, "aimee-delegate-", 15) == 0;
}

/* Accept only a bare container id, so a hostile or malformed name can never reach
 * `docker rm`. */
static int is_container_id(const char *id)
{
   size_t len = id ? strlen(id) : 0;
   if (len < 12 || len > 64)
      return 0;
   for (size_t i = 0; i < len; i++)
      if (!isxdigit((unsigned char)id[i]))
         return 0;
   return 1;
}

int delegate_backend_docker_remove_orphans(void)
{
   const char *list_argv[] = {resolve_docker_bin(), "ps", "-a", "--format",
                              "{{.ID}} {{.Names}}", NULL};
   char *out = NULL;
   if (safe_exec_capture_cwd_env_timeout(list_argv, NULL, NULL, &out, 1 << 20,
                                         DOCKER_PROBE_TIMEOUT_MS) != 0)
   {
      free(out);
      return -1;
   }
   int removed = 0;
   char *save = NULL;
   for (char *line = strtok_r(out, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
   {
      char id[80], name[256];
      if (sscanf(line, "%79s %255s", id, name) != 2)
         continue;
      if (!is_delegate_container_name(name))
         continue;
      if (!is_container_id(id))
      {
         LOG_WARN("delegate-sandbox", "refusing invalid orphan container id from docker: %s", id);
         continue;
      }
      const char *remove_argv[] = {resolve_docker_bin(), "rm", "-f", id, NULL};
      if (run_docker(remove_argv) == 0)
         removed++;
      else
         LOG_WARN("delegate-sandbox", "failed to remove orphan container %s", id);
   }
   free(out);
   return removed;
}

/* Days-from-civil (Hinnant) → UTC epoch seconds for a naive Y-M-D H:M:S.
 * Avoids the non-portable timegm(); the caller applies the TZ offset. */
static time_t docker_utc_epoch(int y, int mo, int d, int h, int mi, int s)
{
   y -= mo <= 2;
   int era = (y >= 0 ? y : y - 399) / 400;
   unsigned yoe = (unsigned)(y - era * 400);
   unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
   unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
   long days = (long)era * 146097L + (long)doe - 719468L;
   return (time_t)days * 86400 + h * 3600 + mi * 60 + s;
}

/* Periodic runtime reap of aged delegate containers.
 *
 * remove_orphans() only runs at startup, and docker_release() only removes the
 * container on the NORMAL turn-completion path. When the heartbeat monitor
 * stale-cancels a delegate job (or the delegate crashes/fails) its running
 * container is left behind — leaking one container per stale/failed turn until the
 * next restart. Over a long uptime these accumulate and fill the image volume.
 *
 * Reap any aimee-delegate-* container older than max_age_secs, chosen ABOVE the
 * in-tool job cap (DEFAULT_IN_TOOL_THRESHOLD_SECS, 1200s) so a live long-running
 * turn is never removed. Returns the count removed, or -1 on a docker error. */
int delegate_backend_docker_reap_aged(int max_age_secs)
{
   const char *list_argv[] = {resolve_docker_bin(), "ps", "--format",
                              "{{.ID}} {{.Names}} {{.CreatedAt}}", NULL};
   char *out = NULL;
   if (safe_exec_capture_cwd_env_timeout(list_argv, NULL, NULL, &out, 1 << 20,
                                         DOCKER_PROBE_TIMEOUT_MS) != 0)
   {
      free(out);
      return -1;
   }
   time_t now = time(NULL);
   int removed = 0;
   char *save = NULL;
   for (char *line = strtok_r(out, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
   {
      /* Line is "<id> <name> " + Go's default CreatedAt: "2006-01-02 15:04:05 -0700 MST". */
      char id[80], name[256];
      int yy, mo, dd, hh, mi, ss;
      char off[8] = "+0000";
      if (sscanf(line, "%79s %255s %d-%d-%d %d:%d:%d %7s", id, name, &yy, &mo, &dd, &hh, &mi, &ss,
                 off) < 8)
         continue;
      if (!is_delegate_container_name(name) || !is_container_id(id))
         continue;
      long off_secs = 0;
      if ((off[0] == '+' || off[0] == '-') && strlen(off) >= 5 && isdigit((unsigned char)off[1]))
      {
         int oh = (off[1] - '0') * 10 + (off[2] - '0');
         int om = (off[3] - '0') * 10 + (off[4] - '0');
         off_secs = (long)(oh * 3600 + om * 60) * (off[0] == '-' ? -1 : 1);
      }
      time_t created = docker_utc_epoch(yy, mo, dd, hh, mi, ss) - off_secs;
      if (created <= 0 || (long)(now - created) < max_age_secs)
         continue;
      const char *rm_argv[] = {resolve_docker_bin(), "rm", "-f", id, NULL};
      if (run_docker(rm_argv) == 0)
         removed++;
      else
         LOG_WARN("delegate-sandbox", "failed to reap aged delegate container %s", id);
   }
   free(out);
   return removed;
}

/* Determine whether the running sandbox `container` was actually isolated by the
 * runtime, i.e. whether `--network none` was honoured. Asks the HOST daemon (not a
 * binary inside the untrusted image, so distroless/scratch images are fine) for the
 * container's attached networks and their IPs:
 *   0  = isolated   (no attached network, or only the "none" network with no IP)
 *   1  = has network (a non-"none" network is attached OR some network has an IP —
 *                     the runtime gave the sandbox real egress; ANY assigned address
 *                     counts, so a subnet-only/link-local route cannot slip past)
 *  -1  = undetermined (the inspect probe could not run)
 * The caller decides what an undetermined result means (fail-closed under enforce). */
/* Run the network probe and hand back its RAW report.
 *
 * What the report means is not decided here: reading it and judging it are one
 * rule, and it lives in the module. This half is the part that must stay --
 * inspecting a container is I/O.
 *
 * Returns 0 with *out set to a malloc'd report (caller frees), or -1 when the
 * probe could not run at all. Those are different answers: a failed probe is
 * UNKNOWN, and unknown is not isolated. */
static int docker_sandbox_network_report(const char *container, char **out)
{
   const char *argv[] = {
       resolve_docker_bin(),
       "inspect",
       "--format",
       "{{range $k,$v := .NetworkSettings.Networks}}{{$k}}={{$v.IPAddress}};{{end}}",
       container,
       NULL};
   char *report = NULL;
   if (safe_exec_capture_cwd_env_timeout(argv, NULL, NULL, &report, 65536,
                                         DOCKER_PROBE_TIMEOUT_MS) != 0)
   {
      free(report);
      return -1;
   }
   *out = report;
   return 0;
}

/* Read a linked worktree's `.git` pointer file: "gitdir: <absolute path>".
 * Returns 0 and fills `out` on success. */
static int docker_read_gitlink(const char *gitfile, char *out, size_t outsz)
{
   FILE *f = fopen(gitfile, "r");
   if (!f)
      return -1;
   char line[MAX_PATH_LEN + 16];
   if (!fgets(line, sizeof(line), f))
   {
      fclose(f);
      return -1;
   }
   fclose(f);
   const char *p = strstr(line, "gitdir:");
   if (!p)
      return -1;
   p += 7;
   while (*p == ' ' || *p == '\t')
      p++;
   size_t n = strlen(p);
   while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' '))
      n--;
   if (!n || p[0] != '/' || n >= outsz)
      return -1; /* must be absolute: a relative gitdir is not one we can mount */
   memcpy(out, p, n);
   out[n] = '\0';
   return 0;
}

/* Add "<src>:<dst>[:ro]" to the state's mount list. */
/* Translate a bind-mount SOURCE from this process's filesystem view to the docker
 * DAEMON's view. Needed when aimee-server itself runs in a container and drives a
 * SIBLING daemon (docker-in-docker / SmoothNAS LXC2Docker): a path like
 * /var/lib/aimee/... exists inside aimee-server's container but NOT on the daemon's
 * host, so binding `src:dst` with the container path silently mounts the wrong (or
 * an empty) directory. The mapping comes from AIMEE_SANDBOX_HOST_MOUNTS, a
 * comma-separated list of `<container-prefix>=<host-prefix>` pairs (longest match
 * wins); the deploy sets it from the plugin's own bind mounts. Unset / no match =>
 * the path is used unchanged, so ordinary same-host deploys are untouched. Only
 * the source is translated — the destination (the path the delegate sees inside
 * the sandbox) is deliberately left as-is. */
/* Discover what a caller-provided tree `real` (already canonical) actually IS.
 *
 * This reads the host filesystem, which is why it stays here: the module decides
 * the container's shape, but it never touches disk and could not answer any of
 * these questions. What it gets back is the answer, as content.
 *
 * A LINKED WORKTREE's .git is a FILE holding `gitdir: <absolute host path>` into
 * the main repo. Mounting the worktree alone leaves that path absent inside the
 * container and every git command fails -- after the delegate has started work,
 * looking like a corrupt repo rather than a bad mount.
 *
 * Fills `repo` with the repository root and `gitdir` with the worktree's git
 * metadata directory. For a PLAIN checkout `repo` is `real` itself and `gitdir`
 * is empty: the tree carries its own .git and there is nothing separate to find.
 * Returns 0, or -1 having logged why. */
static int docker_discover_repo(const char *real, char *repo, size_t repo_cap, char *gitdir,
                                size_t gitdir_cap)
{
   repo[0] = '\0';
   gitdir[0] = '\0';

   char gitmark[MAX_PATH_LEN];
   if ((size_t)snprintf(gitmark, sizeof(gitmark), "%s/.git", real) >= sizeof(gitmark))
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace path '%s' is too long to check for .git; refusing", real);
      return -1;
   }
   struct stat gst;
   if (lstat(gitmark, &gst) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s' is not a git checkout (no .git); refusing to bind-mount an "
                "arbitrary host directory into a delegate container",
                real);
      return -1;
   }
   if (S_ISLNK(gst.st_mode))
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s' has a symlinked .git; refusing to treat it as a checkout", real);
      return -1;
   }

   if (S_ISDIR(gst.st_mode))
   {
      /* Plain checkout: the tree carries its own .git and IS its own repo root. */
      if ((size_t)snprintf(repo, repo_cap, "%s", real) >= repo_cap)
         return -1;
      return 0;
   }

   /* Linked worktree. */
   if (docker_read_gitlink(gitmark, gitdir, gitdir_cap) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s': .git is a file but carries no absolute `gitdir:` pointer; "
                "refusing rather than mounting a worktree whose git would be broken",
                real);
      return -1;
   }
   struct stat gdst;
   if (stat(gitdir, &gdst) != 0 || !S_ISDIR(gdst.st_mode))
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s': gitdir '%s' does not exist on the host; refusing", real, gitdir);
      return -1;
   }
   /* The repo root is what contains that gitdir: <repo>/.git/worktrees/<name>. */
   const char *marker = strstr(gitdir, "/.git/");
   if (!marker)
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s': gitdir '%s' is not under a <repo>/.git/ — cannot derive the repo "
                "root to mount; refusing",
                real, gitdir);
      return -1;
   }
   size_t rlen = (size_t)(marker - gitdir);
   if (rlen == 0 || rlen >= repo_cap)
      return -1;
   memcpy(repo, gitdir, rlen);
   repo[rlen] = '\0';

   /* Is the worktree physically nested under its repo root? A normal delegate
    * sibling worktree (under <repo>/.aimee/worktrees/) is; a WFE per-slice
    * worktree is NOT -- it lives outside the repo, under $AIMEE_HOME/wfe-worktrees.
    * Nesting is not required by the mounts, which bind each path at its own
    * absolute location. What nesting DID provide, for free, was evidence that the
    * worktree belongs to the repo. For the disjoint case we cannot lean on
    * physical containment, so prove the two-way git link explicitly before
    * mounting an out-of-repo tree: the repo's per-worktree admin dir carries a
    * `gitdir` file that must point back at <real>/.git. Together with the forward
    * gitlink read above this defeats a spoofed .git aimed at an unrelated repo.
    * (`real` is already realpath-canonicalized and workspace-authorized.) */
   size_t plen = strlen(repo);
   int nested = (strncmp(real, repo, plen) == 0 && (real[plen] == '/' || real[plen] == '\0'));
   if (!nested)
   {
      char backlink[MAX_PATH_LEN], expect[MAX_PATH_LEN], got[MAX_PATH_LEN] = "";
      if ((size_t)snprintf(backlink, sizeof(backlink), "%s/gitdir", gitdir) >= sizeof(backlink) ||
          (size_t)snprintf(expect, sizeof(expect), "%s/.git", real) >= sizeof(expect))
         return -1;
      FILE *bf = fopen(backlink, "r");
      if (bf)
      {
         if (fgets(got, sizeof(got), bf))
         {
            size_t n = strlen(got);
            while (n && (got[n - 1] == '\n' || got[n - 1] == '\r' || got[n - 1] == ' ' ||
                         got[n - 1] == '\t'))
               got[--n] = '\0';
         }
         fclose(bf);
      }
      if (strcmp(got, expect) != 0)
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace '%s' is outside repo root '%s' and its worktree backlink '%s' does "
                   "not point back to it (got '%s', want '%s'); refusing",
                   real, repo, backlink, got[0] ? got : "<none>", expect);
         return -1;
      }
   }
   aimee_log(LOG_INFO, "delegate-backend-docker", "%s worktree '%s': repo '%s', gitdir '%s'",
             nested ? "linked" : "disjoint linked (verified backlink)", real, repo, gitdir);
   return 0;
}

/* Render AIMEE_SANDBOX_HOST_MOUNTS into the ONE table format the module reads:
 * "<destination>\t<source>" per line, as `docker inspect` reports mounts.
 *
 * The env var is written "container=host,container2=host2", which is a different
 * encoding of the same mapping. Passing it through unconverted would match
 * nothing, translate nothing, and leave every bind source as an in-container
 * path -- which docker does not reject, it CREATES. The delegate would come up
 * with empty directories where its workspace should be.
 *
 * The env stays the source of truth for workspace sources exactly as before;
 * only the encoding changes. */
static void docker_mount_table(char *out, size_t cap)
{
   out[0] = '\0';
   const char *map = getenv("AIMEE_SANDBOX_HOST_MOUNTS");
   if (!map || !map[0])
      return;

   size_t at = 0;
   const char *p = map;
   while (*p && at + 1 < cap)
   {
      const char *comma = strchr(p, ',');
      size_t plen = comma ? (size_t)(comma - p) : strlen(p);
      const char *eq = memchr(p, '=', plen);
      if (eq)
      {
         size_t clen = (size_t)(eq - p);
         size_t hlen = plen - clen - 1;
         if (clen > 0 && hlen > 0)
         {
            int n = snprintf(out + at, cap - at, "%.*s\t%.*s\n", (int)clen, p, (int)hlen, eq + 1);
            if (n < 0 || (size_t)n >= cap - at)
            {
               /* A truncated table would translate some mounts and not others,
                * which is worse than translating none: the delegate would get a
                * working repo and an empty worktree. */
               out[0] = '\0';
               return;
            }
            at += (size_t)n;
         }
      }
      p = comma ? comma + 1 : p + plen;
   }
}

/* package_access=proxy: copy the static aimee-forwarder into the (running) delegate
 * container and start it detached, so 127.0.0.1:3129 inside the sandbox bridges to the
 * bound aimee UDS where the package forward proxy runs. Best-effort: on failure the
 * delegate still runs; package installs then fail fast (http_proxy -> a dead port is
 * refused immediately, no hang). The forwarder is re-copied/started after every start
 * because a `docker exec -d` process does not survive a container stop. */
/* Remove the forwarder binary from the container so a half-configured proxy delegate
 * can't be left with an unusable-but-present forwarder it could relaunch pointed at an
 * arbitrary UDS path. Best-effort. */
static void docker_pkg_forwarder_strip(const char *container)
{
   const char *rm_argv[] = {
       "docker", "exec", container, "rm", "-f", "/usr/local/bin/aimee-forwarder", NULL};
   (void)run_docker(rm_argv);
}

static void docker_pkg_forwarder_setup(const char *container)
{
   /* Fixed, canonical ship path — never operator/env-overridable, so nothing can
    * point the copy at an attacker-controlled binary. */
   static const char *const bin = "/usr/local/bin/aimee-forwarder";
   struct stat bst;
   if (stat(bin, &bst) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "package-access=proxy but forwarder binary '%s' is missing — sandbox package "
                "installs will fail (ship aimee-forwarder in the aimee-server image)",
                bin);
      return;
   }
   char dst[MAX_PATH_LEN + 64];
   snprintf(dst, sizeof(dst), "%s:/usr/local/bin/aimee-forwarder", container);
   const char *cp_argv[] = {"docker", "cp", bin, dst, NULL};
   if (run_docker(cp_argv) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox", "forwarder `docker cp` into %s failed", container);
      return;
   }
   const char *ex_argv[] = {"docker",
                            "exec",
                            "-d",
                            container,
                            "env",
                            "AIMEE_FORWARDER_SOCK=" DELEGATE_SOCK_PATH,
                            "AIMEE_FORWARDER_PORT=3129",
                            "/usr/local/bin/aimee-forwarder",
                            NULL};
   if (run_docker(ex_argv) != 0)
   {
      /* Copied but not running: strip it rather than leave a relaunchable binary. */
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "starting package forwarder in %s failed — stripping the binary", container);
      docker_pkg_forwarder_strip(container);
      return;
   }
   /* apt does not honour http_proxy through every transport — drop in an explicit
    * proxy config too. Best-effort (needs a root-writable /etc/apt; a non-root
    * delegate still gets the env vars, which cover apt's http/https transports). */
   const char *apt_argv[] = {"docker",
                             "exec",
                             container,
                             "sh",
                             "-c",
                             "mkdir -p /etc/apt/apt.conf.d 2>/dev/null && "
                             "printf 'Acquire::http::Proxy \"http://127.0.0.1:3129\";\\n"
                             "Acquire::https::Proxy \"http://127.0.0.1:3129\";\\n' "
                             "> /etc/apt/apt.conf.d/99aimee-proxy 2>/dev/null || true",
                             NULL};
   (void)run_docker(apt_argv);
   aimee_log(LOG_INFO, "delegate-sandbox",
             "package proxy armed in %s (127.0.0.1:3129 -> aimee UDS; egress via aimee)",
             container);
}

static int docker_acquire(delegate_backend_t *self, const char *task_id,
                          const delegate_backend_config_t *cfg, void **state_out)
{
   (void)self;
   if (state_out)
      *state_out = NULL;
   if (!task_id || !task_id[0] || !state_out)
      return -1;

   docker_state_t *st = calloc(1, sizeof(*st));
   if (!st)
      return -1;
   /* The container's NAME comes back with its argv: the name folds in the mounts
    * so a task id reused against a different tree cannot resume the old
    * container, and the mounts are the module's decision. Computing it here from
    * a different set of mounts is the bug that rule exists to prevent. */
   aimee_delegates_launch_spec_t launch;
   memset(&launch, 0, sizeof(launch));
   char repo[MAX_PATH_LEN] = "", gitdir[MAX_PATH_LEN] = "", real[MAX_PATH_LEN] = "";
   char userflag[64] = "";
   const char *image = (cfg && cfg->image && cfg->image[0]) ? cfg->image : DOCKER_DEFAULT_IMAGE;
   snprintf(st->image, sizeof(st->image), "%s", image);
   if (cfg && cfg->workspace && cfg->workspace[0])
   {
      /* Caller-provided tree: mount it so the delegate gets the entire current
       * source tree — by bind-mount, so it IS the tree, not a copy that can drift.
       *
       * Canonicalize FIRST: stat() follows symlinks and so does the docker daemon,
       * so validating the path as given and mounting the same string would let a
       * symlinked component point the mount somewhere the checks never saw. */
      if (!realpath(cfg->workspace, real))
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace '%s' does not resolve (%s); refusing to mount it — a delegate "
                   "would see an empty tree and conclude the code is missing",
                   cfg->workspace, strerror(errno));
         free(st);
         return -1;
      }
      struct stat wst;
      if (stat(real, &wst) != 0 || !S_ISDIR(wst.st_mode))
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace '%s' is not an existing directory; refusing to mount it", real);
         free(st);
         return -1;
      }
      /* Refuse truncation: a path that passed the checks but does not fit would
       * mount a different directory than the one that was validated. */
      if ((size_t)snprintf(st->workspace_host, sizeof(st->workspace_host), "%s", real) >=
          sizeof(st->workspace_host))
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace path '%s' is too long for the mount buffer; refusing rather than "
                   "mounting a truncated path",
                   real);
         free(st);
         return -1;
      }
      if (docker_discover_repo(real, repo, sizeof(repo), gitdir, sizeof(gitdir)) != 0)
      {
         free(st);
         return -1;
      }
      /* Mounted at its own absolute path, so paths inside the container match
       * the host's and a tool that reports a path reports a real one. */
      snprintf(st->workdir, sizeof(st->workdir), "%s", real);
      launch.repo_root = repo;
      launch.worktree = real;
      launch.gitdir = gitdir[0] ? gitdir : NULL;
      launch.is_git_checkout = 1;
      launch.writes_allowed = (cfg->workspace_read_only == 0);
      /* Run as the server's uid:gid: the container would otherwise write into
       * the caller's checkout as root, and the user could not then edit or
       * delete their own files. Only for a caller tree -- nobody else owns our
       * scratch dir. */
      snprintf(userflag, sizeof(userflag), "%u:%u", (unsigned)getuid(), (unsigned)getgid());
      launch.run_as_user = userflag;
   }
   else if (compute_workspace_host(task_id, st->workspace_host, sizeof(st->workspace_host)) != 0 ||
            docker_mkdir_p(st->workspace_host) != 0)
   {
      free(st);
      return -1;
   }
   else
   {
      /* Our own scratch dir: keep the historical /workspace contract. */
      snprintf(st->workdir, sizeof(st->workdir), "%s", resolve_docker_workdir());
      launch.scratch_dir = st->workspace_host;
      launch.scratch_target = st->workdir;
   }
   snprintf(st->cwd, sizeof(st->cwd), "%s", st->workdir);

   /* Runtime package-access policy: "proxy" (default) arms the in-container forwarder
    * + http_proxy so a --network none delegate can install via aimee's egress. The
    * caller resolved the mode (it already loads config); the backend stays
    * config-agnostic. */
   int pkg_proxy = cfg && cfg->pkg_proxy;

   /* The server UDS is the delegate's only outward channel (see the
    * DELEGATE_SOCK_PATH note). Docker resolves bind sources in the daemon's
    * namespace, so translate the path when aimee-server itself is containerized. */
   char container_sock[MAX_PATH_LEN + 32] = "";
   char host_sock[MAX_PATH_LEN + 32] = "";
   char sock_bind[MAX_PATH_LEN + 96] = "";
   const char *aimee_h = aimee_home();
   if (aimee_h && aimee_h[0])
      snprintf(container_sock, sizeof(container_sock), "%s/aimee-http.sock", aimee_h);
   struct stat sock_st;
   int socket_path_result = -1;
   const int socket_exists =
       container_sock[0] && stat(container_sock, &sock_st) == 0 && S_ISSOCK(sock_st.st_mode);
   if (socket_exists)
      socket_path_result = docker_host_path_for_self(container_sock, host_sock, sizeof(host_sock));

   /* In a container the daemon cannot use an un-translated in-container path.
    * Fail here instead of asking Docker to create a directory at that path and
    * starting a sandbox whose every aimee tool call will time out. A host-native
    * server legitimately has no self container to inspect, so result 0 is valid. */
   if (socket_exists && server_runs_in_container() && socket_path_result != 1)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "cannot map server socket %s into the Docker daemon namespace; refusing "
                "to start a sandbox with a broken tool channel",
                container_sock);
      free(st);
      return -1;
   }
   const int have_sock = socket_exists && socket_path_result >= 0;
   if (have_sock)
      snprintf(sock_bind, sizeof(sock_bind), "%s:%s", host_sock, DELEGATE_SOCK_PATH);

   /* Ask the module for this container: its name and the command that creates
    * it, decided together from one set of mounts.
    *
    * host_sock is already in the daemon's namespace -- it was resolved by
    * inspecting THIS container, which is better information than the mount
    * table and exists for that path alone. The module leaves the control socket
    * untranslated for exactly that reason. */
   char mount_table[8192];
   docker_mount_table(mount_table, sizeof(mount_table));

   static const char *const sleep_forever[] = {"sleep", "infinity", NULL};
   launch.task_id = task_id;
   launch.image = st->image;
   launch.workdir = st->workdir;
   launch.mount_table = mount_table[0] ? mount_table : NULL;
   launch.command = sleep_forever;
   if (have_sock)
   {
      launch.parent_socket_host = host_sock;
      launch.parent_socket_target = DELEGATE_SOCK_PATH;
      /* Only with the socket present: the forwarder bridges to it, so pointing
       * package managers at a proxy with no channel behind it would hang them. */
      if (pkg_proxy)
         launch.egress_proxy = DELEGATE_PKG_PROXY_URL;
   }

   /* One slot per argument plus the NULL. The buffer is the module's response
    * and the argv points into it, so both live until the container is created. */
   const char *create_argv[128];
   uint8_t *argv_buf = malloc(DELEGATE_LAUNCH_BUF);
   if (!argv_buf)
   {
      free(st);
      return -1;
   }
   create_argv[0] = "docker";
   int argc = delegate_launch_args_resolve(
       &launch, st->container_name, sizeof(st->container_name), create_argv + 1,
       sizeof(create_argv) / sizeof(create_argv[0]) - 1, argv_buf, DELEGATE_LAUNCH_BUF);
   if (argc < 0)
   {
      free(argv_buf);
      free(st);
      return -1;
   }

   /* Try `docker start` first — if a container with this name already
    * exists (operator opted hibernate=1 last release), starting it
    * resumes the same workspace. If start fails, create + start. */
   const char *start_argv[] = {"docker", "start", st->container_name, NULL};
   int started = run_docker(start_argv) == 0;
   if (started && have_sock)
   {
      char resumed_socket[MAX_PATH_LEN + 32] = "";
      int resumed_socket_result = docker_host_path_for_container(
          st->container_name, DELEGATE_SOCK_PATH, resumed_socket, sizeof(resumed_socket));
      if (resumed_socket_result != 1 || strcmp(resumed_socket, host_sock) != 0)
      {
         aimee_log(LOG_WARN, "delegate-sandbox",
                   "container %s has a stale or missing server socket bind; recreating it",
                   st->container_name);
         const char *rm_argv[] = {"docker", "rm", "-f", st->container_name, NULL};
         (void)run_docker(rm_argv);
         started = 0;
      }
   }
   if (!started)
   {
      if (run_docker(create_argv) != 0)
      {
         free(argv_buf);
         free(st);
         return -1;
      }
      const char *start2_argv[] = {"docker", "start", st->container_name, NULL};
      if (run_docker(start2_argv) != 0)
      {
         free(argv_buf);
         free(st);
         return -1;
      }
   }
   free(argv_buf);

   /* Verify the runtime honoured --network none. Some container runtimes (e.g.
    * SmoothNAS/tierd, which attaches a primary veth that cannot be disconnected) ignore
    * it, giving the sandbox real egress and silently defeating the package-access proxy
    * and its allowlist. Probe now that the container is up (covers create AND resume). */
   {
      char *report = NULL;
      int probe_failed = docker_sandbox_network_report(st->container_name, &report) != 0;
      int refuse = 0, warn = 0, is_error = 0;
      char reason[512] = "";
      int judged = delegate_isolation_judge(report ? report : "", probe_failed,
                                            cfg && cfg->require_isolation, &refuse, &warn,
                                            &is_error, reason, sizeof(reason));
      free(report);

      /* No verdict is not a pass. The judgement's own reasoning applies to the
       * judgement itself: a sandbox nobody could assess is not an assessed
       * sandbox, so an unanswered call refuses exactly as a failed probe under
       * require_isolation would. */
      if (judged != 0)
      {
         aimee_log(LOG_ERROR, "delegate-sandbox",
                   "container %s: isolation could not be judged; refusing to run",
                   st->container_name);
         refuse = 1;
         is_error = 1;
      }

      /* Severity comes from the verdict: a breach that runs anyway is an ERROR
       * even though it does not refuse, while an unverifiable probe on a box
       * that does not require isolation is only a warning. */
      if (refuse || warn || is_error)
         aimee_log((refuse || is_error) ? LOG_ERROR : LOG_WARN, "delegate-sandbox",
                   "container %s: %s", st->container_name,
                   reason[0] ? reason : "isolation unverified");

      if (refuse)
      {
         const char *rm_argv[] = {"docker", "rm", "-f", st->container_name, NULL};
         (void)run_docker(rm_argv);
         free(st);
         return DELEGATE_ACQUIRE_REFUSED_ISOLATION;
      }
   }

   /* Container is running (resumed or freshly created): (re)arm the package forwarder.
    * A docker exec -d process does not survive a stop, so this runs on every acquire. */
   if (pkg_proxy)
      docker_pkg_forwarder_setup(st->container_name);

   *state_out = st;
   return 0;
}

static void docker_release(delegate_backend_t *self, void *state, int hibernate)
{
   (void)self;
   if (!state)
      return;
   docker_state_t *st = state;
   if (st->container_name[0])
   {
      if (hibernate)
      {
         /* Stop only; container persists for next acquire. */
         const char *argv[] = {"docker", "stop", st->container_name, NULL};
         (void)run_docker(argv);
      }
      else
      {
         /* Force-remove (kills if running, removes container). */
         const char *argv[] = {"docker", "rm", "-f", st->container_name, NULL};
         (void)run_docker(argv);
      }
   }
   free(st);
}

static int docker_exec(delegate_backend_t *self, void *state, const char *command, int timeout_ms,
                       delegate_exec_result_t *result_out)
{
   (void)self;
   if (!state || !command || !result_out)
      return -1;
   docker_state_t *st = state;
   long long start = util_now_ms();

   /* Prefix `cd <cwd> &&` when set_cwd has been called. The container
    * default cwd is /workspace (see -w flag at create); set_cwd lets
    * the operator land elsewhere across exec calls. */
   char *prefixed = NULL;
   const char *to_run = command;
   if (st->cwd[0] && strcmp(st->cwd, resolve_docker_workdir()) != 0)
   {
      size_t need = strlen(st->cwd) + strlen(command) + 16;
      prefixed = malloc(need);
      if (prefixed)
      {
         snprintf(prefixed, need, "cd %s && %s", st->cwd, command);
         to_run = prefixed;
      }
   }

   /* Build the docker shell command via the iter-37 b64 helper. */
   char *docker_cmd = NULL;
   if (docker_build_exec_command(st->container_name, to_run, timeout_ms, &docker_cmd) != 0 ||
       !docker_cmd)
   {
      free(prefixed);
      return -1;
   }
   free(prefixed);

   /* If AIMEE_DOCKER_BIN is set, swap the leading "docker" in the
    * built command for the override binary. Production runs leave
    * the env unset and the leading "docker" wins via PATH. */
   const char *bin_override = getenv("AIMEE_DOCKER_BIN");
   if (bin_override && bin_override[0] && strncmp(docker_cmd, "docker ", 7) == 0)
   {
      size_t bin_len = strlen(bin_override);
      size_t tail_len = strlen(docker_cmd) - 6; /* "docker" is 6 chars */
      char *swapped = malloc(bin_len + tail_len + 1);
      if (swapped)
      {
         memcpy(swapped, bin_override, bin_len);
         memcpy(swapped + bin_len, docker_cmd + 6, tail_len + 1);
         free(docker_cmd);
         docker_cmd = swapped;
      }
   }

   int out_pipe[2] = {-1, -1};
   int err_pipe[2] = {-1, -1};
   if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
   {
      if (out_pipe[0] >= 0)
         close(out_pipe[0]);
      if (out_pipe[1] >= 0)
         close(out_pipe[1]);
      if (err_pipe[0] >= 0)
         close(err_pipe[0]);
      if (err_pipe[1] >= 0)
         close(err_pipe[1]);
      free(docker_cmd);
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      free(docker_cmd);
      return -1;
   }
   if (pid == 0)
   {
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(err_pipe[1], STDERR_FILENO);
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      execlp("sh", "sh", "-c", docker_cmd, (char *)NULL);
      _exit(127);
   }
   free(docker_cmd);
   close(out_pipe[1]);
   close(err_pipe[1]);

   long long deadline = timeout_ms > 0 ? start + timeout_ms : 0;
   size_t out_off = 0;
   size_t err_off = 0;
   int out_open = 1, err_open = 1;
   while (out_open || err_open)
   {
      fd_set rfds;
      FD_ZERO(&rfds);
      int maxfd = -1;
      if (out_open)
      {
         FD_SET(out_pipe[0], &rfds);
         if (out_pipe[0] > maxfd)
            maxfd = out_pipe[0];
      }
      if (err_open)
      {
         FD_SET(err_pipe[0], &rfds);
         if (err_pipe[0] > maxfd)
            maxfd = err_pipe[0];
      }
      struct timeval tv = {1, 0};
      if (deadline > 0)
      {
         long long remaining = deadline - util_now_ms();
         if (remaining <= 0)
         {
            kill(pid, SIGTERM);
            break;
         }
         tv.tv_sec = remaining / 1000;
         tv.tv_usec = (remaining % 1000) * 1000;
      }
      int sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
      if (sel < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (sel == 0)
         continue;
      if (out_open && FD_ISSET(out_pipe[0], &rfds))
      {
         if (result_out->stdout_buf && out_off + 1 < result_out->stdout_cap)
         {
            ssize_t r = read(out_pipe[0], result_out->stdout_buf + out_off,
                             result_out->stdout_cap - 1 - out_off);
            if (r <= 0)
               out_open = 0;
            else
               out_off += (size_t)r;
         }
         else
         {
            char throwaway[4096];
            if (read(out_pipe[0], throwaway, sizeof(throwaway)) <= 0)
               out_open = 0;
         }
      }
      if (err_open && FD_ISSET(err_pipe[0], &rfds))
      {
         if (result_out->stderr_buf && err_off + 1 < result_out->stderr_cap)
         {
            ssize_t r = read(err_pipe[0], result_out->stderr_buf + err_off,
                             result_out->stderr_cap - 1 - err_off);
            if (r <= 0)
               err_open = 0;
            else
               err_off += (size_t)r;
         }
         else
         {
            char throwaway[4096];
            if (read(err_pipe[0], throwaway, sizeof(throwaway)) <= 0)
               err_open = 0;
         }
      }
   }
   close(out_pipe[0]);
   close(err_pipe[0]);

   /* Terminate where the OUTPUT ends, not at the end of the buffer.
    *
    * The caller's buffer is malloc'd and uninitialized, and callers read it with
    * strlen() -- docker_read_file does exactly that before slicing. Terminating
    * only the final byte leaves whatever the allocator last had in that arena
    * sitting between the real output and that NUL, so strlen() walks straight
    * into it and the delegate is handed its file with heap garbage appended.
    *
    * It read correctly for as long as these allocations happened to be fresh
    * pages, which are zero. Any allocation and free of comparable size beforehand
    * is enough to expose it, and the result is silent: a file the delegate reads
    * comes back longer than it is, with plausible-looking bytes on the end.
    *
    * The final byte stays terminated as a backstop for a truncated capture. */
   if (result_out->stdout_buf && result_out->stdout_cap > 0)
   {
      result_out
          ->stdout_buf[out_off < result_out->stdout_cap ? out_off : result_out->stdout_cap - 1] =
          '\0';
      result_out->stdout_buf[result_out->stdout_cap - 1] = '\0';
   }
   if (result_out->stderr_buf && result_out->stderr_cap > 0)
   {
      result_out
          ->stderr_buf[err_off < result_out->stderr_cap ? err_off : result_out->stderr_cap - 1] =
          '\0';
      result_out->stderr_buf[result_out->stderr_cap - 1] = '\0';
   }

   int status = 0;
   waitpid(pid, &status, 0);
   result_out->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
   result_out->latency_ms = (int)(util_now_ms() - start);
   return 0;
}

/* Workspace path validation: reject absolute paths and any '..' segment
 * so the in-container workspace sandbox is honest. Resolves `rel`
 * against the container's WORKDIR (/workspace by default) so the same
 * path the caller hands in lands in the same place across exec calls. */
static int docker_resolve_in_workspace(const docker_state_t *st, const char *rel, char *out,
                                       size_t outsz)
{
   if (!st || !rel || !out || outsz == 0)
      return -1;
   /* Reject any parent-traversal segment outright — it could escape the workspace
    * whether the input is relative or absolute. */
   const char *p = rel;
   while (*p)
   {
      const char *seg = p;
      while (*p && *p != '/')
         p++;
      size_t len = (size_t)(p - seg);
      if (len == 2 && seg[0] == '.' && seg[1] == '.')
         return -1;
      if (*p == '/')
         p++;
   }
   const char *workdir = st->workdir[0] ? st->workdir : resolve_docker_workdir();
   if (rel[0] == '/')
   {
      /* Accept an ABSOLUTE path only when it is within the workspace root. The slice
       * worktree is bind-mounted path-identically, so such a path is already the valid
       * in-container path. The native file tools (tool_read_file/tool_write_file) resolve
       * to an absolute path via the thread cwd BEFORE calling the provider, so without
       * this branch every in-workspace read/write is wrongly rejected (bash still works
       * because it runs a raw `cd && ...` command that never reaches this resolver) — the
       * live symptom was container delegates hitting "cannot open"/"cannot write" on their
       * own worktree. A path OUTSIDE the workspace stays refused: the container must not
       * reach the host tree. */
      size_t wlen = strlen(workdir);
      if (strncmp(rel, workdir, wlen) == 0 && (rel[wlen] == '/' || rel[wlen] == '\0'))
      {
         if (snprintf(out, outsz, "%s", rel) >= (int)outsz)
            return -1;
         return 0;
      }
      return -1;
   }
   if (snprintf(out, outsz, "%s/%s", workdir, rel) >= (int)outsz)
      return -1;
   return 0;
}

static int docker_read_file(delegate_backend_t *self, void *state, const char *p, int offset,
                            int limit, char **out)
{
   if (out)
      *out = NULL;
   if (!state || !p || !out || offset < 0 || limit < 0)
      return -1;
   docker_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (docker_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
      return -1;
   /* `cat <abs>` via docker_exec; slice in C for offset/limit. limit=0
    * means "to EOF" capped at 16 MiB to bound malloc. */
   size_t cap = limit > 0 ? (size_t)limit : 16 * 1024 * 1024;
   size_t buf_cap = cap + (size_t)offset + 1;
   char *buf = malloc(buf_cap);
   char *err = malloc(4096);
   if (!buf || !err)
   {
      free(buf);
      free(err);
      return -1;
   }
   delegate_exec_result_t r = {0, 0, buf, buf_cap, err, 4096};
   char cmd[MAX_PATH_LEN + 16];
   snprintf(cmd, sizeof(cmd), "cat %s", abs);
   if (docker_exec(self, state, cmd, 30000, &r) != 0 || r.exit_code != 0)
   {
      free(buf);
      free(err);
      return -1;
   }
   free(err);
   size_t n = strlen(buf);
   if ((size_t)offset > n)
      offset = (int)n;
   size_t avail = n - (size_t)offset;
   size_t take = avail < cap ? avail : cap;
   char *slice = malloc(take + 1);
   if (!slice)
   {
      free(buf);
      return -1;
   }
   memcpy(slice, buf + offset, take);
   slice[take] = '\0';
   free(buf);
   *out = slice;
   return 0;
}

static int docker_write_file(delegate_backend_t *self, void *state, const char *p,
                             const char *content)
{
   if (!state || !p || !content)
      return -1;
   docker_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (docker_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
      return -1;

   size_t clen = strlen(content);
   size_t b64_cap = ((clen + 2) / 3) * 4 + 1;
   char *b64 = malloc(b64_cap);
   if (!b64)
      return -1;
   if (util_b64_encode(content, clen, b64, b64_cap) < 0)
   {
      free(b64);
      return -1;
   }
   /* mkdir -p parent then b64-decode into the file. */
   size_t cmd_cap = strlen(abs) * 2 + b64_cap + 128;
   char *cmd = malloc(cmd_cap);
   if (!cmd)
   {
      free(b64);
      return -1;
   }
   snprintf(cmd, cmd_cap, "mkdir -p \"$(dirname %s)\" && echo '%s' | base64 -d > %s", abs, b64,
            abs);
   free(b64);

   char out[256] = {0}, err[256] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   int rc = docker_exec(self, state, cmd, 30000, &r);
   free(cmd);
   if (rc != 0 || r.exit_code != 0)
      return -1;
   return 0;
}

static int docker_list_dir(delegate_backend_t *self, void *state, const char *p,
                           char ***entries_out)
{
   if (entries_out)
      *entries_out = NULL;
   if (!state || !p || !entries_out)
      return -1;
   docker_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (docker_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
   {
      /* The tool surfaces only "glob failed"; log why so a list_files that keeps failing
       * on a delegate's own worktree is diagnosable (path outside workdir, or ".."). */
      LOG_WARN("delegate-backend-docker", "list_dir: path '%s' did not resolve in workspace '%s'",
               p, st->workdir[0] ? st->workdir : resolve_docker_workdir());
      return -1;
   }

   char buf[64 * 1024] = {0};
   char err[4096] = {0};
   delegate_exec_result_t r = {0, 0, buf, sizeof(buf), err, sizeof(err)};
   char cmd[MAX_PATH_LEN + 16];
   snprintf(cmd, sizeof(cmd), "ls -1A %s", abs);
   int xrc = docker_exec(self, state, cmd, 30000, &r);
   if (xrc != 0 || r.exit_code != 0)
   {
      LOG_WARN("delegate-backend-docker",
               "list_dir: 'ls -1A %s' failed (exec_rc=%d exit=%d): %.200s", abs, xrc, r.exit_code,
               err[0] ? err : "(no stderr)");
      return -1;
   }

   int n = 0;
   for (const char *q = buf; *q; q++)
      if (*q == '\n')
         n++;
   if (buf[0] && buf[strlen(buf) - 1] != '\n')
      n++;

   char **entries = calloc((size_t)n + 1, sizeof(*entries));
   if (!entries)
      return -1;
   int i = 0;
   const char *q = buf;
   while (*q && i < n)
   {
      const char *line_start = q;
      while (*q && *q != '\n')
         q++;
      size_t len = (size_t)(q - line_start);
      if (len > 0)
      {
         entries[i] = malloc(len + 1);
         if (!entries[i])
         {
            for (int j = 0; j < i; j++)
               free(entries[j]);
            free(entries);
            return -1;
         }
         memcpy(entries[i], line_start, len);
         entries[i][len] = '\0';
         i++;
      }
      if (*q == '\n')
         q++;
   }
   entries[i] = NULL;
   *entries_out = entries;
   return i;
}

static int docker_get_cwd(delegate_backend_t *self, void *state, char **out)
{
   (void)self;
   if (!state || !out)
      return -1;
   docker_state_t *st = state;
   *out = strdup(st->cwd[0] ? st->cwd : resolve_docker_workdir());
   return *out ? 0 : -1;
}

static int docker_set_cwd(delegate_backend_t *self, void *state, const char *path)
{
   (void)self;
   if (!state || !path || !path[0])
      return -1;
   docker_state_t *st = state;
   snprintf(st->cwd, sizeof(st->cwd), "%s", path);
   return 0;
}

static delegate_backend_t g_docker = {
    .name = "docker",
    .description = "long-lived container per task_id",
    .acquire = docker_acquire,
    .release = docker_release,
    .exec = docker_exec,
    .read_file = docker_read_file,
    .write_file = docker_write_file,
    .list_dir = docker_list_dir,
    .get_cwd = docker_get_cwd,
    .set_cwd = docker_set_cwd,
};

int delegate_backend_register_docker(void)
{
   return delegate_backend_register(&g_docker);
}

delegate_backend_t *delegate_backend_docker_get(void)
{
   return &g_docker;
}
