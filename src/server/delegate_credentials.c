/* delegate_credentials.c: see delegate_credentials.h */

#include "delegate_credentials.h"
#include "util.h"

#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>

typedef struct
{
   char principal[VAULT_PRINCIPAL_MAX]; /* "" = shared env pool; uid:/webuser: = vault */
   char agent_name[MAX_AGENT_NAME];
   char cred_name[MAX_CRED_NAME_LEN];
   int leased;                  /* 1 = currently leased */
   long long cooldown_until_ms; /* monotonic ms; 0 = not cooling */
   time_t cooldown_until_epoch; /* wall clock for status/backoff visibility */
   delegate_credential_status_t status;
   delegate_ratelimit_state_t rl;
   char last_error[128];
} cred_lease_state_t;

#define INITIAL_LEASE_STATE_CAP 64

static cred_lease_state_t *g_state;
static int g_state_count = 0;
static int g_state_cap = 0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static long long now_monotonic_ms(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static time_t now_epoch_seconds(void)
{
   time_t now = time(NULL);
   return now > 0 ? now : 0;
}

/* NULL principal is normalised to the shared "" pool. */
static const char *pr_norm(const char *principal)
{
   return (principal && principal[0]) ? principal : "";
}

/* Caller holds g_mu. Locate or create the per-(principal, agent, cred) row. */
static cred_lease_state_t *find_or_create_locked(const char *principal, const char *agent_name,
                                                 const char *cred_name, int *allocation_failed)
{
   const char *pr = pr_norm(principal);
   if (allocation_failed)
      *allocation_failed = 0;
   for (int i = 0; i < g_state_count; i++)
   {
      if (strcmp(g_state[i].principal, pr) == 0 && strcmp(g_state[i].agent_name, agent_name) == 0 &&
          strcmp(g_state[i].cred_name, cred_name) == 0)
         return &g_state[i];
   }
   if (g_state_count >= g_state_cap)
   {
      int new_cap = g_state_cap > 0 ? g_state_cap * 2 : INITIAL_LEASE_STATE_CAP;
      cred_lease_state_t *grown = realloc(g_state, (size_t)new_cap * sizeof(*g_state));
      if (!grown)
      {
         if (allocation_failed)
            *allocation_failed = 1;
         return NULL;
      }
      memset(grown + g_state_cap, 0, (size_t)(new_cap - g_state_cap) * sizeof(*grown));
      g_state = grown;
      g_state_cap = new_cap;
   }
   cred_lease_state_t *slot = &g_state[g_state_count++];
   memset(slot, 0, sizeof(*slot));
   snprintf(slot->principal, sizeof(slot->principal), "%s", pr);
   snprintf(slot->agent_name, sizeof(slot->agent_name), "%s", agent_name);
   snprintf(slot->cred_name, sizeof(slot->cred_name), "%s", cred_name);
   slot->status = DELEGATE_CRED_STATUS_OK;
   slot->rl.req_limit = slot->rl.req_remaining = -1;
   slot->rl.req_limit_1h = slot->rl.req_remaining_1h = -1;
   slot->rl.tok_limit = slot->rl.tok_remaining = -1;
   slot->rl.tok_limit_1h = slot->rl.tok_remaining_1h = -1;
   return slot;
}

static int state_is_available(cred_lease_state_t *slot, long long now_ms, time_t now_epoch)
{
   if (!slot || slot->leased)
      return 0;
   if (slot->cooldown_until_ms != 0 && now_ms < slot->cooldown_until_ms)
      return 0;
   if (slot->status == DELEGATE_CRED_STATUS_RATE_LIMITED && slot->cooldown_until_epoch > 0 &&
       now_epoch >= slot->cooldown_until_epoch)
   {
      slot->status = DELEGATE_CRED_STATUS_OK;
      slot->cooldown_until_epoch = 0;
      slot->cooldown_until_ms = 0;
      slot->last_error[0] = '\0';
   }
   return slot->status == DELEGATE_CRED_STATUS_OK;
}

static void state_refresh_wallclock_locked(time_t now_epoch)
{
   long long now_ms = now_monotonic_ms();
   for (int i = 0; i < g_state_count; i++)
      (void)state_is_available(&g_state[i], now_ms, now_epoch);
}

static long long state_headroom(const cred_lease_state_t *slot)
{
   long long best = LLONG_MAX / 4;
   if (!slot)
      return -1;
   if (slot->rl.req_remaining >= 0 && slot->rl.req_remaining < best)
      best = slot->rl.req_remaining;
   if (slot->rl.req_remaining_1h >= 0 && slot->rl.req_remaining_1h < best)
      best = slot->rl.req_remaining_1h;
   if (slot->rl.tok_remaining >= 0 && slot->rl.tok_remaining < best)
      best = slot->rl.tok_remaining;
   if (slot->rl.tok_remaining_1h >= 0 && slot->rl.tok_remaining_1h < best)
      best = slot->rl.tok_remaining_1h;
   return best;
}

int delegate_credentials_acquire(const char *principal, const char *agent_name,
                                 const agent_credential_t *creds, int credential_count,
                                 char *out_name, size_t out_name_cap, char *out_env_var,
                                 size_t out_env_var_cap)
{
   if (!agent_name || !agent_name[0] || !creds || credential_count <= 0)
      return -1;
   if (!out_name || out_name_cap == 0 || !out_env_var || out_env_var_cap == 0)
      return -1;

   long long now = now_monotonic_ms();
   time_t epoch = now_epoch_seconds();
   pthread_mutex_lock(&g_mu);

   cred_lease_state_t *best_slot = NULL;
   const agent_credential_t *best_cred = NULL;
   long long best_headroom = -1;
   for (int i = 0; i < credential_count; i++)
   {
      const agent_credential_t *cred = &creds[i];
      if (!cred->name[0])
         continue;
      int allocation_failed = 0;
      cred_lease_state_t *slot =
          find_or_create_locked(principal, agent_name, cred->name, &allocation_failed);
      if (allocation_failed)
         break;
      if (!slot)
         continue;
      if (!state_is_available(slot, now, epoch))
         continue;
      long long headroom = state_headroom(slot);
      if (!best_slot || headroom > best_headroom)
      {
         best_slot = slot;
         best_cred = cred;
         best_headroom = headroom;
      }
   }

   if (best_slot && best_cred)
   {
      best_slot->leased = 1;
      snprintf(out_name, out_name_cap, "%s", best_cred->name);
      snprintf(out_env_var, out_env_var_cap, "%s", best_cred->api_key_env);
      pthread_mutex_unlock(&g_mu);
      return 0;
   }

   pthread_mutex_unlock(&g_mu);
   return -1;
}

void delegate_credentials_release(const char *principal, const char *agent_name,
                                  const char *cred_name)
{
   if (!agent_name || !agent_name[0] || !cred_name || !cred_name[0])
      return;
   const char *pr = pr_norm(principal);
   pthread_mutex_lock(&g_mu);
   for (int i = 0; i < g_state_count; i++)
   {
      if (strcmp(g_state[i].principal, pr) == 0 && strcmp(g_state[i].agent_name, agent_name) == 0 &&
          strcmp(g_state[i].cred_name, cred_name) == 0)
      {
         g_state[i].leased = 0;
         break;
      }
   }
   pthread_mutex_unlock(&g_mu);
}

void delegate_credentials_cooldown(const char *principal, const char *agent_name,
                                   const char *cred_name, int seconds)
{
   if (!agent_name || !agent_name[0] || !cred_name || !cred_name[0] || seconds <= 0)
      return;
   long long until = now_monotonic_ms() + (long long)seconds * 1000LL;
   time_t epoch_until = now_epoch_seconds() + (time_t)seconds;
   pthread_mutex_lock(&g_mu);
   cred_lease_state_t *slot = find_or_create_locked(principal, agent_name, cred_name, NULL);
   if (slot)
   {
      slot->cooldown_until_ms = until;
      slot->cooldown_until_epoch = epoch_until;
      slot->status = DELEGATE_CRED_STATUS_RATE_LIMITED;
   }
   pthread_mutex_unlock(&g_mu);
}

int delegate_credentials_cooldown_remaining(const char *principal, const char *agent_name,
                                            const char *cred_name, time_t now_epoch)
{
   if (!agent_name || !agent_name[0] || !cred_name || !cred_name[0])
      return 0;
   if (now_epoch <= 0)
      now_epoch = now_epoch_seconds();
   const char *pr = pr_norm(principal);
   long long now_ms = now_monotonic_ms();
   int remaining = 0;
   pthread_mutex_lock(&g_mu);
   for (int i = 0; i < g_state_count; i++)
   {
      if (strcmp(g_state[i].principal, pr) != 0 || strcmp(g_state[i].agent_name, agent_name) != 0 ||
          strcmp(g_state[i].cred_name, cred_name) != 0)
         continue;
      /* Honour whichever clock still shows the credential cooling: the monotonic
       * deadline (live process) and the wall-clock epoch (restored from disk). */
      if (g_state[i].cooldown_until_ms > now_ms)
      {
         int ms = (int)(g_state[i].cooldown_until_ms - now_ms);
         remaining = ms / 1000 + (ms % 1000 ? 1 : 0);
      }
      if (g_state[i].cooldown_until_epoch > now_epoch)
      {
         int sec = (int)(g_state[i].cooldown_until_epoch - now_epoch);
         if (sec > remaining)
            remaining = sec;
      }
      break;
   }
   pthread_mutex_unlock(&g_mu);
   return remaining;
}

int delegate_credentials_is_rate_limit(const char *error)
{
   if (!error || !error[0])
      return 0;
   return strstr(error, "429") != NULL || strstr(error, "rate limit") != NULL ||
          strstr(error, "rate_limit") != NULL || strstr(error, "RATE_LIMIT") != NULL ||
          strstr(error, "overloaded") != NULL;
}

static int parse_http_status_hint(const char *error)
{
   if (!error || !error[0])
      return 0;
   for (const char *p = error; *p; p++)
   {
      if (!isdigit((unsigned char)*p))
         continue;
      if ((p == error || !isalnum((unsigned char)p[-1])) && isdigit((unsigned char)p[1]) &&
          isdigit((unsigned char)p[2]) && !isdigit((unsigned char)p[3]))
      {
         int status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
         if (status >= 100 && status <= 599)
            return status;
      }
   }
   return 0;
}

failover_reason_t delegate_credentials_classify_failure(const char *provider, const char *error)
{
   (void)provider;
   if (!error || !error[0])
      return FAILOVER_NONE;
   int status = parse_http_status_hint(error);
   if (status == 401)
      return FAILOVER_AUTH;
   if (status == 403)
      return FAILOVER_AUTH_PERMANENT;
   if (status == 402)
      return FAILOVER_BILLING;
   if (status == 429)
      return FAILOVER_RATE_LIMIT;
   if (delegate_credentials_is_rate_limit(error))
      return FAILOVER_RATE_LIMIT;
   if (str_contains_ci(error, "insufficient quota") || str_contains_ci(error, "credit balance") ||
       str_contains_ci(error, "billing") || str_contains_ci(error, "payment required") ||
       str_contains_ci(error, "quota exceeded") || str_contains_ci(error, "out of credits"))
      return FAILOVER_BILLING;
   if (str_contains_ci(error, "unauthorized") || str_contains_ci(error, "invalid api key"))
      return FAILOVER_AUTH;
   if (str_contains_ci(error, "forbidden"))
      return FAILOVER_AUTH_PERMANENT;
   return FAILOVER_NONE;
}

const char *delegate_credential_status_name(delegate_credential_status_t status)
{
   switch (status)
   {
   case DELEGATE_CRED_STATUS_OK:
      return "ok";
   case DELEGATE_CRED_STATUS_RATE_LIMITED:
      return "rate_limited";
   case DELEGATE_CRED_STATUS_EXHAUSTED:
      return "exhausted";
   case DELEGATE_CRED_STATUS_AUTH_FAILED:
      return "auth_failed";
   }
   return "unknown";
}

static int ascii_eq_ci(char a, char b)
{
   return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static int header_name_eq(const char *line, size_t name_len, const char *name)
{
   size_t want = strlen(name);
   if (name_len != want)
      return 0;
   for (size_t i = 0; i < want; i++)
      if (!ascii_eq_ci(line[i], name[i]))
         return 0;
   return 1;
}

static long parse_header_long(const char *value)
{
   while (*value && isspace((unsigned char)*value))
      value++;
   return strtol(value, NULL, 10);
}

static void capture_header(delegate_ratelimit_state_t *rl, const char *line, size_t name_len,
                           const char *value, time_t now_epoch)
{
   if (header_name_eq(line, name_len, "x-ratelimit-limit-requests"))
      rl->req_limit = (int)parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-remaining-requests"))
      rl->req_remaining = (int)parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-reset-requests"))
      rl->req_reset = now_epoch + (time_t)parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-limit-requests-1h"))
      rl->req_limit_1h = (int)parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-remaining-requests-1h"))
      rl->req_remaining_1h = (int)parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-reset-requests-1h"))
      rl->req_reset_1h = now_epoch + (time_t)parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-limit-tokens"))
      rl->tok_limit = parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-remaining-tokens"))
      rl->tok_remaining = parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-reset-tokens"))
      rl->tok_reset = now_epoch + (time_t)parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-limit-tokens-1h"))
      rl->tok_limit_1h = parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-remaining-tokens-1h"))
      rl->tok_remaining_1h = parse_header_long(value);
   else if (header_name_eq(line, name_len, "x-ratelimit-reset-tokens-1h"))
      rl->tok_reset_1h = now_epoch + (time_t)parse_header_long(value);
}

int delegate_credentials_capture_headers(const char *principal, const char *agent_name,
                                         const char *cred_name, const char *raw_headers,
                                         time_t now_epoch)
{
   if (!agent_name || !agent_name[0] || !cred_name || !cred_name[0] || !raw_headers)
      return -1;
   if (now_epoch <= 0)
      now_epoch = now_epoch_seconds();

   pthread_mutex_lock(&g_mu);
   cred_lease_state_t *slot = find_or_create_locked(principal, agent_name, cred_name, NULL);
   if (!slot)
   {
      pthread_mutex_unlock(&g_mu);
      return -1;
   }

   for (const char *line = raw_headers; line && *line;)
   {
      const char *end = strpbrk(line, "\r\n");
      size_t len = end ? (size_t)(end - line) : strlen(line);
      const char *colon = memchr(line, ':', len);
      if (colon && colon > line)
      {
         size_t name_len = (size_t)(colon - line);
         const char *value = colon + 1;
         capture_header(&slot->rl, line, name_len, value, now_epoch);
      }
      if (!end)
         break;
      line = end + 1;
      while (*line == '\r' || *line == '\n')
         line++;
   }
   slot->rl.captured_at = now_epoch;
   pthread_mutex_unlock(&g_mu);
   return 0;
}

static time_t earliest_reset(const delegate_ratelimit_state_t *rl)
{
   time_t best = 0;
   const time_t vals[] = {rl->req_reset, rl->req_reset_1h, rl->tok_reset, rl->tok_reset_1h};
   for (int i = 0; i < 4; i++)
      if (vals[i] > 0 && (best == 0 || vals[i] < best))
         best = vals[i];
   return best;
}

int delegate_credentials_report_failure(const char *principal, const char *agent_name,
                                        const char *cred_name, failover_reason_t reason,
                                        const char *error, time_t now_epoch)
{
   if (!agent_name || !agent_name[0] || !cred_name || !cred_name[0])
      return 0;
   if (now_epoch <= 0)
      now_epoch = now_epoch_seconds();

   pthread_mutex_lock(&g_mu);
   cred_lease_state_t *slot = find_or_create_locked(principal, agent_name, cred_name, NULL);
   if (!slot)
   {
      pthread_mutex_unlock(&g_mu);
      return 0;
   }

   int changed = 1;
   if (reason == FAILOVER_RATE_LIMIT || reason == FAILOVER_OVERLOADED)
   {
      time_t until = earliest_reset(&slot->rl);
      if (until <= now_epoch)
         until = now_epoch + DELEGATE_CRED_COOLDOWN_SECONDS_DEFAULT;
      slot->status = DELEGATE_CRED_STATUS_RATE_LIMITED;
      slot->cooldown_until_epoch = until;
      slot->cooldown_until_ms = now_monotonic_ms() + (long long)(until - now_epoch) * 1000LL;
   }
   else if (reason == FAILOVER_BILLING)
   {
      slot->status = DELEGATE_CRED_STATUS_EXHAUSTED;
      slot->cooldown_until_epoch = 0;
      slot->cooldown_until_ms = 0;
   }
   else if (reason == FAILOVER_AUTH || reason == FAILOVER_AUTH_PERMANENT)
   {
      slot->status = DELEGATE_CRED_STATUS_AUTH_FAILED;
      slot->cooldown_until_epoch = 0;
      slot->cooldown_until_ms = 0;
   }
   else
      changed = 0;

   if (changed && error && error[0])
      snprintf(slot->last_error, sizeof(slot->last_error), "%s", error);
   pthread_mutex_unlock(&g_mu);
   return changed;
}

int delegate_credentials_rotate_after_failure(const char *principal, const char *agent_name,
                                              const agent_credential_t *creds, int credential_count,
                                              const char *provider, char *leased_cred_name,
                                              size_t leased_cred_name_cap, char *out_env_var,
                                              size_t out_env_var_cap, const char *error,
                                              time_t now_epoch)
{
   if (!agent_name || !agent_name[0] || !creds || credential_count <= 0 || !leased_cred_name ||
       leased_cred_name_cap == 0 || !leased_cred_name[0] || !out_env_var || out_env_var_cap == 0)
      return -1;
   failover_reason_t reason = delegate_credentials_classify_failure(provider, error);
   if (reason == FAILOVER_NONE)
      return 0;

   (void)delegate_credentials_report_failure(principal, agent_name, leased_cred_name, reason, error,
                                             now_epoch);
   delegate_credentials_release(principal, agent_name, leased_cred_name);
   leased_cred_name[0] = '\0';
   out_env_var[0] = '\0';
   return delegate_credentials_acquire(principal, agent_name, creds, credential_count,
                                       leased_cred_name, leased_cred_name_cap, out_env_var,
                                       out_env_var_cap) == 0
              ? 1
              : 0;
}

int delegate_credentials_snapshot(const char *agent_name, delegate_credential_snapshot_t *out,
                                  int max)
{
   if (!out || max <= 0)
      return 0;
   int n = 0;
   pthread_mutex_lock(&g_mu);
   for (int i = 0; i < g_state_count && n < max; i++)
   {
      /* Operator view = the shared env pool only; per-principal vault rows
       * (principal != "") are a per-user concern, not surfaced here. */
      if (g_state[i].principal[0])
         continue;
      if (agent_name && agent_name[0] && strcmp(agent_name, g_state[i].agent_name) != 0)
         continue;
      snprintf(out[n].agent_name, sizeof(out[n].agent_name), "%s", g_state[i].agent_name);
      snprintf(out[n].cred_name, sizeof(out[n].cred_name), "%s", g_state[i].cred_name);
      out[n].status = g_state[i].status;
      out[n].leased = g_state[i].leased;
      out[n].cooldown_until = g_state[i].cooldown_until_epoch;
      snprintf(out[n].last_error, sizeof(out[n].last_error), "%s", g_state[i].last_error);
      out[n].rl = g_state[i].rl;
      n++;
   }
   pthread_mutex_unlock(&g_mu);
   return n;
}

int delegate_credentials_format_quota(const char *agent_name, char *buf, size_t buf_len)
{
   if (!buf || buf_len == 0)
      return -1;
   buf[0] = '\0';
   delegate_credential_snapshot_t rows[64];
   int n = delegate_credentials_snapshot(agent_name, rows, 64);
   size_t used = 0;
   for (int i = 0; i < n; i++)
   {
      int wrote = snprintf(buf + used, buf_len - used,
                           "%s/%s status=%s leased=%d req=%d/%d tok=%ld/%ld cooldown=%lld\n",
                           rows[i].agent_name, rows[i].cred_name,
                           delegate_credential_status_name(rows[i].status), rows[i].leased,
                           rows[i].rl.req_remaining, rows[i].rl.req_limit, rows[i].rl.tok_remaining,
                           rows[i].rl.tok_limit, (long long)rows[i].cooldown_until);
      if (wrote < 0 || used + (size_t)wrote >= buf_len)
      {
         buf[buf_len - 1] = '\0';
         return -1;
      }
      used += (size_t)wrote;
   }
   if (n == 0)
      snprintf(buf, buf_len, "no credential pool state\n");
   return n;
}

static void sanitize_field(char *dst, size_t dst_len, const char *src)
{
   if (!dst || dst_len == 0)
      return;
   size_t n = 0;
   if (src)
   {
      for (const char *p = src; *p && n + 1 < dst_len; p++)
      {
         unsigned char ch = (unsigned char)*p;
         dst[n++] = (ch == '\t' || ch == '\r' || ch == '\n') ? ' ' : (char)ch;
      }
   }
   dst[n] = '\0';
}

int delegate_credentials_save_file(const char *path)
{
   if (!path || !path[0])
      return -1;

   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   FILE *fp = fopen(tmp, "w");
   if (!fp)
      return -1;

   pthread_mutex_lock(&g_mu);
   state_refresh_wallclock_locked(now_epoch_seconds());
   /* v2 adds a leading principal column; v1 readers are gone but v1 files still
    * load (principal defaults to the shared ""). */
   fprintf(fp, "aimee-delegate-credential-state-v2\n");
   for (int i = 0; i < g_state_count; i++)
   {
      char principal[VAULT_PRINCIPAL_MAX];
      char agent[MAX_AGENT_NAME];
      char cred[MAX_CRED_NAME_LEN];
      char last_error[sizeof(g_state[i].last_error)];
      sanitize_field(principal, sizeof(principal), g_state[i].principal);
      sanitize_field(agent, sizeof(agent), g_state[i].agent_name);
      sanitize_field(cred, sizeof(cred), g_state[i].cred_name);
      sanitize_field(last_error, sizeof(last_error), g_state[i].last_error);
      fprintf(fp,
              "%s\t%s\t%s\t%d\t%lld\t%d\t%d\t%lld\t%d\t%d\t%lld\t%ld\t%ld\t%lld\t%ld\t%ld\t%lld\t%"
              "lld\t%s\n",
              principal, agent, cred, (int)g_state[i].status,
              (long long)g_state[i].cooldown_until_epoch, g_state[i].rl.req_limit,
              g_state[i].rl.req_remaining, (long long)g_state[i].rl.req_reset,
              g_state[i].rl.req_limit_1h, g_state[i].rl.req_remaining_1h,
              (long long)g_state[i].rl.req_reset_1h, g_state[i].rl.tok_limit,
              g_state[i].rl.tok_remaining, (long long)g_state[i].rl.tok_reset,
              g_state[i].rl.tok_limit_1h, g_state[i].rl.tok_remaining_1h,
              (long long)g_state[i].rl.tok_reset_1h, (long long)g_state[i].rl.captured_at,
              last_error);
   }
   pthread_mutex_unlock(&g_mu);

   int write_failed = ferror(fp);
   if (fclose(fp) != 0)
      write_failed = 1;
   if (write_failed)
      return -1;
   if (rename(tmp, path) != 0)
      return -1;
   return 0;
}

static char *next_field(char **cursor)
{
   if (!cursor || !*cursor)
      return NULL;
   char *field = *cursor;
   char *tab = strchr(field, '\t');
   if (tab)
   {
      *tab = '\0';
      *cursor = tab + 1;
   }
   else
      *cursor = NULL;
   return field;
}

static int parse_int_field(char **cursor, int fallback)
{
   char *field = next_field(cursor);
   if (!field || !field[0])
      return fallback;
   return (int)strtol(field, NULL, 10);
}

static long parse_long_field(char **cursor, long fallback)
{
   char *field = next_field(cursor);
   if (!field || !field[0])
      return fallback;
   return strtol(field, NULL, 10);
}

static time_t parse_time_field(char **cursor, time_t fallback)
{
   char *field = next_field(cursor);
   if (!field || !field[0])
      return fallback;
   return (time_t)strtoll(field, NULL, 10);
}

int delegate_credentials_load_file(const char *path, time_t now_epoch)
{
   if (!path || !path[0])
      return -1;
   FILE *fp = fopen(path, "r");
   if (!fp)
      return errno == ENOENT ? 0 : -1;
   if (now_epoch <= 0)
      now_epoch = now_epoch_seconds();
   long long now_ms = now_monotonic_ms();

   char line[1024];
   int loaded = 0;
   if (!fgets(line, sizeof(line), fp))
   {
      fclose(fp);
      return 0;
   }
   int has_principal_col; /* v2 prepends a principal field; v1 has none */
   if (strncmp(line, "aimee-delegate-credential-state-v2", 34) == 0)
      has_principal_col = 1;
   else if (strncmp(line, "aimee-delegate-credential-state-v1", 34) == 0)
      has_principal_col = 0;
   else
   {
      fclose(fp);
      return -1;
   }

   pthread_mutex_lock(&g_mu);
   while (fgets(line, sizeof(line), fp))
   {
      line[strcspn(line, "\r\n")] = '\0';
      char *cursor = line;
      char *principal = has_principal_col ? next_field(&cursor) : "";
      char *agent = next_field(&cursor);
      char *cred = next_field(&cursor);
      if (!principal)
         principal = "";
      if (!agent || !agent[0] || !cred || !cred[0])
         continue;
      cred_lease_state_t *slot = find_or_create_locked(principal, agent, cred, NULL);
      if (!slot)
         continue;
      slot->leased = 0;
      slot->status = (delegate_credential_status_t)parse_int_field(&cursor, 0);
      slot->cooldown_until_epoch = parse_time_field(&cursor, 0);
      slot->rl.req_limit = parse_int_field(&cursor, -1);
      slot->rl.req_remaining = parse_int_field(&cursor, -1);
      slot->rl.req_reset = parse_time_field(&cursor, 0);
      slot->rl.req_limit_1h = parse_int_field(&cursor, -1);
      slot->rl.req_remaining_1h = parse_int_field(&cursor, -1);
      slot->rl.req_reset_1h = parse_time_field(&cursor, 0);
      slot->rl.tok_limit = parse_long_field(&cursor, -1);
      slot->rl.tok_remaining = parse_long_field(&cursor, -1);
      slot->rl.tok_reset = parse_time_field(&cursor, 0);
      slot->rl.tok_limit_1h = parse_long_field(&cursor, -1);
      slot->rl.tok_remaining_1h = parse_long_field(&cursor, -1);
      slot->rl.tok_reset_1h = parse_time_field(&cursor, 0);
      slot->rl.captured_at = parse_time_field(&cursor, 0);
      if (cursor && cursor[0])
         snprintf(slot->last_error, sizeof(slot->last_error), "%s", cursor);
      else
         slot->last_error[0] = '\0';

      if (slot->status == DELEGATE_CRED_STATUS_RATE_LIMITED &&
          slot->cooldown_until_epoch > now_epoch)
         slot->cooldown_until_ms =
             now_ms + (long long)(slot->cooldown_until_epoch - now_epoch) * 1000LL;
      else if (slot->status == DELEGATE_CRED_STATUS_RATE_LIMITED)
      {
         slot->status = DELEGATE_CRED_STATUS_OK;
         slot->cooldown_until_epoch = 0;
         slot->cooldown_until_ms = 0;
         slot->last_error[0] = '\0';
      }
      else
         slot->cooldown_until_ms = 0;
      loaded++;
   }
   pthread_mutex_unlock(&g_mu);
   fclose(fp);
   return loaded;
}

void delegate_credentials_reset_for_test(void)
{
   pthread_mutex_lock(&g_mu);
   free(g_state);
   g_state = NULL;
   g_state_count = 0;
   g_state_cap = 0;
   pthread_mutex_unlock(&g_mu);
}
