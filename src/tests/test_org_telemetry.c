/* test_org_telemetry.c: P9a pure-helper unit tests (no Postgres). Covers the
 * dependency-light half of the telemetry slice — the parts that DON'T need a live
 * DB (those live in the real-PG gate scripts/p9_telemetry_rls_test.sql):
 *   - metric_name PII-structural validation (charset + length);
 *   - Prometheus label-value escaping ('\\', '"', newline);
 *   - Prometheus render (# HELP / # TYPE, label emission, escaping, unknown-metric
 *     skip, no-label line);
 *   - the scrape/ingest token SHA-256 + constant-time compare (a wrong token is
 *     rejected; the token itself never appears in any rendered output);
 *   - a STRUCTURAL check that org_telemetry has NO content/sub/payload/jsonb column
 *     (the content-free-by-construction invariant), asserted against the shipped
 *     schema SQL. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "modules/db2/c/org_telemetry_fmt.h"
#include "schema_data.h" /* AIMEE_DB2_SCHEMA_SQL (generated) */

/* ---- 1. metric_name validation (^[a-zA-Z0-9_:]{1,128}$) ---- */
static void test_metric_name_valid(void)
{
   assert(org_telemetry_metric_name_valid("aimee_tokens_total") == 1);
   assert(org_telemetry_metric_name_valid("a") == 1);
   assert(org_telemetry_metric_name_valid("ns:metric_01") == 1);
   assert(org_telemetry_metric_name_valid("ABC123_:") == 1);
   /* Rejects: empty, NULL, disallowed chars (a sub / email / free text cannot pass). */
   assert(org_telemetry_metric_name_valid("") == 0);
   assert(org_telemetry_metric_name_valid(NULL) == 0);
   assert(org_telemetry_metric_name_valid("has space") == 0);
   assert(org_telemetry_metric_name_valid("dash-name") == 0);
   assert(org_telemetry_metric_name_valid("user@example.com") == 0);
   assert(org_telemetry_metric_name_valid("oidc:iss/sub") == 0); /* '/' disallowed */
   assert(org_telemetry_metric_name_valid("quote\"inject") == 0);
   assert(org_telemetry_metric_name_valid("nl\ninject") == 0);
   /* Length: exactly 128 ok, 129 rejected. */
   char n128[129];
   memset(n128, 'a', 128);
   n128[128] = '\0';
   assert(org_telemetry_metric_name_valid(n128) == 1);
   char n129[130];
   memset(n129, 'a', 129);
   n129[129] = '\0';
   assert(org_telemetry_metric_name_valid(n129) == 0);
   printf("  metric_name validation: ok\n");
}

/* ---- 2. Prometheus label-value escaping ---- */
static void test_escape(void)
{
   char out[64];
   assert(org_telemetry_prom_escape("plain", out, sizeof(out)) == 0);
   assert(strcmp(out, "plain") == 0);
   assert(org_telemetry_prom_escape("a\\b", out, sizeof(out)) == 0);
   assert(strcmp(out, "a\\\\b") == 0);
   assert(org_telemetry_prom_escape("a\"b", out, sizeof(out)) == 0);
   assert(strcmp(out, "a\\\"b") == 0);
   assert(org_telemetry_prom_escape("a\nb", out, sizeof(out)) == 0);
   assert(strcmp(out, "a\\nb") == 0);
   /* All three at once. */
   assert(org_telemetry_prom_escape("\\\"\n", out, sizeof(out)) == 0);
   assert(strcmp(out, "\\\\\\\"\\n") == 0);
   /* Overflow -> -1 and empty out. */
   char tiny[3];
   assert(org_telemetry_prom_escape("\\\\", tiny, sizeof(tiny)) == -1);
   assert(tiny[0] == '\0');
   printf("  prometheus escape: ok\n");
}

/* ---- 3. Prometheus render ---- */
static void test_render(void)
{
   org_metric_row_t rows[4];
   memset(rows, 0, sizeof(rows));
   /* team-labelled counter */
   snprintf(rows[0].metric, sizeof(rows[0].metric), "aimee_org_spend_usd");
   rows[0].team = 42;
   rows[0].period[0] = '\0';
   rows[0].model[0] = '\0';
   snprintf(rows[0].value, sizeof(rows[0].value), "12.5");
   /* team+period gauge */
   snprintf(rows[1].metric, sizeof(rows[1].metric), "aimee_org_budget_spend_usd");
   rows[1].team = 42;
   snprintf(rows[1].period, sizeof(rows[1].period), "day");
   snprintf(rows[1].value, sizeof(rows[1].value), "3.0");
   /* no-label gauge */
   snprintf(rows[2].metric, sizeof(rows[2].metric), "aimee_org_teams");
   rows[2].team = -1;
   snprintf(rows[2].value, sizeof(rows[2].value), "7");
   /* unknown metric -> skipped defensively */
   snprintf(rows[3].metric, sizeof(rows[3].metric), "totally_unknown_metric");
   rows[3].team = -1;
   snprintf(rows[3].value, sizeof(rows[3].value), "999");

   char out[2048];
   int rc = org_telemetry_render_prom(rows, 4, out, sizeof(out));
   assert(rc > 0);
   /* HELP + TYPE lines present for the known families. */
   assert(strstr(out, "# HELP aimee_org_spend_usd ") != NULL);
   assert(strstr(out, "# TYPE aimee_org_spend_usd counter") != NULL);
   assert(strstr(out, "aimee_org_spend_usd{team=\"42\"} 12.5") != NULL);
   assert(strstr(out, "# TYPE aimee_org_budget_spend_usd gauge") != NULL);
   assert(strstr(out, "aimee_org_budget_spend_usd{team=\"42\",period=\"day\"} 3.0") != NULL);
   /* No-label line has no braces. */
   assert(strstr(out, "aimee_org_teams 7\n") != NULL);
   /* The unknown metric was skipped entirely. */
   assert(strstr(out, "totally_unknown_metric") == NULL);
   assert(strstr(out, "999") == NULL);
   printf("  prometheus render: ok\n");
}

/* An ingested/free-text model label is escaped, never emitted raw. */
static void test_render_model_escape(void)
{
   org_metric_row_t r;
   memset(&r, 0, sizeof(r));
   snprintf(r.metric, sizeof(r.metric), "aimee_org_catalog_models");
   r.team = -1;
   snprintf(r.model, sizeof(r.model), "weird\"model\\x");
   snprintf(r.value, sizeof(r.value), "1");
   char out[512];
   assert(org_telemetry_render_prom(&r, 1, out, sizeof(out)) > 0);
   assert(strstr(out, "model=\"weird\\\"model\\\\x\"") != NULL);
   /* The raw unescaped form must NOT appear. */
   assert(strstr(out, "weird\"model\\x\"") == NULL);
   printf("  prometheus render model escaping: ok\n");
}

/* ---- 4. token SHA-256 + constant-time compare ---- */
static void test_token(void)
{
   /* Known vector: sha256("abc"). */
   char h[65];
   org_telemetry_sha256_hex("abc", h);
   assert(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

   /* A correct token matches its own hash; a wrong token does not. */
   const char *secret = "s3cr3t-scrape-token";
   char expected[65];
   org_telemetry_sha256_hex(secret, expected);
   char presented_ok[65];
   org_telemetry_sha256_hex(secret, presented_ok);
   assert(org_telemetry_token_hash_eq(presented_ok, expected) == 1);
   char presented_bad[65];
   org_telemetry_sha256_hex("wrong-token", presented_bad);
   assert(org_telemetry_token_hash_eq(presented_bad, expected) == 0);
   /* Malformed (wrong length) never authorizes. */
   assert(org_telemetry_token_hash_eq("short", expected) == 0);
   assert(org_telemetry_token_hash_eq(expected, "") == 0);
   /* Off-by-one-char hash rejected (single mismatch found despite no early-out). */
   char nearmiss[65];
   memcpy(nearmiss, expected, 65);
   nearmiss[63] = (nearmiss[63] == 'a') ? 'b' : 'a';
   assert(org_telemetry_token_hash_eq(nearmiss, expected) == 0);
   printf("  token sha256 + constant-time compare: ok\n");
}

/* ---- 5. structural: org_telemetry is content-free by construction ---- */
static void test_no_content_column(void)
{
   const char *sql = AIMEE_DB2_SCHEMA_SQL;
   /* Isolate the CREATE TABLE ... org_telemetry ( ... ) body. */
   const char *decl = strstr(sql, "CREATE TABLE IF NOT EXISTS org_telemetry (");
   assert(decl != NULL);
   const char *body_end = strstr(decl, ");");
   assert(body_end != NULL);
   size_t len = (size_t)(body_end - decl);
   char body[4096];
   assert(len < sizeof(body));
   memcpy(body, decl, len);
   body[len] = '\0';
   /* NONE of these free-text/PII-bearing columns may exist. */
   assert(strstr(body, "payload") == NULL);
   assert(strstr(body, "content") == NULL);
   assert(strstr(body, "jsonb") == NULL);
   assert(strstr(body, " sub ") == NULL);
   assert(strstr(body, "extra") == NULL);
   /* The expected content-free columns ARE present. */
   assert(strstr(body, "source_event_id") != NULL);
   assert(strstr(body, "origin_cert_cn") != NULL);
   assert(strstr(body, "metric_name") != NULL);
   assert(strstr(body, "metric_kind") != NULL);
   printf("  org_telemetry content-free structural check: ok\n");
}

int main(void)
{
   printf("test_org_telemetry:\n");
   test_metric_name_valid();
   test_escape();
   test_render();
   test_render_model_escape();
   test_token();
   test_no_content_column();
   printf("test_org_telemetry: all passed\n");
   return 0;
}
