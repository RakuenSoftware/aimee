/* Choosing the commit a server-side mirror reconstructs from.
 *
 * The mirror seeds itself by FETCHING a commit from the client's remote, so the
 * only valid base is one the remote already has. Registering the client's HEAD
 * is wrong the moment the developer has a commit they never pushed: the fetch
 * cannot resolve it, the reconstruct fails, and the delegate gets nothing --
 * which is most of the time, for anyone with work in progress.
 *
 * Resolving to the newest PUSHED ancestor instead lets those commits ride along
 * inside the client patch as ordinary file content, which is what a delegate
 * needs to build against.
 *
 * This pins the decision itself, fed the output `git rev-list --boundary HEAD
 * --not --remotes` produces in each case, so it runs without a git repository. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <aimee/workspace/client_diff.h>

#define PUSHED   "a4cd94f25fe510b63b6f0fe3334fd35af10905e3"
#define LOCAL1   "cbdc6266bc9266b8de3536960aa57be7cefde4b1"
#define LOCAL2   "863c5be557f6e1dca24827c16e92a1c69e8704a5"
#define HEAD_SHA "863c5be557f6e1dca24827c16e92a1c69e8704a5"

static void test_unpushed_commits_resolve_to_the_last_pushed_one(void)
{
   /* Two local-only commits above a pushed base. Verbatim shape of the real
    * output: the excluded commits first, then the boundary marked with '-'. */
   const char *out = LOCAL2 "\n" LOCAL1 "\n-" PUSHED "\n";
   char base[64] = "";
   assert(workspace_client_mirror_base_select(out, HEAD_SHA, base, sizeof(base)) == 0);
   assert(strcmp(base, PUSHED) == 0);
   printf("  PASS: unpushed commits resolve to the last pushed ancestor\n");
}

static void test_clean_tree_uses_head(void)
{
   /* Nothing unpushed: rev-list prints nothing, so HEAD is already on a remote. */
   char base[64] = "";
   assert(workspace_client_mirror_base_select("", HEAD_SHA, base, sizeof(base)) == 0);
   assert(strcmp(base, HEAD_SHA) == 0);
   printf("  PASS: a fully pushed tree uses HEAD\n");
}

static void test_no_fetchable_ancestor_is_refused(void)
{
   /* Commits with no boundary beneath them: nothing on any remote. There is no
    * safe answer, so the caller must refuse rather than name an unfetchable
    * commit and fail later inside a reconstruct. */
   const char *out = LOCAL2 "\n" LOCAL1 "\n";
   char base[64] = "";
   assert(workspace_client_mirror_base_select(out, HEAD_SHA, base, sizeof(base)) == -1);
   assert(base[0] == '\0');
   printf("  PASS: no fetchable ancestor is refused\n");
}

static void test_boundary_found_wherever_it_appears(void)
{
   /* Order is git's business, not ours: the boundary is honoured first or last. */
   char base[64] = "";
   assert(workspace_client_mirror_base_select("-" PUSHED "\n" LOCAL1 "\n", HEAD_SHA, base,
                                              sizeof(base)) == 0);
   assert(strcmp(base, PUSHED) == 0);
   /* CRLF and a missing trailing newline must not hide it. */
   base[0] = '\0';
   assert(workspace_client_mirror_base_select(LOCAL1 "\r\n-" PUSHED, HEAD_SHA, base,
                                              sizeof(base)) == 0);
   assert(strcmp(base, PUSHED) == 0);
   printf("  PASS: the boundary is found wherever it appears\n");
}

static void test_garbage_never_becomes_a_base(void)
{
   char base[64] = "";
   /* Not hex, wrong length, and an empty marker: none may be accepted as a
    * commit id, or the server would be asked to fetch nonsense. */
   assert(workspace_client_mirror_base_select("-notacommit\n", HEAD_SHA, base, sizeof(base)) == -1);
   assert(workspace_client_mirror_base_select("-abc123\n", HEAD_SHA, base, sizeof(base)) == -1);
   assert(workspace_client_mirror_base_select("-\n", HEAD_SHA, base, sizeof(base)) == -1);
   /* An unusable HEAD cannot stand in for a base either. */
   assert(workspace_client_mirror_base_select("", "not-a-sha", base, sizeof(base)) == -1);
   assert(workspace_client_mirror_base_select("", "", base, sizeof(base)) == -1);
   assert(workspace_client_mirror_base_select("", NULL, base, sizeof(base)) == -1);
   /* A buffer that cannot hold a commit id is refused, not truncated. */
   char tiny[8];
   assert(workspace_client_mirror_base_select("-" PUSHED "\n", HEAD_SHA, tiny, sizeof(tiny)) == -1);
   printf("  PASS: garbage never becomes a base\n");
}

int main(void)
{
   printf("test_workspace_client_base\n");
   test_unpushed_commits_resolve_to_the_last_pushed_one();
   test_clean_tree_uses_head();
   test_no_fetchable_ancestor_is_refused();
   test_boundary_found_wherever_it_appears();
   test_garbage_never_becomes_a_base();
   printf("All tests passed.\n");
   return 0;
}
