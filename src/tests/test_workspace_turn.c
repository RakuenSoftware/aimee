/* test_workspace_turn.c: a turn whose cwd is inside a registered `detached`
 * workspace binds the active provider to a detached provider; shared workspaces
 * and unregistered cwds stay on the shared provider. The binder reads the
 * registered providers through the configuration module contract. */
#include "modules/workspace/workspace_turn.h"
#include "modules/workspace/workspace_provider.h"
#include "modules/workspace/workspace_runner_registry.h"
#include "modules/workspace/workspace_provider_detached.h"

/* The server's request context, as the bind sees it. */
static const char *g_test_runner_channel = "";
const char *request_context_runner_channel(void)
{
   return g_test_runner_channel;
}
#include "config.h"
#include <aimee/delegates/delegate_backend.h>
#include "modules/workspace/workspace_provider_container.h"
#include "aimee_home.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "util.h"
#include <aimee/workspace/module_api.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

static const char test_shared_workspace[] = "/tmp/ws-shared";
static const char test_detached_workspace[] = "/tmp/ws-detached";

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── fake workspace module ────────────────────────────────────────────────────
 *
 * Which client is serving which tree is the module's answer now, so a turn that
 * binds a detached provider has to ask for it. There is no bus in a unit test,
 * so this stands in for the module and reports that every tree asked about is
 * being served by itself. */

typedef int (*ws_test_module_responder_fn)(uint32_t, uint32_t, const void *, uint32_t, void *,
                                           uint32_t, uint32_t *);
void ws_test_set_module_responder(ws_test_module_responder_fn fn);

static int fake_module(uint32_t event_kind, uint32_t stage_id, const void *request,
                       uint32_t request_len, void *response, uint32_t response_capacity,
                       uint32_t *response_len)
{
   (void)stage_id;
   (void)request_len;
   if (event_kind != AIMEE_WORKSPACE_EVENT_RUNNER ||
       response_capacity < AIMEE_WS_RUNNER_RESPONSE_LEN)
      return -1;
   const uint8_t *in = (const uint8_t *)request;
   uint8_t *out = (uint8_t *)response;
   memset(out, 0, AIMEE_WS_RUNNER_RESPONSE_LEN);
   aimee_workspace_put_u32(out, AIMEE_WS_RUNNER_RESPONSE_MAGIC);
   /* Register and forget only need to succeed; resolve answers with the tree
    * that was asked about, which is what a client serving it would look like. */
   if (in[5] == AIMEE_WS_RUNNER_OP_RESOLVE)
   {
      uint16_t len = aimee_workspace_get_u16(in + 6);
      if (len > AIMEE_WS_RUNNER_ID_MAX)
         len = AIMEE_WS_RUNNER_ID_MAX;
      aimee_workspace_put_u32(out + 4, len);
      memcpy(out + 8, in + 8, len);
   }
   if (response_len)
      *response_len = AIMEE_WS_RUNNER_RESPONSE_LEN;
   return 0;
}

/* ── fake docker backend for the delegate-sandbox cases ───────────────────── */

static int g_acquires, g_releases, g_last_hibernate, g_acquire_fails;
static char g_last_workspace[512];
static int g_last_read_only;
static int g_fake_state = 1;
static int g_replace_workspace;
static char g_retired_workspace[1024];

static int count_source_fds(const char *source)
{
   struct stat expected, current;
   assert(!stat(source, &expected));
   int count = 0;
   for (int fd = 0; fd < 1024; fd++)
      if (!fstat(fd, &current) && current.st_dev == expected.st_dev &&
          current.st_ino == expected.st_ino)
         count++;
   return count;
}

static int fake_acquire(delegate_backend_t *self, const char *task_id,
                        const delegate_backend_config_t *cfg, void **out)
{
   (void)self;
   (void)task_id;
   g_acquires++;
   snprintf(g_last_workspace, sizeof(g_last_workspace), "%s",
            (cfg && cfg->workspace) ? cfg->workspace : "");
   g_last_read_only = cfg ? cfg->workspace_read_only : -1;
   if (g_acquire_fails)
      return -1;
   if (g_replace_workspace)
   {
      assert(!rename(cfg->workspace, g_retired_workspace));
      assert(!mkdir(cfg->workspace, 0700));
   }
   *out = &g_fake_state;
   return 0;
}

static void fake_release(delegate_backend_t *self, void *state, int hibernate)
{
   (void)self;
   (void)state;
   g_releases++;
   g_last_hibernate = hibernate;
}

static int fake_exec(delegate_backend_t *self, void *state, const char *command, int timeout_ms,
                     delegate_exec_result_t *r)
{
   (void)self;
   (void)state;
   (void)command;
   (void)timeout_ms;
   if (r)
      r->exit_code = 0;
   return 0;
}

/* Registered under the name "docker" so workspace_turn_bind_container finds it:
 * the seam looks the backend up by name, and a test must not need a real daemon. */
static delegate_backend_t g_fake_docker = {.name = "docker",
                                           .description = "fake docker for tests",
                                           .acquire = fake_acquire,
                                           .release = fake_release,
                                           .exec = fake_exec,
                                           .read_file = NULL,
                                           .write_file = NULL,
                                           .list_dir = NULL,
                                           .get_cwd = NULL,
                                           .set_cwd = NULL};

static void register_standard_workspaces(void)
{
   (void)config_workspace_remove("/");
   (void)config_workspace_remove("/tmp/ws-mirror-unresolvable");
   (void)config_workspace_remove(test_detached_workspace);
   (void)config_workspace_remove(test_shared_workspace);
   assert(config_workspace_add(test_detached_workspace, "detached", NULL, NULL) == 0);
   assert(config_workspace_add("/tmp/ws-shared", "shared", NULL, NULL) == 0);
}

struct mirror_callback_fixture
{
   workspace_turn_mirror_lease_t *lease;
   const char *cwd;
};
static void *mirror_callback_worker(void *opaque)
{
   struct mirror_callback_fixture *fixture = opaque;
   char allowed[MAX_PATH_LEN], digest[65];
   int accepted = workspace_turn_workspace_authorized(fixture->cwd, allowed, sizeof allowed);
   assert(!accepted);
   const workspace_turn_mirror_lease_t *previous = workspace_turn_mirror_bind(fixture->lease);
   assert(!previous);
   accepted = workspace_turn_workspace_authorized(fixture->cwd, allowed, sizeof allowed);
   assert(accepted && !strcmp(allowed, fixture->cwd));
   workspace_turn_scope_t *foreground = NULL;
   int acquired = workspace_turn_scope_begin(fixture->cwd, &foreground);
   assert(acquired == 1 && foreground);
   assert(!strcmp(workspace_turn_active_cwd(), fixture->cwd));
   assert(workspace_provider_active() == workspace_provider_shared());
   acquired = workspace_turn_scope_end(foreground);
   assert(!acquired);
   accepted = workspace_turn_workspace_authorized(fixture->cwd, allowed, sizeof allowed);
   assert(accepted && !strcmp(allowed, fixture->cwd));
   workspace_turn_scope_t *background = NULL;
   acquired = workspace_turn_scope_begin_background(fixture->cwd, &background);
   assert(acquired == 1 && background);
   assert(!strcmp(workspace_turn_active_cwd(), fixture->cwd));
   assert(workspace_provider_active() == workspace_provider_shared());
   acquired = workspace_turn_scope_end(background);
   assert(!acquired);
   int fd = -1;
   int rc = workspace_turn_resource_identity(fixture->cwd, &fd, digest);
   assert(!rc && fd >= 0 && strlen(digest) == 64);
   close(fd);
   const workspace_turn_mirror_lease_t *nested = workspace_turn_mirror_bind(NULL);
   assert(nested == fixture->lease);
   accepted = workspace_turn_workspace_authorized(fixture->cwd, allowed, sizeof allowed);
   assert(!accepted);
   workspace_turn_mirror_bind(nested);
   workspace_turn_mirror_bind(previous);
   accepted = workspace_turn_workspace_authorized(fixture->cwd, allowed, sizeof allowed);
   assert(!accepted);
   return NULL;
}
static void test_background_source_scope(const char *client, const char *source, const char *head,
                                         const char *kind, const char *temporary)
{
   int rc = config_workspace_add(client, kind, source, head);
   assert(!rc);
   workspace_turn_scope_t *scope = NULL;
   rc = workspace_turn_scope_begin_background(client, &scope);
   assert(rc == 1 && scope && workspace_turn_active_cwd());
   char actual[MAX_PATH_LEN], child[MAX_PATH_LEN + 64], allowed[MAX_PATH_LEN], command[16384];
   snprintf(actual, sizeof actual, "%s", workspace_turn_active_cwd());
   assert(strcmp(actual, client) && workspace_provider_active() == workspace_provider_shared());
   snprintf(command, sizeof command, "git -C '%s' rev-parse --git-common-dir", actual);
   char *repository = run_cmd(command, &rc);
   assert(!rc && repository);
   repository[strcspn(repository, "\r\n")] = 0;
   snprintf(child, sizeof child, "%s/.aimee/worktrees/background/main", repository);
   free(repository);
   snprintf(command, sizeof command, "git -C '%s' worktree add --quiet --detach '%s' HEAD", actual,
            child);
   rc = system(command);
   assert(!rc);
   int authorized = workspace_turn_workspace_authorized(child, allowed, sizeof allowed);
   assert(!authorized);
   const char *ambient = getenv("GIT_DIR");
   char *saved_git_dir = ambient ? strdup(ambient) : NULL;
   char spoof[MAX_PATH_LEN];
   snprintf(spoof, sizeof spoof, "%s/.git", source);
   rc = platform_setenv("GIT_DIR", spoof);
   assert(!rc);
   rc = workspace_turn_scope_hold_created_worktree(scope, temporary);
   assert(rc < 0); /* A host creation receipt cannot grant a different source. */
   rc = workspace_turn_scope_hold_created_worktree(scope, child);
   assert(!rc); /* The owned Git relationship ignores ambient redirection. */
   if (saved_git_dir)
      platform_setenv("GIT_DIR", saved_git_dir);
   else
      platform_unsetenv("GIT_DIR");
   free(saved_git_dir);
   authorized = workspace_turn_workspace_authorized(child, allowed, sizeof allowed);
   assert(authorized);
   workspace_turn_mirror_lease_t *lease = NULL;
   rc = workspace_turn_mirror_capture(child, &lease);
   assert(!rc && lease);
   rc = workspace_turn_scope_end(scope);
   assert(!rc && !workspace_turn_active_cwd());
   struct mirror_callback_fixture fixture = {lease, child};
   pthread_t worker;
   rc = pthread_create(&worker, NULL, mirror_callback_worker, &fixture);
   assert(!rc);
   rc = pthread_join(worker, NULL);
   assert(!rc);
   workspace_turn_mirror_release(lease);
   snprintf(command, sizeof command, "git -C '%s' worktree remove --force '%s'", actual, child);
   rc = system(command);
   assert(!rc);
   rc = config_workspace_remove(client);
   assert(!rc);
}

static void test_owned_mirror_callback_lease(const char *temporary)
{
   char source[1024], client[1024], command[8192], actual[MAX_PATH_LEN], recorded_head[65];
   snprintf(source, sizeof source, "%s/lease-source", temporary);
   snprintf(client, sizeof client, "%s/foreign-client", temporary);
   int made_client = mkdir(client, 0700);
   assert(!made_client);
   snprintf(command, sizeof command,
            "git init -q '%s' && git -C '%s' config user.email t@t && "
            "git -C '%s' config user.name t && echo source > '%s/README.md' && "
            "git -C '%s' add README.md && git -C '%s' commit -q -m fixture",
            source, source, source, source, source, source);
   int rc = system(command);
   assert(!rc);
   snprintf(command, sizeof command, "git -C '%s' rev-parse HEAD", source);
   char *head = run_cmd(command, &rc);
   assert(!rc && head);
   head[strcspn(head, "\r\n")] = 0;
   snprintf(recorded_head, sizeof recorded_head, "%s", head);
   rc = config_workspace_add(client, "mirror", source, head);
   free(head);
   assert(!rc);
   char denied[MAX_PATH_LEN];
   int raw_authorized = workspace_turn_workspace_authorized(client, denied, sizeof denied);
   assert(!raw_authorized); /* Registered foreign input is not a local mount grant. */
   rc = workspace_turn_bind_active(client);
   assert(rc == 1 && workspace_turn_active_cwd());
   snprintf(actual, sizeof actual, "%s", workspace_turn_active_cwd());
   workspace_turn_mirror_lease_t *lease = NULL;
   rc = workspace_turn_mirror_capture(actual, &lease);
   assert(!rc && lease);
   workspace_turn_unbind_active();
   int valid = workspace_turn_mirror_valid(lease, actual);
   assert(valid);
   struct mirror_callback_fixture fixture = {lease, actual};
   pthread_t worker;
   rc = pthread_create(&worker, NULL, mirror_callback_worker, &fixture);
   assert(!rc);
   rc = pthread_join(worker, NULL);
   assert(!rc);
   char moved[MAX_PATH_LEN + 16], outside[1024];
   snprintf(moved, sizeof moved, "%s.replaced", actual);
   snprintf(outside, sizeof outside, "%s/lease-outside", temporary);
   rc = mkdir(outside, 0700);
   assert(!rc);
   rc = rename(actual, moved);
   assert(!rc);
   rc = symlink(outside, actual);
   assert(!rc);
   valid = workspace_turn_mirror_valid(lease, actual);
   assert(!valid);
   const workspace_turn_mirror_lease_t *previous = workspace_turn_mirror_bind(lease);
   char allowed[MAX_PATH_LEN];
   valid = workspace_turn_workspace_authorized(actual, allowed, sizeof allowed);
   assert(!valid);
   workspace_turn_mirror_bind(previous);
   rc = unlink(actual);
   assert(!rc);
   rc = rename(moved, actual);
   assert(!rc);
   valid = workspace_turn_mirror_valid(lease, actual);
   assert(valid);
   workspace_turn_mirror_release(lease);
   rc = config_workspace_remove(client);
   assert(!rc);
   test_background_source_scope(client, source, recorded_head, "mirror", temporary);
   test_background_source_scope(client, source, recorded_head, "detached", temporary);
   rc = config_workspace_add(client, "detached", NULL, NULL);
   assert(!rc);
   workspace_turn_scope_t *scope = NULL;
   rc = workspace_turn_scope_begin_background(client, &scope);
   assert(rc < 0 && !scope && !workspace_turn_active_cwd());
   rc = config_workspace_remove(client);
   assert(!rc);
   puts("  owned mirror callbacks: cross-thread/nested restore and replaced-inode refusal pass");
}

static const char *test_owned_project;
static int test_project_open(const char *cwd)
{
   return test_owned_project && !strcmp(cwd, test_owned_project)
              ? open(cwd, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
              : -1;
}

int main(void)
{
   /* A failed owner acquisition is never a truthy authorization, even locally. */
   int rejected = workspace_turn_reject_foreign_cwd(-1, 1, "/tmp/owner-acquisition-failed");
   assert(rejected);

   /* Isolated temp HOME for the module caller's process-level helpers. */
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-wsturn-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");

   /* Register two workspaces: one detached, one shared (default). */
   register_standard_workspaces();

   ws_test_set_module_responder(fake_module);

   /* Restore only explicitly configured detached roots after a cold registry
    * start. A shared, nested, or unknown path cannot create a serving queue. */
   assert(ws_runner_registry_register_configured(test_detached_workspace) == 1);
   assert(ws_runner_registry_register_configured("/tmp/ws-shared") == 0);
   assert(ws_runner_registry_register_configured("/tmp/ws-detached/subdir") == 0);
   assert(ws_runner_registry_register_configured("/tmp/not-registered") == 0);
   assert(ws_runner_registry_register_configured(NULL) == 0);

   const workspace_provider_t *shared = workspace_provider_shared();
   /* Shared identities survive a new turn but change when the actual directory
    * is replaced. The held descriptor continues to pin the measured resource. */
   {
      char path[768], old[768], before[65], same[65], replaced[65];
      snprintf(path, sizeof path, "%s/identity-workspace", tmpdir);
      snprintf(old, sizeof old, "%s/identity-old", tmpdir);
      assert(!mkdir(path, 0700));
      assert(!config_workspace_add(path, "shared", NULL, NULL));
      int first = -1, second = -1;
      assert(!workspace_turn_resource_identity(path, &first, before) && first >= 0);
      assert(!workspace_turn_resource_identity(path, &second, same) && !strcmp(before, same));
      close(second);
      assert(!rename(path, old) && !mkdir(path, 0700));
      assert(!workspace_turn_resource_identity(path, &second, replaced) &&
             strcmp(before, replaced));
      struct stat pinned, changed;
      assert(!fstat(first, &pinned) && !fstat(second, &changed) && pinned.st_ino != changed.st_ino);
      close(first);
      close(second);
      assert(!rmdir(path) && !rmdir(old));
      assert(!config_workspace_remove(path));
   }

   /* Only the registered host project owner can admit an unconfigured clone;
    * identity retains the actual directory fd and never broadens to siblings. */
   {
      char project[768], sibling[768], allowed[768], digest[65];
      snprintf(project, sizeof project, "%s/owned-clone", tmpdir);
      snprintf(sibling, sizeof sibling, "%s/owned-clone-foreign", tmpdir);
      assert(!mkdir(project, 0700) && !mkdir(sibling, 0700));
      int fd = -1;
      assert(workspace_turn_resource_identity(project, &fd, digest) < 0 && fd == -1);
      workspace_turn_register_project_owner(test_project_open);
      test_owned_project = project;
      assert(!workspace_turn_resource_identity(project, &fd, digest) && fd >= 0);
      struct stat measured, actual;
      assert(!fstat(fd, &measured) && !stat(project, &actual) && measured.st_ino == actual.st_ino &&
             measured.st_dev == actual.st_dev);
      close(fd);
      assert(workspace_turn_workspace_authorized(project, allowed, sizeof allowed));
      assert(!workspace_turn_workspace_authorized(sibling, allowed, sizeof allowed));
      assert(workspace_turn_resource_identity(sibling, &fd, digest) < 0 && fd == -1);
      workspace_turn_register_project_owner(NULL);
      assert(workspace_turn_resource_identity(project, &fd, digest) < 0 && fd == -1);
      test_owned_project = NULL;
      assert(!rmdir(project) && !rmdir(sibling));
   }

   /* cwd inside the detached workspace -> binds a detached provider active */
   {
      assert(workspace_provider_active() == shared); /* default */
      int bound = workspace_turn_bind_active("/tmp/ws-detached/src/file.c");
      assert(bound == 1);
      const workspace_provider_t *active = workspace_provider_active();
      assert(active != shared && active->kind == WS_PROVIDER_DETACHED);
      workspace_turn_scope_t *nested = NULL;
      int rebound = workspace_turn_scope_begin(test_detached_workspace, &nested);
      assert(rebound == 1 && nested && workspace_provider_active() != active);
      assert(active->kind == WS_PROVIDER_DETACHED);
      int restored = workspace_turn_scope_end(nested);
      assert(!restored && workspace_provider_active() == active);
      workspace_turn_unbind_active();
      assert(workspace_provider_active() == shared); /* restored */
   }

   /* A request that names its CLI's runner channel binds that channel's queue,
    * not the tree's: only the client that asked runs its ops. A malformed
    * channel falls back to the tree. */
   {
      static const char channel[] = "runner:0123456789abcdef0123456789abcdef";
      char inside[256];
      snprintf(inside, sizeof inside, "%s/src/file.c", test_detached_workspace);
      g_test_runner_channel = channel;
      assert(workspace_turn_bind_active(inside) == 1);
      const ws_detached_provider_t *bound =
          (const ws_detached_provider_t *)workspace_provider_active();
      assert(!strcmp(((const ws_runner_queue_t *)bound->transport_ctx)->id, channel));
      workspace_turn_unbind_active();
      g_test_runner_channel = "runner:not-a-channel";
      assert(workspace_turn_bind_active(inside) == 1);
      bound = (const ws_detached_provider_t *)workspace_provider_active();
      assert(
          !strcmp(((const ws_runner_queue_t *)bound->transport_ctx)->id, test_detached_workspace));
      workspace_turn_unbind_active();
      /* Channels are per command, so many more than the registry's 64 tree
       * handles come and go; each one still binds its own channel. */
      for (int n = 0; n < 3 * WS_RUNNER_REGISTRY_MAX; n++)
      {
         char many[48];
         snprintf(many, sizeof many, "runner:%032x", n);
         g_test_runner_channel = many;
         assert(workspace_turn_bind_active(inside) == 1);
         bound = (const ws_detached_provider_t *)workspace_provider_active();
         assert(!strcmp(((const ws_runner_queue_t *)bound->transport_ctx)->id, many));
         workspace_turn_unbind_active();
      }
      g_test_runner_channel = "";
   }

   /* the workspace root itself also matches */
   {
      assert(workspace_turn_bind_active(test_detached_workspace) == 1);
      assert(workspace_provider_active()->kind == WS_PROVIDER_DETACHED);
      char first[65], repeated[65], reacquired[65];
      int fd = -1;
      assert(!workspace_turn_resource_identity(test_detached_workspace, &fd, first) && fd == -1);
      assert(!workspace_turn_resource_identity(test_detached_workspace, &fd, repeated) &&
             !strcmp(first, repeated));
      workspace_turn_unbind_active();
      assert(workspace_turn_bind_active(test_detached_workspace) == 1);
      assert(!workspace_turn_resource_identity(test_detached_workspace, &fd, reacquired) &&
             strcmp(first, reacquired));
      workspace_turn_unbind_active();
   }

   /* cwd inside a shared workspace -> stays on shared */
   {
      assert(workspace_turn_bind_active("/tmp/ws-shared/x") == 0);
      assert(workspace_provider_active() == shared);
      workspace_turn_unbind_active(); /* no-op */
      assert(workspace_provider_active() == shared);
   }

   /* unregistered cwd, and a prefix that isn't a path boundary -> shared */
   {
      assert(workspace_turn_bind_active("/tmp/elsewhere") == 0);
      assert(workspace_provider_active() == shared);
      assert(workspace_turn_bind_active("/tmp/ws-detached-other/x") == 0); /* not a boundary */
      assert(workspace_provider_active() == shared);
   }

   /* NULL / empty cwd -> shared */
   assert(workspace_turn_bind_active(NULL) == 0);
   assert(workspace_turn_bind_active("") == 0);
   assert(workspace_provider_active() == shared);

   /* Git repository identity: explicit path beats cwd, except clone path is a
    * not-yet-existing destination and therefore cannot select a repository. */
   assert(strcmp(workspace_turn_git_target("git_status", "/explicit/repo", "/caller/repo"),
                 "/explicit/repo") == 0);
   assert(strcmp(workspace_turn_git_target("git_clone", "/new/destination", "/caller/repo"),
                 "/caller/repo") == 0);
   assert(workspace_turn_git_target("git_status", NULL, NULL) == NULL);

   /* AC #6 — foreign-cwd trust gate (pure decision). A remote peer (not
    * trusted-local) supplying a raw absolute path that did NOT bind a detached
    * provider is rejected; every other combination is allowed. */
   {
      /* remote + raw foreign path + no detached bind -> REJECT */
      assert(workspace_turn_reject_foreign_cwd(0, 0, "/home/someone/repo") == 1);
      /* co-located peer (trusted_local) -> allowed (real server path) */
      assert(workspace_turn_reject_foreign_cwd(0, 1, "/home/someone/repo") == 0);
      /* detached workspace bound -> allowed (acts on the client) */
      assert(workspace_turn_reject_foreign_cwd(1, 0, "/home/someone/repo") == 0);
      /* no cwd / non-absolute / empty -> nothing to reject */
      assert(workspace_turn_reject_foreign_cwd(0, 0, NULL) == 0);
      assert(workspace_turn_reject_foreign_cwd(0, 0, "") == 0);
      assert(workspace_turn_reject_foreign_cwd(0, 0, "relative/path") == 0);
      /* traversal path -> not bound, but not a hard reject either */
      assert(workspace_turn_reject_foreign_cwd(0, 0, "/a/../etc") == 0);
   }

   /* safe_exec_capture_env: the explicit child env is honored (this is the seam
    * the mirror git runner uses to inject the forge GH_TOKEN env), and a NULL env
    * inherits the parent's. */
   {
      const char *argv[] = {"/bin/sh", "-c", "printf %s \"$WS_TURN_ENV_PROBE\"", NULL};
      char *env[] = {(char *)"WS_TURN_ENV_PROBE=mirror-token-ok", NULL};
      char *out = NULL;
      int rc = safe_exec_capture_env(argv, env, &out, 256);
      assert(rc == 0 && out && strcmp(out, "mirror-token-ok") == 0);
      free(out);

      /* NULL env → inherit; the probe var is absent in the parent → empty. */
      platform_unsetenv("WS_TURN_ENV_PROBE");
      out = NULL;
      rc = safe_exec_capture_env(argv, NULL, &out, 256);
      assert(rc == 0 && out && out[0] == '\0');
      free(out);
   }

   /* ── delegate sandbox: workspace_turn_bind_container ─────────────────────
    *
    * Binding this is the difference between a delegate's shell running in its own
    * container and running IN-PROCESS inside aimee-server with the server's
    * filesystem and environment. Each case below is a way that distinction could
    * silently collapse back to "on the host". */
   {
      delegate_backend_reset_for_test();
      assert(delegate_backend_register(&g_fake_docker) == 0);

      /* Acquires a container and binds a CONTAINER provider — not shared. If this
       * ever resolved to `shared` the delegate would be on the host while the
       * operator believed it was sandboxed. There is no dial to turn this off:
       * a delegate runs in its own container or not at all. */
      {
         g_acquires = g_releases = 0;
         mkdir(test_shared_workspace, 0700);
         assert(workspace_turn_container_bound() == 0); /* nothing bound yet */
         assert(workspace_turn_bind_container("deleg-2", NULL, test_shared_workspace, 0) == 1);
         assert(g_acquires == 1);
         const workspace_provider_t *p = workspace_provider_active();
         assert(p != shared);
         assert(p->kind == WS_PROVIDER_CONTAINER);
         /* The container-bound predicate the shell-git gate keys off: true while
          * bound, false once released. */
         assert(workspace_turn_container_bound() == 1);
         char lease[65], same[65], remote[65];
         int identity_fd = -1;
         assert(!workspace_turn_resource_identity(test_shared_workspace, &identity_fd, lease) &&
                identity_fd == -1);

         /* A nested shared lookup and detached Git operation cannot steal the
          * container lease or overwrite its provider storage. */
         workspace_turn_scope_t *outer = NULL, *inner = NULL;
         int scoped = workspace_turn_scope_begin(test_shared_workspace, &outer);
         assert(scoped == 0 && outer && workspace_provider_active() == p);
         assert(workspace_turn_container_bound() && !g_releases);
         scoped = workspace_turn_scope_begin(test_detached_workspace, &inner);
         assert(scoped == 1 && inner && workspace_provider_active() != p);
         const workspace_provider_t *nested = workspace_provider_active();
         assert(nested->kind == WS_PROVIDER_DETACHED && p->kind == WS_PROVIDER_CONTAINER);
         assert(!workspace_turn_resource_identity(test_detached_workspace, &identity_fd, remote) &&
                strcmp(lease, remote));
         assert(!workspace_turn_container_bound() && !g_releases);
         int closed = workspace_turn_scope_end(outer);
         assert(closed < 0 && workspace_provider_active() == nested && !g_releases);
         closed = workspace_turn_scope_end(inner);
         assert(!closed && workspace_provider_active() == p);
         assert(workspace_turn_container_bound() && !g_releases);
         closed = workspace_turn_scope_end(outer);
         assert(!closed && workspace_provider_active() == p && !g_releases);
         assert(!workspace_turn_resource_identity(test_shared_workspace, &identity_fd, same) &&
                !strcmp(lease, same));

         /* Unbind must RELEASE the container, not just drop the pointer: a leaked
          * container outlives its turn and pins its workspace. And the active
          * provider must go back to shared — a pooled worker thread that kept the
          * binding would run the NEXT delegate's tools in a dead container. */
         workspace_turn_unbind_active();
         assert(g_releases == 1);
         assert(workspace_provider_active() == shared);
         assert(workspace_turn_container_bound() == 0); /* cleared on unbind */
         /* Even the fixture's recycled backend state address cannot reuse a
          * released acquisition's identity. */
         assert(workspace_turn_bind_container("deleg-reacquired", NULL, test_shared_workspace, 0) ==
                1);
         assert(!workspace_turn_resource_identity(test_shared_workspace, &identity_fd, same) &&
                strcmp(lease, same));
         workspace_turn_unbind_active();
         assert(g_releases == 2);
      }

      /* Source inspection has its own exact local capability; file/exec stays
       * container-bound and detached nesting cannot borrow that capability. */
      {
         char source[768], sibling[768], retired[768], digest[65], resource[65];
         snprintf(source, sizeof source, "%s/container-source", tmpdir);
         snprintf(sibling, sizeof sibling, "%s/container-sibling", tmpdir);
         snprintf(retired, sizeof retired, "%s/container-retired", tmpdir);
         assert(!mkdir(source, 0700) && !mkdir(sibling, 0700));
         assert(!config_workspace_add(source, "shared", NULL, NULL));
         assert(!config_workspace_add(sibling, "shared", NULL, NULL));
         g_acquires = g_releases = 0;
         int before = count_source_fds(source);
         g_acquire_fails = 1;
         assert(workspace_turn_bind_container("source-failed", NULL, source, 0) < 0);
         assert(count_source_fds(source) == before && workspace_provider_active() == shared);
         g_acquire_fails = 0;
         snprintf(g_retired_workspace, sizeof g_retired_workspace, "%s", retired);
         g_replace_workspace = 1;
         assert(workspace_turn_bind_container("source-replaced-during-acquire", NULL, source, 0) <
                0);
         assert(g_releases == 1 && workspace_provider_active() == shared);
         assert(count_source_fds(retired) == before);
         assert(!rmdir(source) && !rename(retired, source));
         g_replace_workspace = 0;
         g_acquires = g_releases = 0;
         assert(workspace_turn_bind_container("source-owned", NULL, source, 0) == 1);
         const workspace_provider_t *container = workspace_provider_active();
         assert(container->kind == WS_PROVIDER_CONTAINER && count_source_fds(source) == before + 1);
         int fd = -1;
         assert(!workspace_turn_resource_identity(source, &fd, resource) && fd == -1);
         assert(!workspace_turn_source_identity(source, &fd, digest) && fd >= 0);
         struct stat measured, actual;
         assert(!fstat(fd, &measured) && !stat(source, &actual) &&
                measured.st_ino == actual.st_ino);
         close(fd);
         assert(workspace_turn_source_identity(sibling, &fd, digest) < 0 && fd == -1 && !digest[0]);
         assert(workspace_provider_active() == container);
         workspace_turn_scope_t *nested = NULL;
         assert(workspace_turn_scope_begin(test_detached_workspace, &nested) == 1 && nested);
         assert(workspace_turn_source_identity(source, &fd, digest) < 0 && fd == -1 && !digest[0]);
         assert(workspace_turn_source_identity(test_detached_workspace, &fd, digest) < 0 &&
                fd == -1);
         assert(!workspace_turn_scope_end(nested) && workspace_provider_active() == container);
         assert(!rename(source, retired) && !mkdir(source, 0700));
         assert(workspace_turn_source_identity(source, &fd, digest) < 0 && fd == -1 && !digest[0]);
         assert(!rmdir(source) && !rename(retired, source));
         assert(!workspace_turn_source_identity(source, &fd, digest) && fd >= 0);
         close(fd);
         /* An acquired exact source survives registration churn for this turn;
          * unbinding closes it and cannot grant a now-unregistered host path. */
         assert(!config_workspace_remove(source));
         assert(!workspace_turn_source_identity(source, &fd, digest) && fd >= 0);
         close(fd);
         workspace_turn_unbind_active();
         assert(g_releases == 1 && count_source_fds(source) == before);
         assert(workspace_turn_source_identity(source, &fd, digest) < 0 && fd == -1 && !digest[0]);
         assert(!config_workspace_remove(sibling));
         assert(!rmdir(source) && !rmdir(sibling));
      }
      /* The complete tree reaches the backend. */
      {
         g_acquires = g_releases = 0;
         g_last_workspace[0] = '\0';
         /* /tmp/ws-shared is a REGISTERED workspace root (see the config above), so
          * it is authorized. mkdir it so realpath resolves. */
         mkdir("/tmp/ws-shared", 0700);
         assert(workspace_turn_bind_container("deleg-5", NULL, "/tmp/ws-shared", 0) == 1);
         assert(strcmp(g_last_workspace, "/tmp/ws-shared") == 0);
         workspace_turn_unbind_active();
         assert(g_releases == 1);
      }

      /* No tree given is a hard refusal before backend acquisition. */
      {
         g_acquires = 0;
         g_last_workspace[0] = 'x';
         assert(workspace_turn_bind_container("deleg-6", NULL, NULL, 0) == -1);
         assert(g_acquires == 0);
      }
      {
         g_last_workspace[0] = 'x';
         assert(workspace_turn_bind_container("deleg-7", NULL, "", 0) == -1);
         assert(g_acquires == 0);
      }

      /* The read-only MODE must reach the backend. A delegate's changes must not
       * leave its container, so a tree that is not its own has to be unwritable at
       * the MOUNT — a guard above the provider is a rule, a :ro bind is a property.
       * If this ever silently became rw, two delegates on one tree would write over
       * each other with nothing to notice it. */
      {
         mkdir("/tmp/ws-shared", 0700);
         g_last_read_only = -1;
         assert(workspace_turn_bind_container("deleg-ro", NULL, "/tmp/ws-shared", 1) == 1);
         assert(g_last_read_only == 1);
         workspace_turn_unbind_active();

         g_last_read_only = -1;
         assert(workspace_turn_bind_container("deleg-rw", NULL, "/tmp/ws-shared", 0) == 1);
         assert(g_last_read_only == 0);
         workspace_turn_unbind_active();
      }

      /* The shared bound itself: every entrance that can hand a workspace to the
       * backend must go through this ONE check. A second copy is a second thing to
       * forget, which is how the delegate.backend_exec RPC came to accept any host
       * path while the seam refused them. */
      {
         char outp[MAX_PATH_LEN] = "";
         mkdir("/tmp/ws-shared", 0700);
         assert(workspace_turn_workspace_authorized("/tmp/ws-shared", outp, sizeof(outp)) == 1);
         assert(strcmp(outp, "/tmp/ws-shared") == 0); /* canonical, and what to mount */

         mkdir("/tmp/aimee-outside-roots", 0700);
         outp[0] = 'x';
         assert(workspace_turn_workspace_authorized("/tmp/aimee-outside-roots", outp,
                                                    sizeof(outp)) == 0);
         assert(outp[0] == '\0');
         rmdir("/tmp/aimee-outside-roots");

         assert(workspace_turn_workspace_authorized(NULL, outp, sizeof(outp)) == 0);
         assert(workspace_turn_workspace_authorized("", outp, sizeof(outp)) == 0);
         assert(workspace_turn_workspace_authorized("/tmp/does-not-exist-at-all", outp,
                                                    sizeof(outp)) == 0);
      }

      /* A registered root of "/" must NOT authorize the whole host: that is not a
       * workspace registration, it is the absence of one. */
      {
         assert(config_workspace_remove(test_detached_workspace) == 0);
         assert(config_workspace_remove("/tmp/ws-shared") == 0);
         assert(config_workspace_add("/", "shared", NULL, NULL) == 0);

         g_acquires = 0;
         mkdir("/tmp/aimee-root-authorized", 0700);
         assert(workspace_turn_bind_container("deleg-9", NULL, "/tmp/aimee-root-authorized", 1) ==
                -1);
         assert(g_acquires == 0);
         rmdir("/tmp/aimee-root-authorized");

         /* A mirror workspace that cannot be resolved must SAY SO.
          *
          * Every failure path in mirror_reconstruct_cwd used to return 0 in silence.
          * The caller then left the client-side path in place, the request fell
          * through to the workspace runner, waited out WS_RUNNER_OP_MS (600s) and
          * failed with "unavailable through the registered workspace runner" -- an
          * error naming a subsystem that was never the problem, with nothing in the
          * log to contradict it. Diagnosing that took hours and produced three wrong
          * theories. A diagnostic nobody can see is the defect, so assert the line is
          * actually emitted rather than trusting that it was added. */
         {
            assert(config_workspace_remove("/") == 0);
            assert(config_workspace_add("/tmp/ws-mirror-unresolvable", "mirror",
                                        "https://example.invalid/r.git", "") == 0);

            char capture[512];
            snprintf(capture, sizeof(capture), "%s/wsturn-log-XXXXXX", platform_tmpdir());
            int cap_fd = mkstemp(capture);
            assert(cap_fd >= 0);
            fflush(stderr);
            int saved = dup(STDERR_FILENO);
            assert(saved >= 0);
            assert(dup2(cap_fd, STDERR_FILENO) >= 0);

            char out[MAX_PATH_LEN] = "sentinel";
            int rc = workspace_turn_resolve_mirror_cwd("/tmp/ws-mirror-unresolvable/src", out,
                                                       sizeof(out));

            fflush(stderr);
            assert(dup2(saved, STDERR_FILENO) >= 0);
            close(saved);

            assert(rc == 0); /* behaviour unchanged: still refuses to resolve */
            assert(!out[0]); /* and still clears the output */
            workspace_turn_scope_t *failed = NULL;
            int bound = workspace_turn_scope_begin("/tmp/ws-mirror-unresolvable/src", &failed);
            assert(bound < 0 && !failed);
            assert(workspace_turn_reject_foreign_cwd(bound, 1, "/tmp/ws-mirror-unresolvable/src"));

            FILE *f = fopen(capture, "r");
            assert(f);
            char logged[4096] = "";
            size_t n = fread(logged, 1, sizeof(logged) - 1, f);
            logged[n] = '\0';
            fclose(f);
            unlink(capture);
            close(cap_fd);

            /* The point of the change: it is no longer silent. */
            assert(strstr(logged, "mirror resolve") != NULL);
            assert(strstr(logged, "/tmp/ws-mirror-unresolvable") != NULL);
            printf("  unresolvable mirror is reported, not silent: ok\n");
         }

         /* restore the real roots for the cases below */
         register_standard_workspaces();
      }

      /* A tree OUTSIDE every registered workspace root must be refused. Repository-
       * ness is not authorization — `mkdir .git` anywhere would satisfy that — so
       * the bound is the operator's registered roots. Without this, an unlucky or
       * hostile session cwd (a home directory, a secrets tree, /) becomes a
       * read-write bind mount inside a delegate's container. */
      {
         g_acquires = 0;
         mkdir("/tmp/aimee-unregistered-tree", 0700);
         assert(workspace_turn_bind_container("deleg-8", NULL, "/tmp/aimee-unregistered-tree", 0) ==
                -1);
         assert(g_acquires == 0); /* refused BEFORE taking a container */
         assert(workspace_provider_active() == shared);
         rmdir("/tmp/aimee-unregistered-tree");
      }

      /* Acquire failure must NOT bind: falling through with a half-bound provider
       * would send every op to the host. It refuses (-1) — there is no in-process
       * path left to fall back to, so the delegation aborts. */
      {
         g_acquires = g_releases = 0;
         g_acquire_fails = 1;
         assert(workspace_turn_bind_container("deleg-3", NULL, test_shared_workspace, 0) == -1);
         assert(g_acquires == 1);
         assert(g_releases == 0); /* nothing to release: it never took one */
         assert(workspace_provider_active() == shared);
         g_acquire_fails = 0;
      }

      /* An empty task_id is refused before any container is taken. */
      {
         g_acquires = 0;
         assert(workspace_turn_bind_container("", NULL, NULL, 0) == 0);
         assert(workspace_turn_bind_container(NULL, NULL, NULL, 0) == 0);
         assert(g_acquires == 0);
      }

      /* No docker backend registered at all: there is nothing to bind to. Must
       * refuse rather than silently run on the host. */
      {
         delegate_backend_reset_for_test();
         g_acquires = 0;
         assert(workspace_turn_bind_container("deleg-4", NULL, test_shared_workspace, 0) == -1);
         assert(workspace_provider_active() == shared);
      }
      delegate_backend_reset_for_test();
   }

   test_owned_mirror_callback_lease(tmpdir);
   printf("workspace_turn: all tests passed\n");
   return 0;
}
