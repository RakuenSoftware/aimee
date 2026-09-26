"""Compile the production provisioning/credential bridge against boundary doubles.

No running daemon or real credentials needed. Real local Git repositories verify
that the fresh remote default is selected. The exec double requires the vault
FD, reproducing a daemon whose bootstrap scrubbed ambient credentials.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    """Read one complete C definition (these bodies contain balanced braces)."""
    import re
    match = re.search(r"^(?:static )?(?:int|void) " + name + r"\(", source, re.M)
    if not match:
        raise AssertionError(f"missing production function {name}")
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define MAX_PATH_LEN 4096
#define GIT_NET_TIMEOUT_MS 30000
#define GIT_CRED_TOKEN_TARGET_FD 21
extern char **environ;
typedef int (*worktree_git_network_runner_fn)(const char *, const char *const *, char **, size_t);
static worktree_git_network_runner_fn g_worktree_git_network_runner;
static const char *repo, *principal = "webuser:alice";
static int vault_mode = 1, exec_result = 0, calls = 0, ambient_calls = 0, last_fd = -1;
static const char *config_session_worktree_base(void) { return "remote_default"; }
static const char *agent_get_request_vault_principal(void) { return principal; }
static char *run_cmd(const char *cmd, int *rc) {
    FILE *p = popen(cmd, "r"); assert(p);
    char buf[8192]; size_t n = fread(buf, 1, sizeof(buf)-1, p); buf[n] = 0;
    int status = pclose(p); *rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return strdup(buf);
}
static int real_git(const char *cwd, const char *const *args, char **out) {
    /* Only fixed fixture paths and fixed production fetch arguments reach this double. */
    char cmd[8192]; int n = snprintf(cmd, sizeof(cmd), "git -C '%s'", cwd);
    for (int i = 0; args[i]; i++) n += snprintf(cmd+n, sizeof(cmd)-n, " '%s'", args[i]);
    int rc; char *result = run_cmd(cmd, &rc);
    if (out) *out = result; else free(result);
    return rc;
}
static int git_net_exec(const char *cwd, const char *const *args, char **out, size_t cap) {
    (void)cap; ambient_calls++;
    if (vault_mode) { if (out) *out = strdup("authentication required"); return 128; }
    return real_git(cwd, args, out);
}
static char **git_cred_inject_build_env_for_repo(const char *who, const char *remote,
    const char *cwd, const char *preferred, char *const *parent, int *fd) {
    assert(who == principal && !remote && !preferred && parent == environ);
    assert(strcmp(cwd, repo) == 0);
    *fd = -1;
    if (!vault_mode) return NULL;
    char **env = calloc(2, sizeof(char *)); assert(env);
    env[0] = strdup("AIMEE_GIT_TOKEN_FD=21");
    if (vault_mode == 1) { *fd = open("/dev/null", O_RDONLY | O_CLOEXEC); assert(*fd >= 0); }
    last_fd = *fd;
    return env;
}
static void git_cred_inject_free_env(char **env) {
    if (env) { free(env[0]); free(env); }
}
static int safe_exec_capture_cwd_env_fd_timeout(const char *const *argv, const char *cwd,
    char *const *env, char **out, size_t cap, int timeout, int fd, int target) {
    calls++;
    assert(strcmp(argv[0], "git") == 0 && strcmp(cwd, repo) == 0);
    assert(env && cap > 0 && timeout == GIT_NET_TIMEOUT_MS);
    assert(target == (fd >= 0 ? GIT_CRED_TOKEN_TARGET_FD : -1));
    if (vault_mode == 1) assert(fd >= 0 && fcntl(fd, F_GETFD) >= 0);
    if (exec_result) { *out = strdup("synthetic fetch failure"); return exec_result; }
    return real_git(cwd, argv+1, out);
}
'''

MAIN = r'''
int main(int argc, char **argv) {
    assert(argc == 3); repo = argv[1]; const char *tip = argv[2];
    char selected[192], oid[96]; int enforce;
    /* Before server registration: scrubbed ambient env cannot authenticate. */
    assert(wt_session_bases(repo, selected, sizeof(selected), oid, sizeof(oid), &enforce) == -1);
    assert(ambient_calls == 1 && calls == 0);
    worktree_register_git_network_runner(server_worktree_git_network);
    assert(wt_session_bases(repo, selected, sizeof(selected), oid, sizeof(oid), &enforce) == 0);
    assert(strcmp(selected, tip) == 0 && strcmp(oid, tip) == 0 && enforce == 1);
    assert(calls == 2 && ambient_calls == 1);
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF);
    /* A second request must resolve its own principal, not reuse the first's. */
    principal = "webuser:bob";
    exec_result = 128;
    assert(wt_session_bases(repo, selected, sizeof(selected), oid, sizeof(oid), &enforce) == -1);
    assert(ambient_calls == 1); /* authenticated failure never retries weaker credentials */
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF);
    const char *args[] = {"fetch", "--quiet", "origin", "HEAD", NULL};
    char *out = NULL;
    exec_result = -2; /* timeout/error is propagated unchanged, with FD closed */
    assert(server_worktree_git_network(repo, args, &out, 123) == -2);
    assert(out && strcmp(out, "synthetic fetch failure") == 0); free(out);
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF);
    exec_result = 0;
    vault_mode = 2; /* SSH-only credentials: no HTTPS token FD */
    assert(server_worktree_git_network(repo, args, NULL, 0) == 0);
    vault_mode = 0; /* no vault entry: bounded ambient runner remains available */
    assert(server_worktree_git_network(repo, args, NULL, 0) == 0);
    assert(ambient_calls == 2);
    worktree_register_git_network_runner(NULL); /* thin client/CLI */
    assert(wt_session_bases(repo, selected, sizeof(selected), oid, sizeof(oid), &enforce) == 0);
    assert(ambient_calls == 4);
    puts("PASS: vault-only startup, fresh default, per-request identity, failure/timeout, FD cleanup, SSH and CLI fallback");
    return 0;
}
'''


class WorkspaceGitCredentials(unittest.TestCase):
    def test_vault_only_session_start(self):
        workspace = (ROOT / "src/modules/workspace/workspace.c").read_text()
        server = (ROOT / "src/server/server_main.c").read_text()
        # Registration is a lifecycle requirement: the executable test below
        # exercises the same bridge, but cannot boot the entire server.
        self.assertIn("worktree_register_git_network_runner(server_worktree_git_network);", server)
        source = PRELUDE + "\n" + function(server, "server_worktree_git_network")
        for name in ("worktree_register_git_network_runner", "worktree_git_network",
                     "wt_resolve_candidate", "wt_ref_oid", "wt_session_bases"):
            source += "\n" + function(workspace, name)
        source += MAIN
        with tempfile.TemporaryDirectory(prefix="aimee-workspace-credentials-") as tmp:
            tmp = Path(tmp)
            env = os.environ.copy()
            env.pop("AIMEE_SESSION_WORKTREE_BASE", None)
            env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
            def git(*args):
                return subprocess.check_output(["git", *map(str, args)], env=env,
                                               stderr=subprocess.DEVNULL, text=True).strip()
            origin, checkout = tmp / "origin", tmp / "checkout"
            git("init", "--initial-branch=main", origin)
            git("-C", origin, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                "commit", "--allow-empty", "-m", "initial")
            git("clone", origin, checkout)
            # The clone is stale: startup must obtain this newer remote commit.
            git("-C", origin, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                "commit", "--allow-empty", "-m", "new remote tip")
            tip = git("-C", origin, "rev-parse", "HEAD")
            cfile, binary = tmp / "test.c", tmp / "test"
            cfile.write_text(source)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                            "-Werror", str(cfile), "-o", str(binary)], check=True)
            subprocess.run([str(binary), str(checkout), tip], env=env, check=True)


if __name__ == "__main__":
    unittest.main()
