"""Compile the production provisioning path and existing model Git runner against boundary doubles.

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
    match = re.search(r"^(?:static )?(?:int |void |char \*)" + name + r"\(", source, re.M)
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
static char *(*g_worktree_git_runner)(const char *, int *);
static const char *repo;
#define GIT_BUF_SIZE 65536
#define MCP_GIT_TIMEOUT_MS GIT_NET_TIMEOUT_MS
#define SAFE_EXEC_TIMEOUT -2
#define LOG_WARN(...) ((void)0)
static char current_cwd[MAX_PATH_LEN];
static void run_cmd_set_cwd(const char *cwd) {
    snprintf(current_cwd, sizeof(current_cwd), "%s", cwd ? cwd : "");
}
static const char *run_cmd_get_cwd(void) { return current_cwd[0] ? current_cwd : NULL; }
static int vault_mode = 1, exec_result = 0, calls = 0, ambient_calls = 0, last_fd = -1;
static const char *config_session_worktree_base(void) { return "remote_default"; }
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
    assert(!who && !remote && !preferred && parent == environ);
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
    assert(strcmp(argv[0], "/bin/sh") == 0 && strcmp(argv[1], "-c") == 0);
    assert(strcmp(cwd, repo) == 0);
    assert(env && cap > 0 && timeout == GIT_NET_TIMEOUT_MS);
    assert(target == (fd >= 0 ? GIT_CRED_TOKEN_TARGET_FD : -1));
    if (vault_mode == 1) assert(fd >= 0 && fcntl(fd, F_GETFD) >= 0);
    if (exec_result) { *out = strdup("synthetic fetch failure"); return exec_result; }
    char cmd[8192]; snprintf(cmd, sizeof(cmd), "cd '%s' && %s", cwd, argv[2]);
    int rc; *out = run_cmd(cmd, &rc); return rc;
}
typedef struct workspace_provider workspace_provider_t;
struct workspace_provider {
    int kind;
    char *(*exec_shell_timeout)(const workspace_provider_t *, const char *, int, int *);
    char *(*exec_shell)(const workspace_provider_t *, const char *, int *);
};
static char *ambient_shell(const workspace_provider_t *ws, const char *cmd, int ms, int *rc) {
    (void)ws; assert(ms == MCP_GIT_TIMEOUT_MS); ambient_calls++;
    if (vault_mode) { *rc = 128; return strdup("authentication required"); }
    char line[8192]; snprintf(line, sizeof(line), "cd '%s' && %s", run_cmd_get_cwd(), cmd);
    return run_cmd(line, rc);
}
static const workspace_provider_t provider = {0, ambient_shell, NULL};
static const workspace_provider_t *workspace_provider_active(void) { return &provider; }
static const workspace_provider_t *workspace_provider_shared(void) { return &provider; }
static int owns_workspace = 1;
static int forge_workspace_for_cwd(const char *cwd, int kind, int *on_server, char *id, size_t cap) {
    assert(cwd && strcmp(cwd, repo) == 0 && kind == 0); *on_server = 1;
    snprintf(id, cap, "%s", cwd); return owns_workspace ? 0 : 1;
}
static const char *workspace_remote_for_root(const char *root) {
    assert(strcmp(root, repo) == 0); return NULL;
}
'''

MAIN = r'''
int main(int argc, char **argv) {
    assert(argc == 3); repo = argv[1]; const char *tip = argv[2];
    char selected[192], oid[96]; int enforce;
    /* Original ambient-only path fails on a vault-only daemon. */
    assert(wt_session_bases(repo, selected, sizeof(selected), oid, sizeof(oid), &enforce) == -1);
    assert(ambient_calls == 1 && calls == 0);
    worktree_register_git_runner(mcp_git_run);
    run_cmd_set_cwd("/previous/turn");
    assert(wt_session_bases(repo, selected, sizeof(selected), oid, sizeof(oid), &enforce) == 0);
    assert(strcmp(selected, tip) == 0 && strcmp(oid, tip) == 0 && enforce == 1);
    assert(calls == 2 && ambient_calls == 1);
    assert(strcmp(run_cmd_get_cwd(), "/previous/turn") == 0);
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF);
    exec_result = 128;
    assert(wt_session_bases(repo, selected, sizeof(selected), oid, sizeof(oid), &enforce) == -1);
    assert(ambient_calls == 1); /* no independent retry bypassing model tooling */
    assert(strcmp(run_cmd_get_cwd(), "/previous/turn") == 0);
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF);
    run_cmd_set_cwd(NULL);
    exec_result = SAFE_EXEC_TIMEOUT;
    assert(worktree_fetch(repo, 1) == SAFE_EXEC_TIMEOUT);
    assert(run_cmd_get_cwd() == NULL);
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF);
    exec_result = 0;
    vault_mode = 2; /* existing runner's SSH-only path */
    assert(worktree_fetch(repo, 1) == 0);
    vault_mode = 1;
    owns_workspace = 0; /* existing model workspace policy must still apply */
    int before = calls;
    assert(worktree_fetch(repo, 1) == 128 && calls == before);
    assert(run_cmd_get_cwd() == NULL);
    owns_workspace = 1;
    vault_mode = 0; /* model runner's ambient fallback */
    assert(worktree_fetch(repo, 1) == 0);
    worktree_register_git_runner(NULL); /* CLI remains independent of server tooling */
    assert(wt_session_bases(repo, selected, sizeof(selected), oid, sizeof(oid), &enforce) == 0);
    puts("PASS: existing model Git runner, fresh tip, vault-only auth, workspace policy, error/timeout, cwd restoration, CLI fallback");
    return 0;
}
'''



class WorkspaceGitCredentials(unittest.TestCase):
    def test_vault_only_session_start(self):
        workspace = (ROOT / "src/modules/workspace/workspace.c").read_text()
        server = (ROOT / "src/server/server_main.c").read_text()
        # Registration is a lifecycle requirement: the executable test below
        # exercises the same bridge, but cannot boot the entire server.
        self.assertIn("worktree_register_git_runner(mcp_git_run);", server)
        self.assertNotIn("server_worktree_git_network", server)
        git = (ROOT / "src/modules/git/mcp_git_query.c").read_text()
        source = PRELUDE + "\n" + function(git, "mcp_git_timeout_result")
        source += "\n" + function(git, "mcp_git_run")
        for name in ("worktree_register_git_runner", "worktree_fetch",
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
