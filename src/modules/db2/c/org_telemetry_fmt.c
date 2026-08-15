/* db2/org_telemetry_fmt.c: pure P9a telemetry helpers. See org_telemetry_fmt.h.
 *
 * Prometheus text rendering + label escaping, the metric_name PII-structural
 * validator, and the scrape/ingest token SHA-256 + constant-time compare. No
 * libpq — unit-testable standalone (links only libc + OpenSSL). */

#include "org_telemetry_fmt.h"

#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

int org_telemetry_metric_name_valid(const char *s)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, 129);
   if (n < 1 || n > 128)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)s[i];
      int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == ':';
      if (!ok)
         return 0;
   }
   return 1;
}

int org_telemetry_prom_escape(const char *in, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   if (!in)
      return 0;
   size_t o = 0;
   for (const char *p = in; *p; ++p)
   {
      const char *rep = NULL;
      char two[3];
      if (*p == '\\')
         rep = "\\\\";
      else if (*p == '"')
         rep = "\\\"";
      else if (*p == '\n')
         rep = "\\n";
      else
      {
         two[0] = *p;
         two[1] = '\0';
         rep = two;
      }
      size_t rl = strlen(rep);
      if (o + rl + 1 > cap)
      {
         out[0] = '\0';
         return -1;
      }
      memcpy(out + o, rep, rl);
      o += rl;
   }
   out[o] = '\0';
   return 0;
}

/* The fixed metric catalog: HELP text + TYPE for each authoritative series.
 * Rendering looks a row's metric up here; an unknown metric name is skipped (a
 * defensive guard — the snapshot only ever emits these). */
typedef struct
{
   const char *name;
   const char *type; /* "counter" | "gauge" */
   const char *help;
} prom_meta_t;

static const prom_meta_t k_meta[] = {
    {"aimee_org_spend_usd", "counter", "Cumulative attributed spend in USD, by team."},
    {"aimee_org_budget_limit_usd", "gauge", "Configured budget cap in USD, by team and period."},
    {"aimee_org_budget_reserved_usd", "gauge",
     "Currently reserved (in-flight) budget in USD, by team and period."},
    {"aimee_org_budget_spend_usd", "gauge", "Settled period spend in USD, by team and period."},
    {"aimee_org_catalog_models", "gauge", "Number of enabled models in the org catalog."},
    {"aimee_org_audit_events_total", "counter", "Total governance audit events recorded."},
    {"aimee_org_teams", "gauge", "Number of teams in the org."},
    {"aimee_org_witness_evidence_records", "gauge",
     "Witness evidence records currently retained on this kb."},
    {"aimee_org_witness_shards", "gauge", "Non-empty witness evidence shards."},
    {"aimee_org_witness_checkpoint_seq", "gauge",
     "Sequence of the latest signed witness checkpoint (0 if none)."},
    {"aimee_org_witness_checkpoint_age_seconds", "gauge",
     "Age of the latest signed witness checkpoint; a growing value means new signed roots "
     "have stopped."},
    {"aimee_org_witness_emit_backlog_records", "gauge",
     "Witness evidence records not yet published on the log/OTLP path. A backlog that only "
     "grows means retained off-host copies are falling behind."},
    {"aimee_org_witness_emit_backlog_checkpoints", "gauge",
     "Signed witness checkpoints not yet published on the log/OTLP path."},
};

static const prom_meta_t *meta_lookup(const char *name)
{
   for (size_t i = 0; i < sizeof(k_meta) / sizeof(k_meta[0]); i++)
      if (strcmp(k_meta[i].name, name) == 0)
         return &k_meta[i];
   return NULL;
}

/* Append s to out[cap] at *o (NUL-terminated). Returns 0 or -1 on overflow. */
static int append(char *out, size_t cap, size_t *o, const char *s)
{
   size_t l = strlen(s);
   if (*o + l + 1 > cap)
      return -1;
   memcpy(out + *o, s, l);
   *o += l;
   out[*o] = '\0';
   return 0;
}

int org_telemetry_render_prom(const org_metric_row_t *rows, int n, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   size_t o = 0;
   const char *cur = ""; /* the metric family whose HELP/TYPE was last emitted */
   for (int i = 0; i < n; i++)
   {
      const org_metric_row_t *r = &rows[i];
      const prom_meta_t *m = meta_lookup(r->metric);
      if (!m)
         continue; /* defensive: never emit an unknown series */
      if (strcmp(cur, r->metric) != 0)
      {
         char line[512];
         snprintf(line, sizeof(line), "# HELP %s %s\n# TYPE %s %s\n", m->name, m->help, m->name,
                  m->type);
         if (append(out, cap, &o, line) != 0)
            return -1;
         cur = m->name;
      }
      /* Build the bounded label set. team = numeric id; period ∈ {day,month};
       * model = catalog name (escaped). Absent labels (team<0 / empty) are
       * omitted. No ingested/free-text value ever becomes a label here. */
      char labels[ORG_TELEMETRY_MODEL_MAX + 64] = "";
      size_t lo = 0;
      int first = 1;
      labels[0] = '\0';
      if (r->team >= 0)
      {
         char t[48];
         snprintf(t, sizeof(t), "%steam=\"%lld\"", first ? "" : ",", r->team);
         if (lo + strlen(t) + 1 > sizeof(labels))
            return -1;
         memcpy(labels + lo, t, strlen(t) + 1);
         lo += strlen(t);
         first = 0;
      }
      if (r->period[0])
      {
         char esc[ORG_TELEMETRY_PERIOD_MAX * 2 + 4];
         if (org_telemetry_prom_escape(r->period, esc, sizeof(esc)) != 0)
            return -1;
         char t[64];
         snprintf(t, sizeof(t), "%speriod=\"%s\"", first ? "" : ",", esc);
         if (lo + strlen(t) + 1 > sizeof(labels))
            return -1;
         memcpy(labels + lo, t, strlen(t) + 1);
         lo += strlen(t);
         first = 0;
      }
      if (r->model[0])
      {
         char esc[ORG_TELEMETRY_MODEL_MAX * 2 + 4];
         if (org_telemetry_prom_escape(r->model, esc, sizeof(esc)) != 0)
            return -1;
         char t[ORG_TELEMETRY_MODEL_MAX * 2 + 32];
         snprintf(t, sizeof(t), "%smodel=\"%s\"", first ? "" : ",", esc);
         if (lo + strlen(t) + 1 > sizeof(labels))
            return -1;
         memcpy(labels + lo, t, strlen(t) + 1);
         lo += strlen(t);
         first = 0;
      }
      /* value defaults to 0 when the DB returned NULL/empty. */
      const char *val = r->value[0] ? r->value : "0";
      char line[ORG_TELEMETRY_MODEL_MAX * 2 + 256];
      if (labels[0])
         snprintf(line, sizeof(line), "%s{%s} %s\n", m->name, labels, val);
      else
         snprintf(line, sizeof(line), "%s %s\n", m->name, val);
      if (append(out, cap, &o, line) != 0)
         return -1;
   }
   return (int)o;
}

void org_telemetry_sha256_hex(const char *s, char out[65])
{
   unsigned char d[SHA256_DIGEST_LENGTH];
   SHA256((const unsigned char *)(s ? s : ""), s ? strlen(s) : 0, d);
   static const char hexd[] = "0123456789abcdef";
   for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
   {
      out[i * 2] = hexd[d[i] >> 4];
      out[i * 2 + 1] = hexd[d[i] & 0x0f];
   }
   out[64] = '\0';
}

int org_telemetry_token_hash_eq(const char *presented_hex, const char *expected_hex)
{
   if (!presented_hex || !expected_hex)
      return 0;
   /* A configured hash must be a full 64-char sha256 hex; a malformed expected
    * value never authorizes. */
   if (strnlen(expected_hex, 65) != 64 || strnlen(presented_hex, 65) != 64)
      return 0;
   /* Constant-time: accumulate the XOR of every byte pair; no early return. */
   volatile unsigned char diff = 0;
   for (int i = 0; i < 64; i++)
      diff |= (unsigned char)(presented_hex[i] ^ expected_hex[i]);
   return diff == 0 ? 1 : 0;
}
