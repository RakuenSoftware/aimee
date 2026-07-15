/* test_workspace_turn.c: a turn whose cwd is inside a registered `detached`
 * workspace binds the active provider to a detached provider; shared workspaces
 * and unregistered cwds stay on the shared provider. Config-backed (the binder
 * reads the registered providers via config_load). */
#include "workspace_turn.h"
#include "workspace_provider.h"
#include "config.h"
#include "delegate_backend.h"
#include "workspace_provider_container.h"
#include "aimee_home.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── fake docker backend for the delegate-sandbox cases ───────────────────── */

static int g_acquires, g_releases, g_last_hibernate, g_acquire_fails;
static char g_last_workspace[512];
static int g_last_read_only;
static int g_fake_state = 1;

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

int main(void)
{
   /* Isolated temp HOME so config_save/load never touch the real config. */
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-wsturn-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");

   /* Register two workspaces: one detached, one shared (default). */
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_load(&cfg);
   cfg.workspace_count = 2;
   snprintf(cfg.workspaces[0], MAX_PATH_LEN, "/tmp/ws-detached");
   snprintf(cfg.workspace_providers[0], sizeof(cfg.workspace_providers[0]), "detached");
   snprintf(cfg.workspaces[1], MAX_PATH_LEN, "/tmp/ws-shared");
   cfg.workspace_providers[1][0] = '\0';
   assert(config_save(&cfg) == 0);

   const workspace_provider_t *shared = workspace_provider_shared();

   /* cwd inside the detached workspace -> binds a detached provider active */
   {
      assert(workspace_provider_active() == shared); /* default */
      int bound = workspace_turn_bind_active("/tmp/ws-detached/src/file.c");
      assert(bound == 1);
      const workspace_provider_t *active = workspace_provider_active();
      assert(active != shared && active->kind == WS_PROVIDER_DETACHED);
      workspace_turn_unbind_active();
      assert(workspace_provider_active() == shared); /* restored */
   }

   /* the workspace root itself also matches */
   {
      assert(workspace_turn_bind_active("/tmp/ws-detached") == 1);
      assert(workspace_provider_active()->kind == WS_PROVIDER_DETACHED);
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

      /* Dial OFF (the default): no container, no acquire, and the active provider
       * is untouched — the turn runs in-process exactly as it does today. */
      {
         config_t c;
         memset(&c, 0, sizeof(c));
         config_load(&c);
         c.delegate_sandbox = 0;
         assert(config_save(&c) == 0);
         g_acquires = g_releases = 0;
         assert(workspace_turn_bind_container("deleg-1", NULL, NULL, 0) == 0);
         assert(g_acquires == 0); /* must not even try to take a container */
         assert(workspace_provider_active() == shared);
      }

      /* Dial ON: acquires a container and binds a CONTAINER provider — not shared.
       * If this ever resolved to `shared` the delegate would be on the host while
       * the operator believed it was sandboxed. */
      {
         config_t c;
         memset(&c, 0, sizeof(c));
         config_load(&c);
         c.delegate_sandbox = 1;
         assert(config_save(&c) == 0);
         g_acquires = g_releases = 0;
         assert(workspace_turn_bind_container("deleg-2", NULL, NULL, 0) == 1);
         assert(g_acquires == 1);
         const workspace_provider_t *p = workspace_provider_active();
         assert(p != shared);
         assert(p->kind == WS_PROVIDER_CONTAINER);

         /* Unbind must RELEASE the container, not just drop the pointer: a leaked
          * container outlives its turn and pins its workspace. And the active
          * provider must go back to shared — a pooled worker thread that kept the
          * binding would run the NEXT delegate's tools in a dead container. */
         workspace_turn_unbind_active();
         assert(g_releases == 1);
         assert(workspace_provider_active() == shared);
      }

      /* The tree reaches the backend. Without this the backend mints an EMPTY
       * scratch dir and mounts that — the delegate opens the file named in its
       * task and finds nothing, then reasons about code it cannot see. */
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

      /* No tree given: the backend keeps its historical empty scratch dir. Passed
       * as NULL rather than "" so the backend can tell "use your default" from a
       * caller that meant a path and computed an empty string. */
      {
         g_acquires = 0;
         g_last_workspace[0] = 'x';
         assert(workspace_turn_bind_container("deleg-6", NULL, NULL, 0) == 1);
         assert(g_last_workspace[0] == '\0');
         workspace_turn_unbind_active();
      }
      {
         g_last_workspace[0] = 'x';
         assert(workspace_turn_bind_container("deleg-7", NULL, "", 0) == 1);
         assert(g_last_workspace[0] == '\0'); /* "" is not a path: same as NULL */
         workspace_turn_unbind_active();
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
         config_t c;
         memset(&c, 0, sizeof(c));
         config_load(&c);
         c.workspace_count = 1;
         snprintf(c.workspaces[0], MAX_PATH_LEN, "/");
         c.workspace_providers[0][0] = '\0';
         c.delegate_sandbox = 1;
         assert(config_save(&c) == 0);

         g_acquires = 0;
         mkdir("/tmp/aimee-root-authorized", 0700);
         assert(workspace_turn_bind_container("deleg-9", NULL, "/tmp/aimee-root-authorized", 1) ==
                0);
         assert(g_acquires == 0);
         rmdir("/tmp/aimee-root-authorized");

         /* restore the real roots for the cases below */
         memset(&c, 0, sizeof(c));
         config_load(&c);
         c.workspace_count = 2;
         snprintf(c.workspaces[0], MAX_PATH_LEN, "/tmp/ws-detached");
         snprintf(c.workspace_providers[0], sizeof(c.workspace_providers[0]), "detached");
         snprintf(c.workspaces[1], MAX_PATH_LEN, "/tmp/ws-shared");
         c.workspace_providers[1][0] = '\0';
         c.delegate_sandbox = 1;
         assert(config_save(&c) == 0);
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
                0);
         assert(g_acquires == 0); /* refused BEFORE taking a container */
         assert(workspace_provider_active() == shared);
         rmdir("/tmp/aimee-unregistered-tree");
      }

      /* Acquire failure must NOT bind: falling through with a half-bound provider
       * would send every op to the host. It returns 0, so the caller runs
       * in-process (and the seam logs at ERROR). */
      {
         g_acquires = g_releases = 0;
         g_acquire_fails = 1;
         assert(workspace_turn_bind_container("deleg-3", NULL, NULL, 0) == 0);
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

      /* No docker backend registered at all: the dial is on but there is nothing
       * to bind to. Must refuse rather than silently run on the host. */
      {
         delegate_backend_reset_for_test();
         g_acquires = 0;
         assert(workspace_turn_bind_container("deleg-4", NULL, NULL, 0) == 0);
         assert(workspace_provider_active() == shared);
      }
      delegate_backend_reset_for_test();
   }

   printf("workspace_turn: all tests passed\n");
   return 0;
}
