/* Unit tests for the pure self-update helpers (self_update_util.c): version
 * comparison, version-string validation, platform asset name, and the drift
 * verdict that decides whether a client is told it is behind its server. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cmd_self_update.h"

static void test_version_compare(void)
{
   /* Equality, ignoring a leading 'v' on either side. */
   assert(aimee_version_compare("0.2.182", "0.2.182") == 0);
   assert(aimee_version_compare("v0.2.182", "0.2.182") == 0);
   assert(aimee_version_compare("0.2.182", "v0.2.182") == 0);

   /* Ordering by major, minor, patch. */
   assert(aimee_version_compare("0.2.183", "0.2.182") > 0);
   assert(aimee_version_compare("0.2.182", "0.2.183") < 0);
   assert(aimee_version_compare("0.3.0", "0.2.999") > 0);
   assert(aimee_version_compare("1.0.0", "0.9.9") > 0);
   assert(aimee_version_compare("0.2.9", "0.2.10") < 0); /* numeric, not lexical */

   /* git-describe suffix is ignored (compares by the tag part). */
   assert(aimee_version_compare("v0.2.180-31-g3342b09e", "0.2.180") == 0);
   assert(aimee_version_compare("v0.2.181-1-gabc", "0.2.180") > 0);

   /* Missing components count as zero. */
   assert(aimee_version_compare("0.2", "0.2.0") == 0);
   assert(aimee_version_compare("1", "0.9.9") > 0);

   /* Defensive: NULL/garbage parse to 0.0.0. */
   assert(aimee_version_compare(NULL, "0.0.0") == 0);
   assert(aimee_version_compare("0.0.1", NULL) > 0);
   printf("  test_version_compare ok\n");
}

static void test_version_is_safe(void)
{
   assert(aimee_version_is_safe("0.2.182"));
   assert(aimee_version_is_safe("v0.2.180-31-g3342b09e"));
   assert(aimee_version_is_safe("1.2.3_rc1"));
   assert(!aimee_version_is_safe(""));
   assert(!aimee_version_is_safe(NULL));
   assert(!aimee_version_is_safe("0.2.1; rm -rf /")); /* shell metachars rejected */
   assert(!aimee_version_is_safe("0.2.1$(x)"));
   assert(!aimee_version_is_safe("0.2.1/../etc"));
   printf("  test_version_is_safe ok\n");
}

static void test_version_is_semver(void)
{
   assert(aimee_version_is_semver("0.2.182"));
   assert(aimee_version_is_semver("v0.2.182"));
   assert(aimee_version_is_semver("v0.2.180-31-g3342b09e"));
   assert(aimee_version_is_semver("1"));
   assert(!aimee_version_is_semver("testing-b4a856b")); /* deliberate dev/branch build */
   assert(!aimee_version_is_semver("vtesting"));
   assert(!aimee_version_is_semver(""));
   assert(!aimee_version_is_semver(NULL));
   printf("  test_version_is_semver ok\n");
}

static void test_asset(void)
{
   /* On the platforms we support, the asset name is non-NULL and starts with
    * the "aimee-" prefix and matches the running OS family. NULL is acceptable
    * (unsupported platform) but on Linux/macOS CI we expect a name. */
   const char *a = aimee_self_update_asset();
   if (a)
   {
      assert(strncmp(a, "aimee-", 6) == 0);
      assert(strstr(a, "linux") || strstr(a, "macos") || strstr(a, "windows"));
   }
   printf("  test_asset ok (%s)\n", a ? a : "(unsupported platform -> NULL)");
}

/* One day in epoch seconds, for readable drift fixtures. */
#define DAY 86400L
#define T0  1754000000L /* arbitrary fixed "client build" instant */

static void test_notice_semver(void)
{
   char buf[256];

   /* Released server ahead of a released client: name `self-update`, which can
    * actually fetch that release asset. */
   assert(aimee_self_update_notice_for("0.3.1", 0, "0.3.0", 0, buf, sizeof buf) == 1);
   assert(strstr(buf, "self-update"));
   assert(strstr(buf, "0.3.1") && strstr(buf, "0.3.0"));

   /* Equal or older server says nothing -- a client must never be told to
    * "catch up" to something behind it. */
   assert(aimee_self_update_notice_for("0.3.0", 0, "0.3.0", 0, buf, sizeof buf) == 0);
   assert(buf[0] == '\0');
   assert(aimee_self_update_notice_for("0.2.9", 0, "0.3.0", 0, buf, sizeof buf) == 0);

   /* A newer semver server outranks timestamps: even with the client stamped
    * later, the version ordering is the authority. */
   assert(aimee_self_update_notice_for("0.3.1", T0, "0.3.0", T0 + 30 * DAY, buf, sizeof buf) == 1);
   printf("  test_notice_semver ok\n");
}

static void test_notice_dev_build_drift(void)
{
   char buf[256];

   /* THE REGRESSION. A `testing-<sha>` server is not orderable by version, and
    * returning 0 here left a 7-day-stale client completely invisible: commands
    * exited 0 having printed nothing, and nothing anywhere said why. Commit
    * times order what version strings cannot. */
   assert(aimee_self_update_notice_for("testing-f3b7979", T0 + 7 * DAY, "0.3.0", T0, buf,
                                       sizeof buf) == 1);
   assert(strstr(buf, "testing-f3b7979"));
   assert(strstr(buf, "7 day"));

   /* It must NOT name `self-update`: that resolves a release asset by version,
    * and no release is published for a testing build. A remedy that cannot work
    * is worse than the silence it replaces. */
   assert(!strstr(buf, "self-update"));

   /* Sub-day drift still speaks, without claiming "0 day(s)". */
   assert(aimee_self_update_notice_for("testing-f3b7979", T0 + 600, "0.3.0", T0, buf, sizeof buf) ==
          1);
   assert(!strstr(buf, "0 day"));

   /* Client at or ahead of the server stays silent -- drift is not symmetric,
    * and a newer client is not a problem to report. */
   assert(aimee_self_update_notice_for("testing-f3b7979", T0, "0.3.0", T0, buf, sizeof buf) == 0);
   assert(aimee_self_update_notice_for("testing-f3b7979", T0, "0.3.0", T0 + DAY, buf, sizeof buf) ==
          0);

   /* An unstamped side is not evidence of anything; stay silent rather than
    * guess. (A server too old to advertise commit_time reports 0.) */
   assert(aimee_self_update_notice_for("testing-f3b7979", 0, "0.3.0", T0, buf, sizeof buf) == 0);
   assert(aimee_self_update_notice_for("testing-f3b7979", T0 + 7 * DAY, "0.3.0", 0, buf,
                                       sizeof buf) == 0);
   printf("  test_notice_dev_build_drift ok\n");
}

/* The notice interpolates BOTH version strings, and a git-describe version is
 * far longer than a release tag. Caught live: cli_session_start passed a 256-byte
 * buffer and the dev-build notice truncated mid-word ("...the server's own bui").
 * Short fixtures like "0.3.0" hide this entirely, so pin the real shape. */
static void test_notice_fits_real_version_strings(void)
{
   char buf[512];
   const char *srv = "pre-merge-safety-902-g0ec2c4f523";
   const char *cli = "pre-merge-safety-903-g6fee67ae87";

   assert(aimee_self_update_notice_for(srv, T0 + 7 * DAY, cli, T0, buf, sizeof buf) == 1);
   /* Rendered whole: the last sentence must survive to its final word. */
   size_t len = strlen(buf);
   assert(len > 0 && buf[len - 1] == '.');
   assert(strstr(buf, "server's own build."));
   /* And it genuinely does not fit the old buffer -- otherwise this test would
    * pass for the wrong reason and stop defending the size it exists to justify. */
   assert(len >= 256);
   printf("  test_notice_fits_real_version_strings ok (%zu bytes)\n", len);
}

static void test_notice_defensive(void)
{
   char buf[256];

   assert(aimee_self_update_notice_for(NULL, T0, "0.3.0", T0, buf, sizeof buf) == 0);
   assert(aimee_self_update_notice_for("", T0, "0.3.0", T0, buf, sizeof buf) == 0);
   assert(aimee_self_update_notice_for("0.3.1", 0, NULL, 0, buf, sizeof buf) == 1);
   assert(aimee_self_update_notice_for("0.3.1", 0, "0.3.0", 0, NULL, 0) == 0);
   printf("  test_notice_defensive ok\n");
}

int main(void)
{
   test_version_compare();
   test_version_is_safe();
   test_version_is_semver();
   test_notice_semver();
   test_notice_dev_build_drift();
   test_notice_fits_real_version_strings();
   test_notice_defensive();
   test_asset();
   printf("test_self_update: all passed\n");
   return 0;
}
