/* test_session_degraded_notice.c: the SessionStart degraded-dependency notice.
 *
 * An agent that cannot distinguish "the knowledge service is down" from "no
 * results" will state that a symbol or fact does not exist when it merely could
 * not look. This notice is the only thing that tells it otherwise, so its
 * trigger conditions are pinned here. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

int ss_degraded_notice(const char *kb, const char *retrieval, char *out, size_t cap);

static void test_healthy_is_silent(void)
{
   char buf[640];
   assert(ss_degraded_notice("ok", "ok", buf, sizeof buf) == 0);
   assert(buf[0] == '\0');
   printf("  PASS: healthy dependencies emit nothing\n");
}

static void test_unknown_is_silent(void)
{
   /* "unknown" means the readiness sampler has not run yet. That is normal at
    * boot and must NOT cry wolf -- a notice on every cold start would train the
    * agent (and the operator) to ignore it. */
   char buf[640];
   assert(ss_degraded_notice("unknown", "unknown", buf, sizeof buf) == 0);
   assert(ss_degraded_notice(NULL, NULL, buf, sizeof buf) == 0);
   assert(ss_degraded_notice("", "", buf, sizeof buf) == 0);
   printf("  PASS: unknown/absent state is not an alert\n");
}

static void test_kb_down_alerts(void)
{
   char buf[640];
   assert(ss_degraded_notice("fail", "ok", buf, sizeof buf) == 1);
   assert(strstr(buf, "degraded") != NULL);
   assert(strstr(buf, "knowledge service is unreachable") != NULL);
   /* The operative instruction: do not read empty results as authoritative. */
   assert(strstr(buf, "does NOT mean") != NULL);
   /* And the user must be told, not worked around silently. */
   assert(strstr(buf, "not fully functional") != NULL);
   printf("  PASS: kb failure alerts and instructs\n");
}

static void test_retrieval_down_alerts(void)
{
   char buf[640];
   assert(ss_degraded_notice("ok", "fail", buf, sizeof buf) == 1);
   assert(strstr(buf, "Retrieval is unavailable") != NULL);
   printf("  PASS: retrieval failure alerts\n");
}

static void test_kb_takes_precedence(void)
{
   /* Both down: name the KB, since retrieval failing is usually its symptom and
    * two alarms for one cause is noise. */
   char buf[640];
   assert(ss_degraded_notice("fail", "fail", buf, sizeof buf) == 1);
   assert(strstr(buf, "knowledge service is unreachable") != NULL);
   printf("  PASS: kb named when both are down\n");
}

static void test_clone_reassurance_present(void)
{
   /* The reconciler re-queues GUI clones once the service returns. Without this
    * line an agent would reasonably tell the user to re-clone by hand. */
   char buf[640];
   assert(ss_degraded_notice("fail", "ok", buf, sizeof buf) == 1);
   assert(strstr(buf, "indexed once the service returns") != NULL);
   printf("  PASS: states that clones self-heal\n");
}

static void test_truncation_is_not_a_half_message(void)
{
   /* A half-rendered warning is worse than none: it could cut off mid-sentence
    * and invert the meaning. Too small a buffer must yield no notice at all. */
   char small[32];
   assert(ss_degraded_notice("fail", "fail", small, sizeof small) == 0);
   assert(small[0] == '\0');
   char zero[1];
   assert(ss_degraded_notice("fail", "fail", zero, 0) == 0);
   assert(ss_degraded_notice("fail", "fail", NULL, 640) == 0);
   printf("  PASS: truncation yields no notice rather than a partial one\n");
}

int main(void)
{
   printf("session_degraded_notice:\n");
   test_healthy_is_silent();
   test_unknown_is_silent();
   test_kb_down_alerts();
   test_retrieval_down_alerts();
   test_kb_takes_precedence();
   test_clone_reassurance_present();
   test_truncation_is_not_a_half_message();
   printf("session_degraded_notice: all tests passed\n");
   return 0;
}
