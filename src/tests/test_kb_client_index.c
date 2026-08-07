/* test_kb_client_index.c: pin the wire contract for kb_client_index_scan.
 *
 * Regression: kb_client_index_scan used to ignore the response status
 * field, so a kb-side error like "canonical index unavailable (DB2 not
 * initialized)" was flattened into a successful empty scan and the CLI
 * cheerfully printed "==> Scan complete: 0 project(s), 0 file(s) scanned".
 * These tests pin the parse helper so that contract can't drift again. */
#include "cJSON.h"
#include "kb_client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *parse_resp(const char *json)
{
   cJSON *r = cJSON_Parse(json);
   assert(r != NULL);
   return r;
}

static void test_transport_failure_is_no_kb(void)
{
   kb_client_index_scan_result_t res;
   memset(&res, 0xff, sizeof(res));
   int rc = kb_client_index_scan_apply_response(NULL, &res);
   assert(rc == -1);
   assert(res.skipped == 1);
   assert(strcmp(res.reason, "no_kb") == 0);
   assert(res.message[0] == '\0');
   assert(res.projects == 0);
   assert(res.files == 0);
}

static void test_kb_error_surfaces_message(void)
{
   /* This is the exact response shape an out-of-the-box aimee-kb returns
    * when DB2 is not initialised. Before the fix this returned 0 with
    * everything zeroed; the user saw "Scan complete: 0 projects". */
   cJSON *resp = parse_resp("{\"status\":\"error\","
                            "\"message\":\"canonical index unavailable (DB2 not initialized)\"}");
   kb_client_index_scan_result_t res;
   int rc = kb_client_index_scan_apply_response(resp, &res);
   assert(rc == -1);
   assert(res.skipped == 1);
   assert(strcmp(res.reason, "error") == 0);
   assert(strstr(res.message, "canonical index unavailable") != NULL);
   assert(res.projects == 0);
   assert(res.files == 0);
   cJSON_Delete(resp);
}

static void test_kb_error_without_message(void)
{
   cJSON *resp = parse_resp("{\"status\":\"error\"}");
   kb_client_index_scan_result_t res;
   int rc = kb_client_index_scan_apply_response(resp, &res);
   assert(rc == -1);
   assert(res.skipped == 1);
   assert(strcmp(res.reason, "error") == 0);
   /* No message means no message — never invent one. */
   assert(res.message[0] == '\0');
   cJSON_Delete(resp);
}

static void test_success(void)
{
   cJSON *resp = parse_resp("{\"status\":\"ok\",\"skipped\":false,"
                            "\"projects\":3,\"files\":42}");
   kb_client_index_scan_result_t res;
   int rc = kb_client_index_scan_apply_response(resp, &res);
   assert(rc == 0);
   assert(res.skipped == 0);
   assert(res.projects == 3);
   assert(res.files == 42);
   /* Older kb daemons don't send "inspected" — must default to 0 so the
    * CLI knows to fall back to the unqualified "re-indexed" message. */
   assert(res.inspected == 0);
   assert(res.reason[0] == '\0');
   assert(res.message[0] == '\0');
   cJSON_Delete(resp);
}

static void test_success_carries_inspected(void)
{
   cJSON *resp = parse_resp("{\"status\":\"ok\",\"skipped\":false,"
                            "\"projects\":3,\"files\":11,\"inspected\":2850}");
   kb_client_index_scan_result_t res;
   int rc = kb_client_index_scan_apply_response(resp, &res);
   assert(rc == 0);
   assert(res.files == 11);
   assert(res.inspected == 2850);
   cJSON_Delete(resp);
}

static void test_skipped_cooldown(void)
{
   cJSON *resp = parse_resp("{\"status\":\"ok\",\"skipped\":true,"
                            "\"reason\":\"cooldown\",\"retry_after\":42}");
   kb_client_index_scan_result_t res;
   int rc = kb_client_index_scan_apply_response(resp, &res);
   assert(rc == 0);
   assert(res.skipped == 1);
   assert(strcmp(res.reason, "cooldown") == 0);
   assert(res.retry_after == 42);
   cJSON_Delete(resp);
}

static void test_skipped_busy(void)
{
   cJSON *resp = parse_resp("{\"status\":\"ok\",\"skipped\":true,\"reason\":\"busy\"}");
   kb_client_index_scan_result_t res;
   int rc = kb_client_index_scan_apply_response(resp, &res);
   assert(rc == 0);
   assert(res.skipped == 1);
   assert(strcmp(res.reason, "busy") == 0);
   assert(res.retry_after == 0);
   cJSON_Delete(resp);
}

static void test_long_kb_message_is_truncated_safely(void)
{
   /* Build a giant message; the result struct caps it at 256 bytes
    * including the NUL. snprintf must clamp without smashing the stack. */
   char big[1024];
   memset(big, 'x', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';

   char json[2048];
   snprintf(json, sizeof(json), "{\"status\":\"error\",\"message\":\"%s\"}", big);
   cJSON *resp = parse_resp(json);
   kb_client_index_scan_result_t res;
   int rc = kb_client_index_scan_apply_response(resp, &res);
   assert(rc == -1);
   assert(strcmp(res.reason, "error") == 0);
   /* sizeof - 1 because snprintf NUL-terminates within the buffer. */
   assert(strlen(res.message) == sizeof(res.message) - 1);
   cJSON_Delete(resp);
}

/* --- format_response: shape of what aimee-server returns to the CLI --- */

static cJSON *format(int kb_rc, const kb_client_index_scan_result_t *res)
{
   cJSON *r = (cJSON *)kb_client_index_scan_format_response(kb_rc, res);
   assert(r != NULL);
   return r;
}

static const char *str_field(cJSON *obj, const char *key)
{
   cJSON *f = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsString(f) ? f->valuestring : NULL;
}

static int int_field(cJSON *obj, const char *key)
{
   cJSON *f = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsNumber(f) ? (int)f->valuedouble : 0;
}

static void test_format_kb_error_propagates_message(void)
{
   /* The regression: kb said "DB2 not initialized" but the wire response
    * used to be {status:ok, projects:0, files:0}. Pin the new contract:
    * kb-side errors flow through as status:error with the kb message. */
   kb_client_index_scan_result_t res;
   memset(&res, 0, sizeof(res));
   res.skipped = 1;
   snprintf(res.reason, sizeof(res.reason), "error");
   snprintf(res.message, sizeof(res.message), "canonical index unavailable (DB2 not initialized)");

   cJSON *resp = format(-1, &res);
   assert(strcmp(str_field(resp, "status"), "error") == 0);
   assert(strstr(str_field(resp, "message"), "DB2 not initialized") != NULL);
   /* Must NOT carry projects/files — that fooled the CLI before. */
   assert(cJSON_GetObjectItemCaseSensitive(resp, "projects") == NULL);
   assert(cJSON_GetObjectItemCaseSensitive(resp, "files") == NULL);
   cJSON_Delete(resp);
}

static void test_format_no_kb_uses_generic_message(void)
{
   kb_client_index_scan_result_t res;
   memset(&res, 0, sizeof(res));
   res.skipped = 1;
   snprintf(res.reason, sizeof(res.reason), "no_kb");

   cJSON *resp = format(-1, &res);
   assert(strcmp(str_field(resp, "status"), "error") == 0);
   assert(strcmp(str_field(resp, "message"), "knowledge service unavailable") == 0);
   cJSON_Delete(resp);
}

static void test_format_success_carries_counts(void)
{
   kb_client_index_scan_result_t res;
   memset(&res, 0, sizeof(res));
   res.projects = 7;
   res.files = 1234;

   cJSON *resp = format(0, &res);
   assert(strcmp(str_field(resp, "status"), "ok") == 0);
   assert(int_field(resp, "projects") == 7);
   assert(int_field(resp, "files") == 1234);
   /* inspected omitted when 0 so old wire contract still parses cleanly. */
   assert(cJSON_GetObjectItemCaseSensitive(resp, "inspected") == NULL);
   cJSON *skipped = cJSON_GetObjectItemCaseSensitive(resp, "skipped");
   assert(cJSON_IsFalse(skipped));
   assert(cJSON_GetObjectItemCaseSensitive(resp, "reason") == NULL);
   cJSON_Delete(resp);
}

static void test_format_success_carries_inspected(void)
{
   kb_client_index_scan_result_t res;
   memset(&res, 0, sizeof(res));
   res.projects = 33;
   res.files = 11;
   res.inspected = 2850;

   cJSON *resp = format(0, &res);
   assert(int_field(resp, "files") == 11);
   assert(int_field(resp, "inspected") == 2850);
   cJSON_Delete(resp);
}

static void test_format_skipped_cooldown(void)
{
   kb_client_index_scan_result_t res;
   memset(&res, 0, sizeof(res));
   res.skipped = 1;
   snprintf(res.reason, sizeof(res.reason), "cooldown");
   res.retry_after = 42;

   cJSON *resp = format(0, &res);
   assert(strcmp(str_field(resp, "status"), "ok") == 0);
   cJSON *skipped = cJSON_GetObjectItemCaseSensitive(resp, "skipped");
   assert(cJSON_IsTrue(skipped));
   assert(strcmp(str_field(resp, "reason"), "cooldown") == 0);
   assert(int_field(resp, "retry_after") == 42);
   cJSON_Delete(resp);
}

/* A scan that outruns its timeout is not an unavailable service. The bound is
 * tunable because "large tree" has no fixed size, and a bad value must not be
 * able to disable it. */
static void test_scan_timeout_is_tunable_and_bounded(void)
{
   unsetenv("AIMEE_KB_SCAN_TIMEOUT_MS");
   assert(kb_client_index_scan_timeout_ms() == 5 * 60 * 1000);

   setenv("AIMEE_KB_SCAN_TIMEOUT_MS", "1800000", 1);
   assert(kb_client_index_scan_timeout_ms() == 1800000);

   /* Rejected: zero, negative, junk, and beyond the 24h ceiling all fall back. */
   setenv("AIMEE_KB_SCAN_TIMEOUT_MS", "0", 1);
   assert(kb_client_index_scan_timeout_ms() == 5 * 60 * 1000);
   setenv("AIMEE_KB_SCAN_TIMEOUT_MS", "-1", 1);
   assert(kb_client_index_scan_timeout_ms() == 5 * 60 * 1000);
   setenv("AIMEE_KB_SCAN_TIMEOUT_MS", "banana", 1);
   assert(kb_client_index_scan_timeout_ms() == 5 * 60 * 1000);
   setenv("AIMEE_KB_SCAN_TIMEOUT_MS", "999999999", 1);
   assert(kb_client_index_scan_timeout_ms() == 5 * 60 * 1000);
   unsetenv("AIMEE_KB_SCAN_TIMEOUT_MS");
}

/* ---- blast-radius contract -------------------------------------------------
 *
 * RED-GREEN NOTE. These exist because kb_client_index_blast_radius failed with a
 * bare "blast radius lookup failed" while the kb served a conforming 200, and
 * there was NO test that took the bytes the kb actually returns and asserted the
 * client accepts them. Four container images and a broken server went into
 * answering a question the first assertion below settles in milliseconds.
 *
 * BLAST_RADIUS_KB_PAYLOAD is recorded verbatim from aimee-kb on 403
 * (GET /v1/code/blast-radius?project=...&file_path=src/dstr.c, HTTP 200),
 * trimmed to two edges of each kind. If the kb's shape ever drifts from what
 * this client demands, this test fails instead of a cell dying in readiness. */
#define BLAST_RADIUS_KB_PAYLOAD                                                                    \
   "{\"file\":\"src/dstr.c\",\"project\":\"p1\",\"generation\":1,\"freshness\":\"current\","       \
   "\"resolved\":true,"                                                                            \
   "\"dependency_edges\":[{\"identity\":\"dstr.h\",\"provenance\":\"import\","                     \
   "\"confidence\":\"high\",\"project\":\"p1\",\"generation\":1,\"freshness\":\"current\"}],"      \
   "\"dependent_edges\":["                                                                         \
   "{\"path\":\"src/client_integrations.c\",\"provenance\":\"call\",\"confidence\":\"high\","      \
   "\"project\":\"p1\",\"generation\":1,\"freshness\":\"current\"},"                               \
   "{\"path\":\"src/diff.c\",\"provenance\":\"call\",\"confidence\":\"high\","                     \
   "\"project\":\"p1\",\"generation\":1,\"freshness\":\"current\"}]}"

static void test_blast_radius_accepts_the_payload_the_kb_actually_sends(void)
{
   cJSON *resp = parse_resp(BLAST_RADIUS_KB_PAYLOAD);
   char why[64] = "unset";
   assert(kb_client_index_blast_response_valid(resp, why, sizeof(why)) == 1);
   assert(why[0] == '\0');
   cJSON_Delete(resp);
}

/* An unfinished ingest answers with resolved=false rather than an error, and the
 * caller must treat that as "not ready", not as a malformed kb. */
static void test_blast_radius_rejects_an_unresolved_lookup_by_name(void)
{
   cJSON *resp = parse_resp("{\"project\":\"p1\",\"generation\":1,\"freshness\":\"stale\","
                            "\"resolved\":false,\"dependency_edges\":[],\"dependent_edges\":[]}");
   char why[64] = "";
   assert(kb_client_index_blast_response_valid(resp, why, sizeof(why)) == 0);
   assert(strcmp(why, "resolved") == 0);
   cJSON_Delete(resp);
}

/* One malformed edge rejects the whole payload -- a partial impact graph is
 * worse than none -- but the rejection has to say which side was malformed. */
static void test_blast_radius_names_the_malformed_edge_set(void)
{
   cJSON *resp = parse_resp(
       "{\"project\":\"p1\",\"generation\":1,\"freshness\":\"current\",\"resolved\":true,"
       "\"dependency_edges\":[{\"identity\":\"dstr.h\",\"provenance\":\"import\","
       "\"confidence\":\"high\",\"project\":\"p1\",\"generation\":1,\"freshness\":\"current\"}],"
       "\"dependent_edges\":[{\"path\":\"a.c\",\"provenance\":\"call\",\"confidence\":\"high\","
       "\"project\":\"p1\",\"generation\":1,\"freshness\":\"current\"},"
       "{\"path\":\"b.c\",\"provenance\":\"call\",\"project\":\"p1\",\"generation\":1,"
       "\"freshness\":\"current\"}]}");
   char why[64] = "";
   assert(kb_client_index_blast_response_valid(resp, why, sizeof(why)) == 0);
   assert(strcmp(why, "dependent_edges") == 0);
   cJSON_Delete(resp);
}

/* A kb that answers with an error object is a refusal, not a verdict. */
static void test_blast_radius_rejects_an_error_object(void)
{
   cJSON *resp = parse_resp("{\"error\":\"unknown project\"}");
   char why[64] = "";
   assert(kb_client_index_blast_response_valid(resp, why, sizeof(why)) == 0);
   assert(strcmp(why, "error field") == 0);
   cJSON_Delete(resp);
}

/* No response at all is distinct from a bad one; the caller renders them
 * differently (unreachable vs malformed). */
static void test_blast_radius_rejects_a_missing_response(void)
{
   char why[64] = "";
   assert(kb_client_index_blast_response_valid(NULL, why, sizeof(why)) == 0);
   assert(strcmp(why, "no response") == 0);
}

/* Edges must be arrays. A kb that sends the summary strings ("dependents") but
 * omits the edge objects this client requires is a contract break, and must not
 * be mistaken for an empty graph. */
static void test_blast_radius_rejects_missing_edge_arrays(void)
{
   cJSON *resp = parse_resp("{\"project\":\"p1\",\"generation\":1,\"freshness\":\"current\","
                            "\"resolved\":true,\"dependents\":[\"a.c\"]}");
   char why[64] = "";
   assert(kb_client_index_blast_response_valid(resp, why, sizeof(why)) == 0);
   assert(strcmp(why, "dependency_edges") == 0);
   cJSON_Delete(resp);
}

/* ---- index read timeout ----------------------------------------------------
 *
 * RED-GREEN. This was a hardcoded 5s. Measured on CT403 against a 3825-file
 * checkout, the kb answers /v1/code/blast-radius in 5.8-5.9s consistently -- it
 * walks a dependency graph, not one row -- so every lookup missed by under a
 * second and surfaced as "blast radius lookup failed" (http=-1, no body). Symbol
 * and caller lookups are cheap and stayed inside it, which is exactly why two of
 * three readiness probes passed and the third never did.
 *
 * The defect scaled with corpus size: invisible on a small fixture, total on a
 * real one. A bound below the measured cost is the bug, so assert the headroom. */
static void test_index_read_timeout_clears_measured_blast_radius_cost(void)
{
   /* 5.9s measured; a 5s bound is what broke it. Demand real headroom. */
   assert(kb_client_index_read_timeout_ms() >= 15000);
}

static void test_index_read_timeout_is_operator_tunable(void)
{
   setenv("AIMEE_KB_READ_TIMEOUT_MS", "12345", 1);
   assert(kb_client_index_read_timeout_ms() == 12345);
   /* Garbage and out-of-range values fall back rather than disabling the bound. */
   setenv("AIMEE_KB_READ_TIMEOUT_MS", "0", 1);
   assert(kb_client_index_read_timeout_ms() >= 15000);
   setenv("AIMEE_KB_READ_TIMEOUT_MS", "not-a-number", 1);
   assert(kb_client_index_read_timeout_ms() >= 15000);
   unsetenv("AIMEE_KB_READ_TIMEOUT_MS");
}

int main(void)
{
   test_scan_timeout_is_tunable_and_bounded();
   test_transport_failure_is_no_kb();
   test_kb_error_surfaces_message();
   test_kb_error_without_message();
   test_success();
   test_success_carries_inspected();
   test_skipped_cooldown();
   test_skipped_busy();
   test_long_kb_message_is_truncated_safely();
   test_format_kb_error_propagates_message();
   test_format_no_kb_uses_generic_message();
   test_format_success_carries_counts();
   test_format_success_carries_inspected();
   test_format_skipped_cooldown();
   test_blast_radius_accepts_the_payload_the_kb_actually_sends();
   test_blast_radius_rejects_an_unresolved_lookup_by_name();
   test_blast_radius_names_the_malformed_edge_set();
   test_blast_radius_rejects_an_error_object();
   test_blast_radius_rejects_a_missing_response();
   test_blast_radius_rejects_missing_edge_arrays();
   test_index_read_timeout_clears_measured_blast_radius_cost();
   test_index_read_timeout_is_operator_tunable();
   printf("kb_client_index: all tests passed\n");
   return 0;
}
