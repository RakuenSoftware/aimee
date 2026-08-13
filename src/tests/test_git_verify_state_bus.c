/* test_git_verify_state_bus.c: the verify ledger, proven across the real bus.
 *
 * The C seam in git_verify_state.c no longer does the work; it asks the git
 * module. Every other test of that seam links module_bus_stub, whose honest
 * default is "no module attached" -- useful for proving the seam fails closed,
 * useless for proving it is correct. Correctness needs the actual module, and
 * the rollout proposal asks for exactly that: behaviour shown to be equivalent
 * THROUGH the bus, not beside it.
 *
 * So this fixture stands up the daemon's authenticated module endpoint, execs
 * the real Go binary against it, and drives the ordinary C entry points. What
 * it proves is agreement with git itself and round-trip fidelity of the
 * ledger -- the same claims the in-process implementation used to carry.
 *
 * The module is a separate program, so a failure to start is a failure of the
 * test, never a skip: a skip here would silently retire the only coverage the
 * ledger has.
 */
#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <aimee/audit/obs_bus.h>
#include <aimee/git/module_api.h>
#include "modules/git/git_verify.h"
#include "modules/git/git_verify_internal.h"

static char g_tmp[512];
static pid_t g_module = -1;

static void must(int condition, const char *what)
{
   if (condition)
      return;
   fprintf(stderr, "FAIL: %s (errno=%s)\n", what, strerror(errno));
   if (g_module > 0)
      kill(g_module, SIGKILL);
   abort();
}

/* Run a command and return its trimmed first line, or NULL. Used only to ask
 * git for the truth the module's answer is compared against. */
static char *shell(const char *format, ...)
{
   char command[2048];
   va_list args;
   va_start(args, format);
   vsnprintf(command, sizeof(command), format, args);
   va_end(args);

   FILE *pipe = popen(command, "r");
   if (!pipe)
      return NULL;
   static char buffer[512];
   char *line = fgets(buffer, sizeof(buffer), pipe);
   int status = pclose(pipe);
   if (!line || status != 0)
      return NULL;
   buffer[strcspn(buffer, "\r\n")] = '\0';
   return buffer[0] ? buffer : NULL;
}

/* Copy the multicall binary to the name the module runtime will pin. main.go
 * derives the module from argv[0]'s basename, and the grant is checked against
 * the peer's resolved executable path -- so this must be a real file at that
 * name, not a symlink back to the shared binary. */
static void install_module_binary(const char *source, const char *destination)
{
   must(shell("cp '%s' '%s' && chmod 0755 '%s'", source, destination, destination) != NULL ||
            access(destination, X_OK) == 0,
        "copy the module binary");
   must(access(destination, X_OK) == 0, "module binary is executable");
}

static void write_grant(const char *policy_dir, const char *executable)
{
   char path[640];
   snprintf(path, sizeof(path), "%s/git.grant", policy_dir);
   FILE *file = fopen(path, "w");
   must(file != NULL, "open the grant manifest");
   /* The serve set is the contract's, not a convenient subset: a grant that
    * omitted a declared stage would make an unserved stage indistinguishable
    * from an ungranted one. */
   fprintf(file,
           "version=1\nprincipal_class=1\nprincipal_ref=13\nuid=self\n"
           "executable=%s\nserve=%u,%u,%u,%u\n",
           executable, AIMEE_GIT_EVENT_OPERATION, AIMEE_GIT_EVENT_REF_VALIDATE,
           AIMEE_GIT_EVENT_CI_GRADE, AIMEE_GIT_EVENT_VERIFY_RUN);
   must(fclose(file) == 0, "write the grant manifest");
}

static void start_module(const char *executable, const char *socket_path)
{
   pid_t parent = getpid();
   pid_t child = fork();
   must(child >= 0, "fork the module");
   if (child == 0)
   {
      /* Outlive the test and this module runs forever: cleanup here is
       * atexit-shaped and does not run when the test dies by a signal. */
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent)
         _exit(0);
      execl(executable, executable, socket_path, (char *)NULL);
      _exit(127);
   }
   g_module = child;

   /* Attachment is asynchronous. Poll availability rather than sleeping: the
    * point of the fixture is that a real process really registered. */
   for (int tick = 0; tick < 200; tick++)
   {
      if (obs_bus_module_available(AIMEE_GIT_EVENT_VERIFY_RUN))
         return;
      int status = 0;
      if (waitpid(child, &status, WNOHANG) == child)
      {
         g_module = -1;
         must(0, "module exited before it attached");
      }
      struct timespec pause = {0, 50 * 1000 * 1000};
      nanosleep(&pause, NULL);
   }
   must(0, "module never registered the verify stage");
}

static void stop_module(void)
{
   if (g_module <= 0)
      return;
   kill(g_module, SIGTERM);
   waitpid(g_module, NULL, 0);
   g_module = -1;
}

/* The hashes must be git's, not merely self-consistent: a module that returned
 * a stable wrong value would pass a round-trip test and fail every real gate. */
static void test_hashes_match_git(const char *repo)
{
   char *expected_tree = shell("git -C '%s' rev-parse HEAD^{tree}", repo);
   must(expected_tree != NULL, "git produced a tree hash");
   char tree_copy[128];
   snprintf(tree_copy, sizeof(tree_copy), "%s", expected_tree);

   char *expected_commit = shell("git -C '%s' rev-parse HEAD", repo);
   must(expected_commit != NULL, "git produced a commit hash");

   char *tree = verify_compute_file_hash(repo);
   must(tree != NULL && strcmp(tree, tree_copy) == 0, "tree hash matches git");
   char *commit = verify_compute_commit_hash(repo);
   must(commit != NULL && strcmp(commit, expected_commit) == 0, "commit hash matches git");
   free(tree);
   free(commit);

   must(verify_worktree_has_changes(repo) == 0, "a fresh checkout is clean");
   must(shell("touch '%s/dirty.txt'", repo) != NULL || access(repo, F_OK) == 0, "dirty the tree");
   must(verify_worktree_has_changes(repo) == 1, "an untracked file makes the tree dirty");
   must(shell("rm -f '%s/dirty.txt'", repo) != NULL || 1, "undirty the tree");
}

static void test_ledger_round_trip(const char *repo)
{
   verify_state_entry_t entries[VERIFY_STATE_MAX];
   must(read_verify_entries(repo, entries, VERIFY_STATE_MAX) == 0, "a fresh repo has no ledger");

   const char *hash = "1111111111111111111111111111111111111111";
   must(write_verify_state(repo, 1700000000, hash, 1, 4, "lint:0,build:1") == 0,
        "write the ledger");

   int count = read_verify_entries(repo, entries, VERIFY_STATE_MAX);
   must(count == 1, "the written entry reads back");
   must(entries[0].ts == 1700000000, "timestamp survives the round trip");
   must(strcmp(entries[0].hash, hash) == 0, "hash survives the round trip");
   must(entries[0].failed == 1 && entries[0].total == 4, "counters survive the round trip");
   must(strcmp(entries[0].step_results, "lint:0,build:1") == 0, "step results survive");

   must(find_verify_entry(entries, count, hash) == 0, "the entry is findable by hash");

   /* The step-results string is still produced and read by C; the module
    * carries it opaquely. Proving the lookup works on a string that made the
    * round trip is what shows the two sides agree on it. */
   int rc = -1;
   must(verify_state_step_result_lookup(entries[0].step_results, "build", &rc) == 1 && rc == 1,
        "a step's recorded exit code survives");

   /* Re-verifying the same tree replaces rather than appends. */
   must(write_verify_state(repo, 1700000100, hash, 0, 4, "") == 0, "rewrite the same tree");
   count = read_verify_entries(repo, entries, VERIFY_STATE_MAX);
   must(count == 1 && entries[0].ts == 1700000100, "the same tree is replaced, not duplicated");
}

/* The pre-push hook runs from the main checkout, so state recorded in a linked
 * worktree has to be visible there. This is the one behaviour that depends on
 * the module resolving the repository the same way C used to. */
static void test_worktree_sharing(const char *repo)
{
   /* Inside the fixture directory, so it goes away with everything else. A
    * path outside it survives the run and the next one silently adopts a
    * worktree whose repository no longer exists. */
   char linked[640];
   snprintf(linked, sizeof(linked), "%s/linked", g_tmp);
   must(access(linked, F_OK) != 0, "the linked worktree path is unused");
   (void)shell("git -C '%s' worktree add -q -b side '%s' 2>&1", repo, linked);
   must(access(linked, F_OK) == 0, "add a linked worktree");

   const char *hash = "2222222222222222222222222222222222222222";
   must(write_verify_state(linked, 1700000200, hash, 0, 2, "") == 0, "write from the worktree");

   verify_state_entry_t entries[VERIFY_STATE_MAX];
   int count = read_verify_entries(repo, entries, VERIFY_STATE_MAX);
   must(find_verify_entry(entries, count, hash) >= 0,
        "the main checkout sees the linked worktree's entry");
}

/* With the module gone the seam must report "nothing verified" -- the state
 * that forces a verify run. Reporting a verified tree here would let an
 * unverified push through on a broken deployment. */
static void test_absent_module_fails_closed(const char *repo)
{
   stop_module();
   for (int tick = 0; tick < 200 && obs_bus_module_available(AIMEE_GIT_EVENT_VERIFY_RUN); tick++)
   {
      struct timespec pause = {0, 50 * 1000 * 1000};
      nanosleep(&pause, NULL);
   }

   verify_state_entry_t entries[VERIFY_STATE_MAX];
   must(read_verify_entries(repo, entries, VERIFY_STATE_MAX) == 0,
        "an absent module reports no verified trees");
   must(verify_compute_file_hash(repo) == NULL, "an absent module yields no tree hash");
   must(write_verify_state(repo, 1, "3333333333333333333333333333333333333333", 0, 1, "") == -1,
        "an absent module fails the write rather than losing it silently");
}

int main(int argc, char **argv)
{
   must(argc >= 2, "usage: test_git_verify_state_bus <path to aimee-module binary>");

   snprintf(g_tmp, sizeof(g_tmp), "/tmp/aimee-verify-bus-%d", (int)getpid());
   char policy[640], socket_path[512], executable[640], repo[640];
   snprintf(policy, sizeof(policy), "%s/policy", g_tmp);
   snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", g_tmp);
   snprintf(executable, sizeof(executable), "%s/aimee-module-git", g_tmp);
   snprintf(repo, sizeof(repo), "%s/repo", g_tmp);
   must(shell("mkdir -p '%s' '%s' '%s'", g_tmp, policy, repo) != NULL || access(g_tmp, F_OK) == 0,
        "create the fixture directories");

   install_module_binary(argv[1], executable);
   write_grant(policy, executable);

   must(obs_bus_configure_module_runtime(socket_path, policy) == 0,
        "configure the module endpoint");
   must(obs_bus_start() == 0, "start the bus");

   must(shell("git -C '%s' init -q -b main && git -C '%s' config user.email t@example.com && "
              "git -C '%s' config user.name t && git -C '%s' commit -q --allow-empty -m one",
              repo, repo, repo, repo) != NULL ||
            access(repo, F_OK) == 0,
        "create the fixture repository");

   start_module(executable, socket_path);

   test_hashes_match_git(repo);
   test_ledger_round_trip(repo);
   test_worktree_sharing(repo);
   test_absent_module_fails_closed(repo);

   stop_module();
   (void)shell("rm -rf '%s'", g_tmp);
   printf("git-verify-state-bus: ok (ledger agrees with git across the real module)\n");
   return 0;
}
