/* test_kb_vault_rotation_aws.c: the IAM self-rotation provider against a fake AWS.
 *
 * The fake answers the Query API calls the provider makes (ListAccessKeys,
 * CreateAccessKey, DeleteAccessKey, GetCallerIdentity) from a small model of one
 * IAM user, and checks each request is SigV4-signed by the key it claims. The
 * vault use is a double that hands the callback the slot's current credential. */
#include "kb/kb_vault_rotation_aws.h"
#include "kb/kb_vault_key_use.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USER_ARN "arn:aws:iam::123456789012:user/aimee-bedrock"

typedef struct
{
   char id[32];
   char secret[48];
   long long created;
   int live;
} fake_key_t;

static fake_key_t g_keys[4];
static long long g_now = 1793260000;
static int g_next = 1;
static int g_lose_create_response; /* create succeeds at AWS, reply lost */
static int g_unpropagated;         /* STS rejects a key this many more times */
static int g_list_lags;            /* list still shows a deleted key this many times */
static int g_calls_create, g_calls_delete;
static char g_current[160]; /* the slot's current credential JSON */

static time_t fake_now(void)
{
   return (time_t)g_now;
}

static fake_key_t *find(const char *id)
{
   for (int i = 0; i < 4; i++)
      if (g_keys[i].live && !strcmp(g_keys[i].id, id))
         return &g_keys[i];
   return NULL;
}

static fake_key_t *add_key(long long created)
{
   for (int i = 0; i < 4; i++)
      if (!g_keys[i].live)
      {
         snprintf(g_keys[i].id, sizeof(g_keys[i].id), "AKIATESTKEY%09d", g_next);
         snprintf(g_keys[i].secret, sizeof(g_keys[i].secret), "secret/%d+xyz", g_next++);
         g_keys[i].created = created;
         g_keys[i].live = 1;
         return &g_keys[i];
      }
   return NULL;
}

static void iso(long long t, char out[32])
{
   time_t tt = (time_t)t;
   struct tm tm;
   gmtime_r(&tt, &tm);
   strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static const char *signer(const char *const *names, const char *const *values, size_t n)
{
   static char id[64];
   for (size_t i = 0; i < n; i++)
      if (!strcmp(names[i], "Authorization"))
      {
         const char *p = strstr(values[i], "Credential=");
         assert(p);
         p += strlen("Credential=");
         size_t k = strcspn(p, "/");
         assert(k < sizeof(id));
         memcpy(id, p, k);
         id[k] = '\0';
         return id;
      }
   return NULL;
}

static int fake_send(void *ctx, const char *authority, const char *const *names,
                     const char *const *values, size_t count, const char *body, size_t body_len,
                     int *status, char *out, size_t cap, size_t *out_len)
{
   (void)ctx;
   (void)body_len;
   int host = 0;
   for (size_t i = 0; i < count; i++)
      if (!strcmp(names[i], "Host"))
         host = !strcmp(values[i], authority);
   assert(host);
   const char *by = signer(names, values, count);
   assert(by);
   fake_key_t *caller = find(by);
   int n;
   if (!strcmp(authority, "sts.example"))
   {
      assert(strstr(body, "Action=GetCallerIdentity"));
      if (!caller || (g_unpropagated > 0 && g_unpropagated--))
      {
         *status = 403;
         n = snprintf(out, cap,
                      "<ErrorResponse><Error><Code>InvalidClientTokenId</Code>"
                      "</Error></ErrorResponse>");
      }
      else
      {
         *status = 200;
         n = snprintf(out, cap,
                      "<GetCallerIdentityResponse><GetCallerIdentityResult><Arn>" USER_ARN
                      "</Arn></GetCallerIdentityResult></GetCallerIdentityResponse>");
      }
      *out_len = (size_t)n;
      return 0;
   }
   assert(!strcmp(authority, "iam.example"));
   if (!caller)
   {
      *status = 403;
      *out_len = (size_t)snprintf(out, cap,
                                  "<ErrorResponse><Error><Code>InvalidClientTokenId"
                                  "</Code></Error></ErrorResponse>");
      return 0;
   }
   if (strstr(body, "Action=ListAccessKeys"))
   {
      size_t at = (size_t)snprintf(out, cap,
                                   "<ListAccessKeysResponse><ListAccessKeysResult>"
                                   "<AccessKeyMetadata>");
      for (int i = 0; i < 4; i++)
         if (g_keys[i].live || (g_list_lags > 0 && g_keys[i].id[0] && !g_keys[i].live))
         {
            char when[32];
            iso(g_keys[i].created, when);
            at += (size_t)snprintf(out + at, cap - at,
                                   "<member><UserName>aimee-bedrock</UserName><AccessKeyId>%s"
                                   "</AccessKeyId><Status>Active</Status><CreateDate>%s"
                                   "</CreateDate></member>",
                                   g_keys[i].id, when);
         }
      if (g_list_lags > 0)
         g_list_lags--;
      at += (size_t)snprintf(out + at, cap - at,
                             "</AccessKeyMetadata><IsTruncated>false</IsTruncated>"
                             "</ListAccessKeysResult></ListAccessKeysResponse>");
      *status = 200;
      *out_len = at;
      return 0;
   }
   if (strstr(body, "Action=CreateAccessKey"))
   {
      g_calls_create++;
      fake_key_t *k = add_key(g_now);
      assert(k);
      if (g_lose_create_response)
      {
         g_lose_create_response = 0;
         return 1;
      }
      *status = 200;
      *out_len = (size_t)snprintf(out, cap,
                                  "<CreateAccessKeyResponse><CreateAccessKeyResult><AccessKey>"
                                  "<AccessKeyId>%s</AccessKeyId><SecretAccessKey>%s"
                                  "</SecretAccessKey><Status>Active</Status></AccessKey>"
                                  "</CreateAccessKeyResult></CreateAccessKeyResponse>",
                                  k->id, k->secret);
      return 0;
   }
   if (strstr(body, "Action=DeleteAccessKey"))
   {
      g_calls_delete++;
      const char *p = strstr(body, "AccessKeyId=");
      assert(p);
      char id[32];
      p += strlen("AccessKeyId=");
      size_t k = strcspn(p, "&");
      memcpy(id, p, k);
      id[k] = '\0';
      fake_key_t *key = find(id);
      if (!key)
      {
         *status = 404;
         *out_len = (size_t)snprintf(out, cap,
                                     "<ErrorResponse><Error><Code>NoSuchEntity</Code>"
                                     "</Error></ErrorResponse>");
         return 0;
      }
      key->live = 0;
      *status = 200;
      *out_len = (size_t)snprintf(out, cap, "<DeleteAccessKeyResponse/>");
      return 0;
   }
   assert(!"unexpected IAM action");
   return -1;
}

/* The vault use double: the slot's current credential, nothing else. */
kb_vault_key_use_status_t
kb_vault_key_use(const kb_principal_t *caller, int64_t team_id, const kb_principal_t *origin,
                 const char *use_id, const char *key_id, const char *principal, const char *agent,
                 const char *cred, const char *digest, const char *provider, const char *model,
                 const char *operation, kb_vault_key_use_fn callback, void *ctx)
{
   assert(caller && origin && team_id == 7 && use_id && strlen(use_id) == 32);
   assert(!strcmp(key_id, "team:7|bedrock|primary") && !strcmp(principal, "team:7"));
   assert(!strcmp(agent, "bedrock") && !strcmp(cred, "primary") && strlen(digest) == 64);
   assert(!strcmp(provider, "aws-iam") && !strcmp(model, "iam") &&
          !strncmp(operation, "rotate-", 7));
   return callback((const unsigned char *)g_current, strlen(g_current), ctx) == 0
              ? KB_VAULT_KEY_USE_OK
              : KB_VAULT_KEY_USE_CALLBACK_FAILED;
}

static int beat(void *ctx)
{
   (void)ctx;
   return 0;
}

static void use_key(const fake_key_t *k)
{
   snprintf(g_current, sizeof(g_current), "{\"access_key_id\":\"%s\",\"secret_access_key\":\"%s\"}",
            k->id, k->secret);
}

int main(void)
{
   kb_vault_rotation_aws_t aws;
   kb_vault_rotation_aws_defaults(&aws);
   aws.send = fake_send;
   aws.now = fake_now;
   aws.iam_authority = "iam.example";
   aws.sts_authority = "sts.example";
   const kb_vault_rotation_provider_t *p = kb_vault_rotation_aws_provider();
   const kb_principal_t owner = {.kind = KB_PRIN_OWNER, .subject = "owner", .authenticated = 1};
   const kb_vault_rotation_lease_t lease = {30, beat, NULL};
   kb_store_vault_rotation_row_t row;
   memset(&row, 0, sizeof(row));
   row.id = 42;
   row.team_id = 7;
   row.has_team = 1;
   row.from_version = 1;
   row.to_version = 2;
   snprintf(row.key_id, sizeof(row.key_id), "team:7|bedrock|primary");
   snprintf(row.principal, sizeof(row.principal), "team:7");
   snprintf(row.agent, sizeof(row.agent), "bedrock");
   snprintf(row.cred, sizeof(row.cred), "primary");
   char ref[513], receipt[513];
   unsigned char secret[4096];
   size_t secret_len = 0;
   int reconciled = 0, exists = 0;

   fake_key_t *old = add_key(g_now - 86400);
   use_key(old);

   /* Without the caller set, no vault use is attempted. */
   assert(p->resolve_current(&aws, "op", &row, &lease, ref, sizeof(ref)) ==
          KB_VAULT_OP_DEFINITE_FAILURE);
   kb_vault_rotation_aws_set_caller(&owner, 7);

   /* A second key the operator owns blocks the rotation; nothing is touched. */
   fake_key_t *foreign = add_key(g_now - 3600);
   assert(p->resolve_current(&aws, "op", &row, &lease, ref, sizeof(ref)) ==
          KB_VAULT_OP_DEFINITE_FAILURE);
   assert(foreign->live && !g_calls_delete);
   foreign->live = 0;
   foreign->id[0] = '\0';

   /* resolve records the key, the user and the start time. */
   assert(p->resolve_current(&aws, "op", &row, &lease, ref, sizeof(ref)) == KB_VAULT_OP_OK);
   char want[256];
   snprintf(want, sizeof(want), "aws-iam:%s;arn=" USER_ARN ";since=%lld", old->id, g_now);
   assert(!strcmp(ref, want));
   snprintf(row.old_vendor_ref, sizeof(row.old_vendor_ref), "%s", ref);

   /* A lost CreateAccessKey response is uncertain; the retry revokes the
    * orphan it left before creating the key it stages. */
   g_now += 5;
   g_lose_create_response = 1;
   assert(p->provision(&aws, "op", &row, &lease, secret, sizeof(secret), &secret_len, ref,
                       sizeof(ref), &reconciled) == KB_VAULT_OP_UNCERTAIN);
   assert(!secret_len && !reconciled && g_calls_create == 1);
   /* the retried resolve keeps the recorded reference despite the second key */
   assert(p->resolve_current(&aws, "op", &row, &lease, ref, sizeof(ref)) == KB_VAULT_OP_OK &&
          !strcmp(ref, row.old_vendor_ref));
   assert(p->provision(&aws, "op", &row, &lease, secret, sizeof(secret), &secret_len, ref,
                       sizeof(ref), &reconciled) == KB_VAULT_OP_OK);
   assert(reconciled && g_calls_create == 2 && g_calls_delete == 1);
   fake_key_t *fresh = NULL;
   for (int i = 0; i < 4; i++)
      if (g_keys[i].live && &g_keys[i] != old)
         fresh = &g_keys[i];
   assert(fresh);
   snprintf(want, sizeof(want), "{\"access_key_id\":\"%s\",\"secret_access_key\":\"%s\"}",
            fresh->id, fresh->secret);
   assert(secret_len == strlen(want) && !memcmp(secret, want, secret_len));
   snprintf(want, sizeof(want), "aws-iam:%s", fresh->id);
   assert(!strcmp(ref, want));
   snprintf(row.new_vendor_ref, sizeof(row.new_vendor_ref), "%s", ref);

   /* A key STS has not seen yet is probed again, not failed. */
   g_unpropagated = 1;
   assert(p->probe(&aws, "op", &row, &lease, secret, secret_len) == KB_VAULT_OP_UNCERTAIN);
   assert(p->probe(&aws, "op", &row, &lease, secret, secret_len) == KB_VAULT_OP_OK);
   /* A staged secret for some other key is a definite failure. */
   const char *wrong = "{\"access_key_id\":\"AKIAOTHERKEY0000001\",\"secret_access_key\":\"x\"}";
   assert(p->probe(&aws, "op", &row, &lease, (const unsigned char *)wrong, strlen(wrong)) ==
          KB_VAULT_OP_DEFINITE_FAILURE);

   /* After activation the new key is current. It never revokes itself... */
   use_key(fresh);
   assert(p->revoke(&aws, "op", &row, &lease, row.new_vendor_ref, receipt, sizeof(receipt)) ==
          KB_VAULT_OP_DEFINITE_FAILURE);
   assert(fresh->live);
   /* ...and revokes the old one only when IAM stops listing it. */
   g_list_lags = 1;
   assert(p->revoke(&aws, "op", &row, &lease, row.old_vendor_ref, receipt, sizeof(receipt)) ==
          KB_VAULT_OP_UNCERTAIN);
   assert(!old->live);
   assert(p->revoke(&aws, "op", &row, &lease, row.old_vendor_ref, receipt, sizeof(receipt)) ==
          KB_VAULT_OP_OK);
   snprintf(want, sizeof(want), "aws-iam:deleted:%s;listed-by:%s", old->id, fresh->id);
   assert(!strcmp(receipt, want));

   /* Remediation of a provision that failed after creating a key it never
    * staged: reconcile reports that key so it is revoked. */
   kb_store_vault_rotation_row_t failed = row;
   failed.new_vendor_ref[0] = '\0';
   use_key(fresh);
   snprintf(failed.old_vendor_ref, sizeof(failed.old_vendor_ref),
            "aws-iam:%s;arn=" USER_ARN ";since=%lld", fresh->id, g_now);
   g_now += 5;
   fake_key_t *stray = add_key(g_now);
   assert(p->reconcile(&aws, "op", &failed, &lease, ref, sizeof(ref), &exists, receipt,
                       sizeof(receipt)) == KB_VAULT_OP_OK);
   snprintf(want, sizeof(want), "aws-iam:%s", stray->id);
   assert(exists && !strcmp(ref, want));
   stray->live = 0;
   assert(p->reconcile(&aws, "op", &failed, &lease, ref, sizeof(ref), &exists, receipt,
                       sizeof(receipt)) == KB_VAULT_OP_OK);
   assert(!exists && !strncmp(receipt, "aws-iam:no-key-since:", 21));

   kb_vault_rotation_aws_set_caller(NULL, 0);
   puts("PASS: IAM self-rotation provider (resolve, orphan reconcile, probe, revoke, remediate)");
   return 0;
}

/* The production transport is not exercised here. */
#include "kb/http/kb_http_client.h"
kb_http_result_t kb_http_tls_exchange(const kb_http_request_t *request,
                                      kb_http_response_t *response, kb_http_headers_fn headers_cb,
                                      kb_http_body_fn body_cb, void *context)
{
   (void)request;
   (void)response;
   (void)headers_cb;
   (void)body_cb;
   (void)context;
   return KB_HTTP_CONNECT_ERROR;
}
