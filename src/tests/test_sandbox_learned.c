#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform_path.h"
#include "platform_test_util.h"
#include "sandbox_learned.h"

/* Count how many of the given names are present in the parsed set. */
static int has(char pk[][SBX_PKG_MAX], int n, const char *name)
{
   for (int i = 0; i < n; i++)
      if (strcmp(pk[i], name) == 0)
         return 1;
   return 0;
}

int main(void)
{
   printf("sandbox_learned: ");

   char pk[SBX_LEARN_MAX][SBX_PKG_MAX];

   /* --- parser: plain apt install --- */
   int n = sandbox_learned_parse_apt("apt install gcc make", pk, SBX_LEARN_MAX);
   assert(n == 2 && has(pk, n, "gcc") && has(pk, n, "make"));

   /* --- apt-get install with flags (skipped) --- */
   n = sandbox_learned_parse_apt("apt-get install -y --no-install-recommends libssl-dev cmake", pk,
                                 SBX_LEARN_MAX);
   assert(n == 2 && has(pk, n, "libssl-dev") && has(pk, n, "cmake"));
   assert(!has(pk, n, "-y") && !has(pk, n, "--no-install-recommends"));

   /* --- sudo + env prefix, options BEFORE the subcommand --- */
   n = sandbox_learned_parse_apt(
       "DEBIAN_FRONTEND=noninteractive sudo apt-get -y install pkg-config", pk, SBX_LEARN_MAX);
   assert(n == 1 && has(pk, n, "pkg-config"));

   /* --- stops at a shell operator; only the apt segment contributes --- */
   n = sandbox_learned_parse_apt("apt-get install -y git && make && ./configure", pk,
                                 SBX_LEARN_MAX);
   assert(n == 1 && has(pk, n, "git"));

   /* --- a later apt segment in a chain is also captured --- */
   n = sandbox_learned_parse_apt("apt-get update && apt-get install -y curl wget", pk,
                                 SBX_LEARN_MAX);
   assert(n == 2 && has(pk, n, "curl") && has(pk, n, "wget"));
   assert(!has(pk, n, "update"));

   /* --- non-install apt subcommands contribute nothing --- */
   n = sandbox_learned_parse_apt("apt-get remove gcc; apt-get update; apt-get upgrade -y", pk,
                                 SBX_LEARN_MAX);
   assert(n == 0);

   /* --- version pin / release suffix stripped to the bare name --- */
   n = sandbox_learned_parse_apt("apt-get install -y gcc=4:12.2 libfoo/bookworm", pk,
                                 SBX_LEARN_MAX);
   assert(n == 2 && has(pk, n, "gcc") && has(pk, n, "libfoo"));

   /* --- de-dup within a call --- */
   n = sandbox_learned_parse_apt("apt install gcc gcc gcc", pk, SBX_LEARN_MAX);
   assert(n == 1 && has(pk, n, "gcc"));

   /* --- a non-apt command yields nothing (no false positives from substrings) --- */
   n = sandbox_learned_parse_apt("echo apt install fake | tee /tmp/x", pk, SBX_LEARN_MAX);
   assert(n == 0);
   n = sandbox_learned_parse_apt("pip install requests && npm install left-pad", pk, SBX_LEARN_MAX);
   assert(n == 0); /* apt-only by design (matches the apt Dockerfile builder) */

   /* --- injection-y tokens never become packages --- */
   n = sandbox_learned_parse_apt("apt-get install -y 'gcc; rm -rf /' $(whoami) ../etc/passwd", pk,
                                 SBX_LEARN_MAX);
   assert(!has(pk, n, "$(whoami)"));
   for (int i = 0; i < n; i++)
   {
      /* whatever survived must be a clean package name */
      assert(pk[i][0] && pk[i][0] != '-' && !strchr(pk[i], '/') && !strchr(pk[i], ';') &&
             !strchr(pk[i], '$') && !strchr(pk[i], ' '));
   }

   /* --- store: record + load round-trip under a sandboxed AIMEE_HOME --- */
   {
      char home[512];
      snprintf(home, sizeof(home), "%s/aimee-learned-XXXXXX", platform_tmpdir());
      assert(platform_mkdtemp(home) != NULL);
      platform_setenv("HOME", home);
      platform_setenv("AIMEE_HOME", home);
      platform_setenv("AIMEE_NO_CACHE", "1");

      const char *root = "/proj/alpha";
      const char *a[] = {"gcc", "make"};
      assert(sandbox_learned_record(root, a, 2) == 0);
      const char *b[] = {"make", "cmake"}; /* make dup, cmake new */
      assert(sandbox_learned_record(root, b, 2) == 0);

      char got[SBX_LEARN_MAX][SBX_PKG_MAX];
      int gn = sandbox_learned_load(root, got, SBX_LEARN_MAX);
      assert(gn == 3);
      assert(has(got, gn, "gcc") && has(got, gn, "make") && has(got, gn, "cmake"));
      /* load() sorts for a stable build hash */
      assert(strcmp(got[0], "cmake") == 0 && strcmp(got[1], "gcc") == 0 &&
             strcmp(got[2], "make") == 0);

      /* a different project is isolated */
      const char *c[] = {"nodejs"};
      assert(sandbox_learned_record("/proj/beta", c, 1) == 0);
      gn = sandbox_learned_load(root, got, SBX_LEARN_MAX);
      assert(gn == 3 && !has(got, gn, "nodejs"));
      gn = sandbox_learned_load("/proj/beta", got, SBX_LEARN_MAX);
      assert(gn == 1 && has(got, gn, "nodejs"));

      /* unknown project -> empty */
      assert(sandbox_learned_load("/proj/none", got, SBX_LEARN_MAX) == 0);
   }

   printf("all tests passed\n");
   return 0;
}
