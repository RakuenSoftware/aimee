/* test_workspace_mirror.c — the `detached` mirror tier (workspace-resource-plane
 * §3 / AC #5). The drift classifier is pure; the mirror-lifecycle + drift
 * helpers are exercised through a MOCK git runner (no real git), so the test is
 * hermetic. The point of AC #5: drift is detected and SURFACED, never merged. */
#include "modules/workspace/workspace_mirror.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- mock git runner --- */
static char g_last_cmd[512]; /* space-joined args of the last call */
static char g_cmd_log[4096]; /* every command this run, one per line */
static const char *g_rev_parse_head;
static int g_is_ancestor_rc; /* exit for merge-base --is-ancestor (0=yes) */
static int g_fetch_rc;
static int g_clone_rc;
static int g_worktree_rc; /* exit for `worktree add` */
static int g_checkout_rc; /* exit for `checkout` (fallback path) */
static int g_apply_rc;    /* exit for `apply` */

static int mock_git(void *ctx, const char *const args[], char *out, size_t out_cap)
{
   (void)ctx;
   g_last_cmd[0] = '\0';
   for (int i = 0; args[i]; i++)
   {
      strncat(g_last_cmd, i ? " " : "", sizeof(g_last_cmd) - strlen(g_last_cmd) - 1);
      strncat(g_last_cmd, args[i], sizeof(g_last_cmd) - strlen(g_last_cmd) - 1);
   }
   strncat(g_cmd_log, g_last_cmd, sizeof(g_cmd_log) - strlen(g_cmd_log) - 1);
   strncat(g_cmd_log, "\n", sizeof(g_cmd_log) - strlen(g_cmd_log) - 1);
   if (out && out_cap)
      out[0] = '\0';

   /* find the subcommand (first arg that isn't -C/<dir>) */
   for (int i = 0; args[i]; i++)
   {
      if (strcmp(args[i], "rev-parse") == 0)
      {
         if (out && g_rev_parse_head)
            snprintf(out, out_cap, "%s\n", g_rev_parse_head);
         return g_rev_parse_head ? 0 : 1;
      }
      if (strcmp(args[i], "merge-base") == 0)
         return g_is_ancestor_rc;
      if (strcmp(args[i], "fetch") == 0)
         return g_fetch_rc;
      if (strcmp(args[i], "clone") == 0)
         return g_clone_rc;
      if (strcmp(args[i], "worktree") == 0)
         return g_worktree_rc;
      if (strcmp(args[i], "checkout") == 0)
         return g_checkout_rc;
      if (strcmp(args[i], "apply") == 0)
         return g_apply_rc;
   }
   return 0;
}

int main(void)
{
   /* --- pure classifier --- */
   assert(ws_mirror_drift_classify("abc", "abc", 0, 0) == WS_MIRROR_IN_SYNC);
   assert(ws_mirror_drift_classify("c2", "c1", 0, 1) ==
          WS_MIRROR_CLIENT_AHEAD); /* mirror anc client */
   assert(ws_mirror_drift_classify("c1", "c2", 1, 0) ==
          WS_MIRROR_MIRROR_AHEAD); /* client anc mirror */
   assert(ws_mirror_drift_classify("x", "y", 0, 0) == WS_MIRROR_DIVERGED);
   assert(ws_mirror_drift_classify(NULL, "y", 0, 0) == WS_MIRROR_DRIFT_UNKNOWN);
   assert(ws_mirror_drift_classify("x", "", 0, 0) == WS_MIRROR_DRIFT_UNKNOWN);

   assert(strcmp(ws_mirror_drift_name(WS_MIRROR_IN_SYNC), "in_sync") == 0);
   assert(strcmp(ws_mirror_drift_name(WS_MIRROR_DIVERGED), "diverged") == 0);
   assert(strcmp(ws_mirror_drift_name(WS_MIRROR_CLIENT_AHEAD), "client_ahead") == 0);

   /* --- ensure: clone --mirror when absent (no objects/ in a bogus dir) --- */
   g_clone_rc = 0;
   assert(workspace_mirror_ensure(mock_git, NULL, "https://host/r.git",
                                  "/nonexistent/mirror/dir") == 0);
   assert(strstr(g_last_cmd, "clone --mirror https://host/r.git") != NULL);

   /* --- head read via rev-parse --- */
   g_rev_parse_head = "deadbeef";
   char head[128];
   assert(workspace_mirror_head(mock_git, NULL, "/m", "HEAD", head, sizeof(head)) == 0);
   assert(strcmp(head, "deadbeef") == 0); /* trailing newline trimmed */

   /* --- drift: identical heads → in_sync (no ancestry calls needed) --- */
   g_rev_parse_head = "samehead";
   assert(workspace_mirror_drift(mock_git, NULL, "/m", "HEAD", "samehead") == WS_MIRROR_IN_SYNC);

   /* --- drift: client ahead (mirror head is an ancestor of client) --- */
   g_rev_parse_head = "mhead";
   g_is_ancestor_rc = 0; /* every --is-ancestor returns "yes" in this simple mock... */
   /* ...so refine: classify directly covers the matrix; here just confirm the
    * drift path reads the mirror head and returns a non-unknown verdict. */
   ws_mirror_drift_t d = workspace_mirror_drift(mock_git, NULL, "/m", "HEAD", "chead");
   assert(d != WS_MIRROR_DRIFT_UNKNOWN && d != WS_MIRROR_IN_SYNC);

   /* --- drift: unknown when mirror head can't be read --- */
   g_rev_parse_head = NULL;
   assert(workspace_mirror_drift(mock_git, NULL, "/m", "HEAD", "chead") == WS_MIRROR_DRIFT_UNKNOWN);

   /* --- reconstruct: fresh worktree at head, then apply the client diff --- */
   g_cmd_log[0] = '\0';
   g_worktree_rc = g_apply_rc = 0;
   assert(workspace_mirror_reconstruct(mock_git, NULL, "/m", "/w", "abc123", "/w/.client.diff") ==
          0);
   assert(strstr(g_cmd_log, "-C /m worktree add --detach --force /w abc123") != NULL);
   assert(strstr(g_cmd_log,
                 "-C /w apply --allow-empty --whitespace=nowarn --binary /w/.client.diff") != NULL);
   assert(strstr(g_cmd_log, "checkout") == NULL); /* add succeeded → no fallback */

   /* --- reconstruct: no diff → clean checkout, NO apply --- */
   g_cmd_log[0] = '\0';
   assert(workspace_mirror_reconstruct(mock_git, NULL, "/m", "/w", "abc123", NULL) == 0);
   assert(strstr(g_cmd_log, "worktree add") != NULL);
   assert(strstr(g_cmd_log, "apply") == NULL);

   /* --- reconstruct: resumed session — worktree add fails, fall back to a hard
    *     checkout + clean in the existing worktree, then apply --- */
   g_cmd_log[0] = '\0';
   g_worktree_rc = 1; /* already populated */
   g_checkout_rc = 0;
   assert(workspace_mirror_reconstruct(mock_git, NULL, "/m", "/w", "abc123", "/w/.client.diff") ==
          0);
   assert(strstr(g_cmd_log, "-C /w checkout --detach --force abc123") != NULL);
   assert(strstr(g_cmd_log, "-C /w clean -fd") != NULL);
   assert(strstr(g_cmd_log, "apply") != NULL);

   /* --- reconstruct: a failing `git apply` fails the reconstruction --- */
   g_cmd_log[0] = '\0';
   g_worktree_rc = 0;
   g_apply_rc = 1; /* conflicting/garbled diff */
   assert(workspace_mirror_reconstruct(mock_git, NULL, "/m", "/w", "abc123", "/w/.client.diff") ==
          -1);
   g_apply_rc = 0;

   /* Git-tool snapshots are independent branch checkouts, not detached shared
    * worktrees, and retain the client's upstream tracking. */
   g_cmd_log[0] = '\0';
   g_clone_rc = g_checkout_rc = 0;
   assert(workspace_mirror_reconstruct_branch(mock_git, NULL, "https://host/r.git", "/m", "/w-2",
                                              "abc123", "fix/live", "origin/fix/live",
                                              "/w/client.diff") == 0);
   assert(strstr(g_cmd_log, "clone --no-checkout --shared /m /w-2") != NULL);
   assert(strstr(g_cmd_log, "-C /w-2 remote set-url origin https://host/r.git") != NULL);
   assert(strstr(g_cmd_log, "-C /w-2 checkout -B fix/live abc123") != NULL);
   assert(strstr(g_cmd_log, "-C /w-2 branch --set-upstream-to origin/fix/live fix/live") != NULL);
   assert(strstr(g_cmd_log,
                 "-C /w-2 apply --allow-empty --whitespace=nowarn --binary /w/client.diff") !=
          NULL);

   /* --- drift_report: surfaced summary lines for each verdict --- */
   char rep[256];
   g_rev_parse_head = "same"; /* in_sync (no ancestry needed) */
   assert(workspace_mirror_drift_report(mock_git, NULL, "/m", "HEAD", "same", rep, sizeof(rep)) ==
          WS_MIRROR_IN_SYNC);
   assert(strstr(rep, "in sync") != NULL);

   g_rev_parse_head = "mhead";
   g_is_ancestor_rc = 0; /* every is-ancestor "yes" → client_ahead per classifier */
   ws_mirror_drift_t dr =
       workspace_mirror_drift_report(mock_git, NULL, "/m", "HEAD", "chead", rep, sizeof(rep));
   assert(dr != WS_MIRROR_IN_SYNC && dr != WS_MIRROR_DRIFT_UNKNOWN);
   assert(strstr(rep, "drift") != NULL && strstr(rep, "mirror") != NULL);

   g_rev_parse_head = NULL; /* unreadable head → unknown */
   assert(workspace_mirror_drift_report(mock_git, NULL, "/m", "HEAD", "chead", rep, sizeof(rep)) ==
          WS_MIRROR_DRIFT_UNKNOWN);
   assert(strstr(rep, "unknown") != NULL);

   /* --- base dir: AIMEE_WORKSPACES_DIR (durable volume) overrides aimee_home --- */
   char b[256];
   setenv("AIMEE_WORKSPACES_DIR", "/mnt/aimee-workspaces", 1);
   assert(workspace_mirror_base(b, sizeof(b)) == 0);
   assert(strcmp(b, "/mnt/aimee-workspaces") == 0); /* used verbatim */
   unsetenv("AIMEE_WORKSPACES_DIR");
   setenv("AIMEE_HOME", "/tmp/mhome", 1); /* fall back to <aimee_home>/workspaces */
   assert(workspace_mirror_base(b, sizeof(b)) == 0);
   assert(strcmp(b, "/tmp/mhome/workspaces") == 0);
   /* a non-absolute AIMEE_WORKSPACES_DIR is ignored (must be an absolute mount) */
   setenv("AIMEE_WORKSPACES_DIR", "relative/dir", 1);
   assert(workspace_mirror_base(b, sizeof(b)) == 0);
   assert(strcmp(b, "/tmp/mhome/workspaces") == 0);
   unsetenv("AIMEE_WORKSPACES_DIR");

   /* --- path derivation: deterministic <base>/<hash>/{mirror,work} --- */
   char md[256], wd[256], md2[256], wd2[256];
   assert(workspace_mirror_paths("/home/u/.config/aimee/workspaces", "/home/u/proj", md, sizeof(md),
                                 wd, sizeof(wd)) == 0);
   assert(strncmp(md, "/home/u/.config/aimee/workspaces/", 33) == 0);
   /* each path ends with its kind suffix under a hashed parent */
   assert(strcmp(md + strlen(md) - 7, "/mirror") == 0);
   assert(strcmp(wd + strlen(wd) - 5, "/work") == 0);
   /* mirror and work share the same hashed parent dir */
   assert(strncmp(md, wd, strlen(wd) - 5) == 0);
   /* same root → stable; distinct root → distinct dir */
   assert(workspace_mirror_paths("/b", "/home/u/proj", md2, sizeof(md2), wd2, sizeof(wd2)) == 0);
   char md3[256], wd3[256];
   assert(workspace_mirror_paths("/b", "/home/u/other", md3, sizeof(md3), wd3, sizeof(wd3)) == 0);
   assert(strcmp(md2, md3) != 0);
   /* truncation → -1 */
   char tiny[8];
   assert(workspace_mirror_paths("/some/long/base", "/r", tiny, sizeof(tiny), wd, sizeof(wd)) ==
          -1);

   /* diff path: same hashed parent as the worktree, named client.diff */
   char dp[256];
   assert(workspace_mirror_diff_path("/b", "/home/u/proj", dp, sizeof(dp)) == 0);
   assert(strcmp(dp + strlen(dp) - 12, "/client.diff") == 0);
   {
      /* shares the <hash> parent with work_dir from the same (base,root) */
      char md_same[256], wd_same[256];
      assert(workspace_mirror_paths("/b", "/home/u/proj", md_same, sizeof(md_same), wd_same,
                                    sizeof(wd_same)) == 0);
      wd_same[strlen(wd_same) - 5] = '\0'; /* drop "/work" -> <base>/<hash> */
      assert(strncmp(dp, wd_same, strlen(wd_same)) == 0 && dp[strlen(wd_same)] == '/');
   }
   assert(workspace_mirror_diff_path("/b", "/home/u/proj", tiny, sizeof(tiny)) == -1);

   /* A synchronized client snapshot gets immutable generation-qualified paths. */
   char sm[256], sd[256], sw[256];
   assert(workspace_mirror_snapshot_meta_path("/b", "/home/u/proj", sm, sizeof(sm)) == 0);
   const char *digest_a = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   const char *digest_b = "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   assert(workspace_mirror_snapshot_diff_path("/b", "/home/u/proj", 7, digest_a, sd, sizeof(sd)) ==
          0);
   assert(workspace_mirror_snapshot_work_path("/b", "/home/u/proj", 7, digest_a, sw, sizeof(sw)) ==
          0);
   assert(strcmp(sm + strlen(sm) - 16, "/client.snapshot") == 0);
   assert(strstr(sd, "/client.diff-7-") != NULL && strstr(sd, digest_a) != NULL);
   assert(strstr(sw, "/work-7-") != NULL && strstr(sw, digest_a) != NULL);
   char sd_concurrent[256];
   assert(workspace_mirror_snapshot_diff_path("/b", "/home/u/proj", 7, digest_b, sd_concurrent,
                                              sizeof(sd_concurrent)) == 0);
   assert(strcmp(sd, sd_concurrent) != 0); /* concurrent next-gen payloads cannot collide */
   assert(workspace_mirror_snapshot_diff_path("/b", "/home/u/proj", 0, digest_a, sd, sizeof(sd)) ==
          -1);
   ws_mirror_snapshot_t snapshot;
   assert(workspace_mirror_snapshot_parse(
              "7 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef "
              "0123456789abcdef0123456789abcdef01234567 fix/live origin/fix/live\n",
              &snapshot) == 0);
   assert(snapshot.generation == 7);
   assert(strcmp(snapshot.head, "0123456789abcdef0123456789abcdef01234567") == 0);
   assert(strcmp(snapshot.branch, "fix/live") == 0);
   assert(strcmp(snapshot.upstream, "origin/fix/live") == 0);
   assert(workspace_mirror_snapshot_parse(
              "8 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef "
              "0123456789abcdef0123456789abcdef01234567 - -\n",
              &snapshot) == 0);
   assert(snapshot.generation == 8 && snapshot.branch[0] == '\0' && snapshot.upstream[0] == '\0');
   assert(snapshot.sync_order == 0); /* old five-field metadata remains readable */
   assert(workspace_mirror_snapshot_parse(
              "9 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef "
              "0123456789abcdef0123456789abcdef01234567 fix/live origin/fix/live 42\n",
              &snapshot) == 0);
   assert(snapshot.generation == 9 && snapshot.sync_order == 42);
   assert(strcmp(snapshot.branch, "fix/live") == 0);
   const char *sha256_oid = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   assert(workspace_mirror_oid_valid("0123456789abcdef0123456789abcdef01234567") == 1);
   assert(workspace_mirror_oid_valid(sha256_oid) == 1);
   assert(workspace_mirror_oid_valid("ABCDEF0123456789abcdef0123456789abcdef01") == 0);
   assert(workspace_mirror_oid_valid("short") == 0);
   assert(workspace_mirror_ref_valid("") == 1);
   assert(workspace_mirror_ref_valid("feature.locked") == 1);
   assert(workspace_mirror_ref_valid("origin/feature.locked") == 1);
   assert(workspace_mirror_ref_valid("feature.lock") == 0);
   assert(workspace_mirror_ref_valid("team/feature.lock/next") == 0);
   assert(workspace_mirror_ref_valid("team/.hidden") == 0);
   assert(workspace_mirror_ref_valid("-option") == 0);
   assert(workspace_mirror_ref_valid("bad..ref") == 0);
   assert(workspace_mirror_ref_valid("bad@{ref") == 0);
   assert(workspace_mirror_ref_valid("bad//ref") == 0);
   assert(workspace_mirror_ref_valid("bad ref") == 0);
   char sha256_metadata[512];
   snprintf(sha256_metadata, sizeof(sha256_metadata), "%d %s %s main origin/main %d\n", 10,
            digest_a, sha256_oid, 43);
   assert(workspace_mirror_snapshot_parse(sha256_metadata, &snapshot) == 0);
   assert(strcmp(snapshot.head, sha256_oid) == 0 && snapshot.sync_order == 43);
   assert(workspace_mirror_snapshot_parse(
              "9 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0 "
              "0123456789abcdef0123456789abcdef01234567 - -\n",
              &snapshot) == -1); /* 65-char digest is never truncated into validity */
   assert(workspace_mirror_snapshot_parse("7 bad bad", &snapshot) == -1);

   /* Chunk ownership is durable and process-independent: every request derives
    * the same transfer-specific assembly/state paths from wire coordinates,
    * while another transfer cannot collide with them. */
   char ta[256], ts[256], tl[256], tc[256], ta2[256], ts2[256], tl2[256], tc2[256];
   const char *transfer_a = "00112233445566778899aabbccddeeff";
   const char *transfer_b = "10112233445566778899aabbccddeeff";
   assert(workspace_mirror_transfer_paths("/b", "/home/u/proj", transfer_a, ta, sizeof(ta), ts,
                                          sizeof(ts), tl, sizeof(tl), tc, sizeof(tc)) == 0);
   assert(workspace_mirror_transfer_paths("/b", "/home/u/proj", transfer_b, ta2, sizeof(ta2), ts2,
                                          sizeof(ts2), tl2, sizeof(tl2), tc2, sizeof(tc2)) == 0);
   assert(strcmp(ta, ta2) != 0 && strcmp(ts, ts2) != 0);
   assert(strcmp(tl, tl2) == 0 && strcmp(tc, tc2) == 0);
   assert(strstr(ta, transfer_a) != NULL && strstr(ts, transfer_a) != NULL);
   assert(workspace_mirror_transfer_paths("/b", "/home/u/proj", "bad", ta, sizeof(ta), ts,
                                          sizeof(ts), tl, sizeof(tl), tc, sizeof(tc)) == -1);

   ws_mirror_transfer_state_t transfer_state;
   assert(workspace_mirror_transfer_state_parse("41 3\n", &transfer_state) == 0);
   assert(transfer_state.order == 41 && transfer_state.next_seq == 3);
   assert(workspace_mirror_transfer_state_parse("41 0\n", &transfer_state) == -1);
   assert(workspace_mirror_transfer_state_parse("41 3 extra\n", &transfer_state) == -1);

   /* Publication order is a CAS-style guard: a later-started transfer may
    * replace an earlier one, never the reverse; identical content reuses its
    * generation so repeated Git calls preserve their server-side checkout. */
   ws_mirror_snapshot_t current = {0};
   current.generation = 7;
   current.sync_order = 20;
   snprintf(current.digest, sizeof(current.digest), "%s", digest_a);
   unsigned long long next_generation = 0;
   assert(workspace_mirror_snapshot_next_generation(&current, 1, 21, digest_a, &next_generation) ==
          0);
   assert(next_generation == 7);
   assert(workspace_mirror_snapshot_next_generation(&current, 1, 21, digest_b, &next_generation) ==
          0);
   assert(next_generation == 8);
   assert(workspace_mirror_snapshot_next_generation(&current, 1, 19, digest_b, &next_generation) ==
          -1);
   assert(workspace_mirror_snapshot_next_generation(NULL, 0, 1, digest_a, &next_generation) == 0);
   assert(next_generation == 1);

   /* --- session_setup (fresh): ensure(clone) + drift + reconstruct --- */
   g_cmd_log[0] = '\0';
   g_clone_rc = g_worktree_rc = g_apply_rc = 0;
   g_rev_parse_head = "abc123"; /* mirror head == client head → in_sync */
   ws_mirror_drift_t v = WS_MIRROR_DRIFT_UNKNOWN;
   char ds[256];
   assert(workspace_mirror_session_setup(mock_git, NULL, "https://host/r.git", "abc123", "/m", "/w",
                                         NULL, /*already=*/0, ds, sizeof(ds), &v) == 0);
   assert(strstr(g_cmd_log, "clone --mirror https://host/r.git /m") != NULL);
   assert(strstr(g_cmd_log, "worktree add --detach --force /w abc123") != NULL);
   assert(v == WS_MIRROR_IN_SYNC);

   /* --- session_setup (resumed): NO clone, NO reconstruct; drift still read --- */
   g_cmd_log[0] = '\0';
   g_rev_parse_head = "mhead"; /* differs from client head → a drift verdict */
   g_is_ancestor_rc = 0;
   v = WS_MIRROR_DRIFT_UNKNOWN;
   assert(workspace_mirror_session_setup(mock_git, NULL, "https://host/r.git", "chead", "/m", "/w",
                                         NULL, /*already=*/1, ds, sizeof(ds), &v) == 0);
   assert(strstr(g_cmd_log, "clone") == NULL);    /* reused existing mirror */
   assert(strstr(g_cmd_log, "worktree") == NULL); /* preserved in-progress tree */
   assert(strstr(g_cmd_log, "rev-parse") != NULL);
   assert(v != WS_MIRROR_IN_SYNC && v != WS_MIRROR_DRIFT_UNKNOWN);

   /* --- session_setup (fresh) propagates an ensure failure --- */
   g_clone_rc = 1;
   assert(workspace_mirror_session_setup(mock_git, NULL, "https://host/r.git", "abc123", "/m", "/w",
                                         NULL, 0, NULL, 0, NULL) == -1);
   g_clone_rc = 0;

   /* --- session_setup (fresh) requires a remote --- */
   assert(workspace_mirror_session_setup(mock_git, NULL, "", "abc123", "/m", "/w", NULL, 0, NULL, 0,
                                         NULL) == -1);

   printf("workspace_mirror: all tests passed\n");
   return 0;
}
