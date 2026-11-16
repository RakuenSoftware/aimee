/* kb_vault_rotation_aws.c: IAM self-rotation of org-vault Bedrock credentials.
 * See kb_vault_rotation_aws.h for the protocol. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* strptime, timegm */
#endif
#include "kb_vault_rotation_aws.h"

#include "modules/aws/aws_sigv4.h"
#include "http/kb_http_client.h"
#include "kb_vault_key_use.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AWS_REGION   "us-east-1"
#define IAM_VERSION  "2010-05-08"
#define STS_VERSION  "2011-06-15"
#define RESPONSE_MAX (64U * 1024U)
#define KEY_ID_MAX   128
#define SECRET_MAX   256
#define ARN_MAX      512
#define LIST_MAX     8
#define REF_PREFIX   "aws-iam:"

static __thread const kb_principal_t *t_caller;
static __thread int64_t t_team;

void kb_vault_rotation_aws_set_caller(const kb_principal_t *caller, int64_t team_id)
{
   t_caller = caller;
   t_team = caller ? team_id : 0;
}

/* --- credentials ------------------------------------------------------- */

typedef struct
{
   char access_key_id[KEY_ID_MAX];
   char secret_access_key[SECRET_MAX];
} aws_cred_t;

static void cred_clear(aws_cred_t *c)
{
   OPENSSL_cleanse(c, sizeof(*c));
}

/* {"access_key_id":"...","secret_access_key":"..."}: exactly the two fields,
 * plain strings, the same shape the Bedrock egress reads. */
static int json_string(const unsigned char **p, const unsigned char *end, char *out, size_t cap)
{
   while (*p < end && (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r'))
      (*p)++;
   if (*p >= end || **p != '"')
      return -1;
   (*p)++;
   size_t n = 0;
   while (*p < end && **p != '"')
   {
      if (**p == '\\' || **p < 0x20 || n + 1 >= cap)
         return -1;
      out[n++] = (char)*(*p)++;
   }
   if (*p >= end)
      return -1;
   (*p)++;
   out[n] = '\0';
   return n ? 0 : -1;
}

static int skip(const unsigned char **p, const unsigned char *end, char c)
{
   while (*p < end && (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r'))
      (*p)++;
   if (*p >= end || **p != c)
      return -1;
   (*p)++;
   return 0;
}

static int cred_parse(const unsigned char *plain, size_t len, aws_cred_t *out)
{
   const unsigned char *p = plain, *end = plain + len;
   char key[32];
   int seen = 0;
   memset(out, 0, sizeof(*out));
   if (skip(&p, end, '{'))
      return -1;
   for (int i = 0; i < 2; i++)
   {
      if (i && skip(&p, end, ','))
         goto bad;
      if (json_string(&p, end, key, sizeof(key)) || skip(&p, end, ':'))
         goto bad;
      if (!strcmp(key, "access_key_id") && !(seen & 1))
      {
         if (json_string(&p, end, out->access_key_id, sizeof(out->access_key_id)))
            goto bad;
         seen |= 1;
      }
      else if (!strcmp(key, "secret_access_key") && !(seen & 2))
      {
         if (json_string(&p, end, out->secret_access_key, sizeof(out->secret_access_key)))
            goto bad;
         seen |= 2;
      }
      else
         goto bad;
   }
   if (skip(&p, end, '}'))
      goto bad;
   while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
      p++;
   if (p != end || seen != 3)
      goto bad;
   return 0;
bad:
   cred_clear(out);
   return -1;
}

/* --- references -------------------------------------------------------- */

/* aws-iam:<key id>[;arn=<user arn>;since=<epoch>] */
typedef struct
{
   char key_id[KEY_ID_MAX];
   char arn[ARN_MAX];
   long long since;
} aws_ref_t;

static int ref_parse(const char *text, aws_ref_t *out)
{
   memset(out, 0, sizeof(*out));
   if (!text || strncmp(text, REF_PREFIX, strlen(REF_PREFIX)))
      return -1;
   const char *p = text + strlen(REF_PREFIX), *semi = strchr(p, ';');
   size_t n = semi ? (size_t)(semi - p) : strlen(p);
   if (!n || n >= sizeof(out->key_id))
      return -1;
   memcpy(out->key_id, p, n);
   if (!semi)
      return 0;
   const char *arn = strstr(semi, ";arn="), *since = strstr(semi, ";since=");
   if (!arn || !since || since < arn)
      return -1;
   arn += 5;
   n = (size_t)(since - arn);
   if (!n || n >= sizeof(out->arn))
      return -1;
   memcpy(out->arn, arn, n);
   char *tail = NULL;
   out->since = strtoll(since + 7, &tail, 10);
   return tail && !*tail && out->since > 0 ? 0 : -1;
}

/* --- AWS Query API ------------------------------------------------------ */

typedef struct
{
   int status;
   size_t len;
   char body[RESPONSE_MAX + 1];
} aws_reply_t;

static void reply_clear(aws_reply_t *r)
{
   OPENSSL_cleanse(r, sizeof(*r));
}

typedef struct
{
   char *buf;
   size_t cap, len;
   int overflow;
} collect_t;

static kb_http_body_action_t collect_body(const unsigned char *bytes, size_t length, void *context)
{
   collect_t *c = context;
   if (length > c->cap - c->len)
   {
      c->overflow = 1;
      return KB_HTTP_BODY_CALLER_ABORT;
   }
   memcpy(c->buf + c->len, bytes, length);
   c->len += length;
   return KB_HTTP_BODY_CONTINUE;
}

static int tls_send(void *ctx, const char *authority, const char *const *names,
                    const char *const *values, size_t count, const char *body, size_t body_len,
                    int *status, char *response, size_t cap, size_t *response_len)
{
   (void)ctx;
   kb_http_header_t headers[8];
   if (count > sizeof(headers) / sizeof(headers[0]))
      return -1;
   for (size_t i = 0; i < count; i++)
      headers[i] = (kb_http_header_t){names[i], values[i]};
   kb_http_request_t request = {.authority = authority,
                                .method = "POST",
                                .target = "/",
                                .headers = headers,
                                .header_count = count,
                                .body = (const unsigned char *)body,
                                .body_len = body_len,
                                .response_body_max = cap,
                                .connect_timeout_ms = 5000,
                                .total_timeout_ms = 30000,
                                .deliver_error_body = 1};
   collect_t c = {response, cap, 0, 0};
   kb_http_response_t meta;
   kb_http_result_t rc = kb_http_tls_exchange(&request, &meta, NULL, collect_body, &c);
   if (rc == KB_HTTP_OK && !c.overflow)
   {
      *status = meta.status;
      *response_len = c.len;
      return 0;
   }
   /* Nothing can have reached AWS before the connection was up. */
   return rc == KB_HTTP_RESOLVE_ERROR || rc == KB_HTTP_CONNECT_ERROR ||
                  rc == KB_HTTP_INVALID_ARGUMENT
              ? -1
              : 1;
}

static time_t real_now(void)
{
   return time(NULL);
}

void kb_vault_rotation_aws_defaults(kb_vault_rotation_aws_t *aws)
{
   memset(aws, 0, sizeof(*aws));
   aws->send = tls_send;
   aws->now = real_now;
   aws->iam_authority = "iam.amazonaws.com";
   aws->sts_authority = "sts.amazonaws.com";
}

/* Sign and send one Query API call. Returns 0 with a reply, 1 when the outcome
 * at AWS is unknown, -1 when the request cannot have been acted on. */
static int aws_call(const kb_vault_rotation_aws_t *aws, const char *service, const char *authority,
                    const char *form, const aws_cred_t *cred, aws_reply_t *reply)
{
   time_t now = aws->now();
   struct tm tm;
   char amz_date[32], date[16], payload_hash[65];
   if (!gmtime_r(&now, &tm) || !strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", &tm) ||
       !strftime(date, sizeof(date), "%Y%m%d", &tm))
      return -1;
   aws_sha256_hex((const unsigned char *)form, strlen(form), payload_hash);
   const char *content_type = "application/x-www-form-urlencoded; charset=utf-8";
   aws_sigv4_kv_t signed_headers[] = {
       {"content-type", content_type}, {"host", authority}, {"x-amz-date", amz_date}};
   aws_sigv4_request_t req = {.method = "POST",
                              .raw_path = "/",
                              .headers = signed_headers,
                              .n_headers = 3,
                              .payload_hash = payload_hash,
                              .amz_date = amz_date,
                              .date = date,
                              .region = AWS_REGION,
                              .service = service,
                              .access_key_id = cred->access_key_id,
                              .access_key_id_len = strlen(cred->access_key_id),
                              .secret_access_key = cred->secret_access_key,
                              .secret_access_key_len = strlen(cred->secret_access_key)};
   aws_sigv4_result_t *sig = calloc(1, sizeof(*sig));
   if (!sig)
      return -1;
   if (aws_sigv4_sign(&req, sig) != 0)
   {
      OPENSSL_cleanse(sig, sizeof(*sig));
      free(sig);
      return -1;
   }
   const char *names[] = {"Host", "Content-Type", "X-Amz-Date", "Authorization"};
   const char *values[] = {authority, content_type, amz_date, sig->authorization};
   memset(reply, 0, sizeof(*reply));
   int rc = aws->send(aws->send_ctx, authority, names, values, 4, form, strlen(form),
                      &reply->status, reply->body, RESPONSE_MAX, &reply->len);
   OPENSSL_cleanse(sig, sizeof(*sig));
   free(sig);
   if (rc == 0)
      reply->body[reply->len] = '\0';
   return rc;
}

/* The text of the first <tag>...</tag> at or after *from; *from moves past it. */
static int xml_take(const char *xml, const char **from, const char *tag, char *out, size_t cap)
{
   char open[64], close[64];
   snprintf(open, sizeof(open), "<%s>", tag);
   snprintf(close, sizeof(close), "</%s>", tag);
   const char *start = strstr(*from, open);
   if (!start)
      return -1;
   start += strlen(open);
   const char *end = strstr(start, close);
   if (!end || (size_t)(end - start) >= cap)
      return -1;
   size_t n = 0;
   for (const char *p = start; p < end;)
   {
      if (*p == '&')
      {
         static const struct
         {
            const char *ent;
            char ch;
         } ents[] = {
             {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
         size_t i = 0;
         for (; i < sizeof(ents) / sizeof(ents[0]); i++)
            if (!strncmp(p, ents[i].ent, strlen(ents[i].ent)))
               break;
         if (i == sizeof(ents) / sizeof(ents[0]))
            return -1;
         out[n++] = ents[i].ch;
         p += strlen(ents[i].ent);
      }
      else
         out[n++] = *p++;
   }
   out[n] = '\0';
   (void)xml;
   *from = end + strlen(close);
   return 0;
}

static int xml_field(const char *xml, const char *tag, char *out, size_t cap)
{
   const char *from = xml;
   return xml_take(xml, &from, tag, out, cap);
}

static int error_code_is(const aws_reply_t *r, const char *code)
{
   char got[128];
   return r->status >= 400 && xml_field(r->body, "Code", got, sizeof(got)) == 0 &&
          !strcmp(got, code);
}

/* A definite AWS answer that will not change on retry. */
static int definite_refusal(const aws_reply_t *r)
{
   return r->status >= 400 && r->status < 500 && !error_code_is(r, "Throttling") &&
          !error_code_is(r, "RequestExpired");
}

static long long iso8601_epoch(const char *s)
{
   struct tm tm;
   memset(&tm, 0, sizeof(tm));
   const char *end = strptime(s, "%Y-%m-%dT%H:%M:%S", &tm);
   if (!end)
      return -1;
   if (*end == '.')
      while (*++end >= '0' && *end <= '9')
         ;
   if (*end != 'Z' || end[1])
      return -1;
   return (long long)timegm(&tm);
}

typedef struct
{
   char key_id[KEY_ID_MAX];
   long long created;
} aws_key_t;

/* ListAccessKeys for the calling user. Returns the count, or -1 on a definite
 * refusal, or -2 when the call should be retried. */
static int list_keys(const kb_vault_rotation_aws_t *aws, const aws_cred_t *cred, aws_key_t *keys,
                     int max)
{
   aws_reply_t *r = malloc(sizeof(*r));
   if (!r)
      return -2;
   int rc = aws_call(aws, "iam", aws->iam_authority, "Action=ListAccessKeys&Version=" IAM_VERSION,
                     cred, r);
   int n = -2;
   if (rc == 0 && r->status == 200)
   {
      char truncated[16] = "";
      if (xml_field(r->body, "IsTruncated", truncated, sizeof(truncated)) == 0 &&
          !strcmp(truncated, "true"))
         n = -1;
      else
      {
         n = 0;
         const char *from = r->body;
         char member[2048], created[64];
         while (xml_take(r->body, &from, "member", member, sizeof(member)) == 0)
         {
            if (n >= max ||
                xml_field(member, "AccessKeyId", keys[n].key_id, sizeof(keys[n].key_id)) != 0 ||
                xml_field(member, "CreateDate", created, sizeof(created)) != 0 ||
                (keys[n].created = iso8601_epoch(created)) < 0)
            {
               n = -1;
               break;
            }
            n++;
         }
      }
   }
   else if (rc == 0 && definite_refusal(r))
      n = -1;
   reply_clear(r);
   free(r);
   return n;
}

/* GetCallerIdentity: 0 with the ARN, 1 when the key is not (yet) accepted,
 * -1 on any other definite answer, -2 to retry. */
static int caller_arn(const kb_vault_rotation_aws_t *aws, const aws_cred_t *cred, char *arn,
                      size_t cap)
{
   aws_reply_t *r = malloc(sizeof(*r));
   if (!r)
      return -2;
   int rc = aws_call(aws, "sts", aws->sts_authority,
                     "Action=GetCallerIdentity&Version=" STS_VERSION, cred, r);
   int out = -2;
   if (rc == 0 && r->status == 200)
      out = xml_field(r->body, "Arn", arn, cap) == 0 ? 0 : -1;
   else if (rc == 0 &&
            (error_code_is(r, "InvalidClientTokenId") || error_code_is(r, "SignatureDoesNotMatch")))
      out = 1;
   else if (rc == 0 && definite_refusal(r))
      out = -1;
   reply_clear(r);
   free(r);
   return out;
}

/* DeleteAccessKey: 0 when deleted or already absent, -1 definite, -2 retry. */
static int delete_key(const kb_vault_rotation_aws_t *aws, const aws_cred_t *cred,
                      const char *key_id)
{
   char form[256];
   if (snprintf(form, sizeof(form), "AccessKeyId=%s&Action=DeleteAccessKey&Version=" IAM_VERSION,
                key_id) >= (int)sizeof(form))
      return -1;
   aws_reply_t *r = malloc(sizeof(*r));
   if (!r)
      return -2;
   int rc = aws_call(aws, "iam", aws->iam_authority, form, cred, r);
   int out = -2;
   if (rc == 0 && (r->status == 200 || error_code_is(r, "NoSuchEntity")))
      out = 0;
   else if (rc == 0 && definite_refusal(r))
      out = -1;
   reply_clear(r);
   free(r);
   return out;
}

static int key_id_valid(const char *s)
{
   size_t n = strlen(s);
   if (n < 16 || n >= KEY_ID_MAX)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')))
         return 0;
   return 1;
}

/* --- vault use ---------------------------------------------------------- */

typedef int (*with_cred_fn)(const aws_cred_t *cred, void *ctx);

typedef struct
{
   with_cred_fn fn;
   void *ctx;
   int result;
} use_ctx_t;

static int use_callback(const unsigned char *plain, size_t len, void *opaque)
{
   use_ctx_t *u = opaque;
   aws_cred_t cred;
   if (cred_parse(plain, len, &cred))
   {
      u->result = KB_VAULT_OP_DEFINITE_FAILURE;
      return -1;
   }
   u->result = u->fn(&cred, u->ctx);
   cred_clear(&cred);
   return 0;
}

/* Use the slot's current credential once, admitted and audited. */
static int with_current(const kb_store_vault_rotation_row_t *row, const char *op,
                        const char *operation, with_cred_fn fn, void *ctx)
{
   if (!t_caller || t_team != row->team_id)
      return KB_VAULT_OP_DEFINITE_FAILURE;
   unsigned char rnd[16];
   char use_id[33], digest[65];
   if (RAND_bytes(rnd, sizeof(rnd)) != 1)
      return KB_VAULT_OP_UNCERTAIN;
   for (size_t i = 0; i < sizeof(rnd); i++)
      snprintf(use_id + i * 2, 3, "%02x", rnd[i]);
   unsigned char hash[SHA256_DIGEST_LENGTH];
   char material[256];
   snprintf(material, sizeof(material), "%s|%s", op, operation);
   SHA256((const unsigned char *)material, strlen(material), hash);
   for (size_t i = 0; i < sizeof(hash); i++)
      snprintf(digest + i * 2, 3, "%02x", hash[i]);
   use_ctx_t u = {fn, ctx, KB_VAULT_OP_UNCERTAIN};
   kb_vault_key_use_status_t vu = kb_vault_key_use(
       t_caller, row->team_id, t_caller, use_id, row->key_id, row->principal, row->agent, row->cred,
       digest, "aws-iam", "iam", operation, use_callback, &u);
   if (vu == KB_VAULT_KEY_USE_OK || vu == KB_VAULT_KEY_USE_CALLBACK_FAILED)
      return u.result;
   return vu == KB_VAULT_KEY_USE_RETRY || vu == KB_VAULT_KEY_USE_SEALED
              ? KB_VAULT_OP_UNCERTAIN
              : KB_VAULT_OP_DEFINITE_FAILURE;
}

static int heartbeat(const kb_vault_rotation_lease_t *lease)
{
   return lease && lease->heartbeat && lease->heartbeat(lease->heartbeat_ctx) == 0 ? 0 : -1;
}

/* --- provider callbacks -------------------------------------------------- */

typedef struct
{
   const kb_vault_rotation_aws_t *aws;
   const kb_store_vault_rotation_row_t *row;
   const kb_vault_rotation_lease_t *lease;
   char *ref;
   size_t ref_cap;
   unsigned char *secret;
   size_t secret_cap;
   size_t *secret_len;
   int *flag;
   char *evidence;
   size_t evidence_cap;
   const char *target;
} step_ctx_t;

static int resolve_with(const aws_cred_t *cred, void *opaque)
{
   step_ctx_t *s = opaque;
   aws_ref_t prior;
   char arn[ARN_MAX];
   int rc = caller_arn(s->aws, cred, arn, sizeof(arn));
   if (rc == -2)
      return KB_VAULT_OP_UNCERTAIN;
   if (rc != 0)
      return KB_VAULT_OP_DEFINITE_FAILURE;
   /* A retried provision keeps the reference its first attempt recorded. */
   if (s->row->old_vendor_ref[0])
   {
      if (ref_parse(s->row->old_vendor_ref, &prior) != 0 || !prior.arn[0] ||
          strcmp(prior.key_id, cred->access_key_id) || strcmp(prior.arn, arn))
         return KB_VAULT_OP_DEFINITE_FAILURE;
      return snprintf(s->ref, s->ref_cap, "%s", s->row->old_vendor_ref) < (int)s->ref_cap
                 ? KB_VAULT_OP_OK
                 : KB_VAULT_OP_DEFINITE_FAILURE;
   }
   aws_key_t keys[LIST_MAX];
   int n = list_keys(s->aws, cred, keys, LIST_MAX);
   if (n == -2)
      return KB_VAULT_OP_UNCERTAIN;
   /* Exactly the vault's key: a second key belongs to someone else. */
   if (n != 1 || strcmp(keys[0].key_id, cred->access_key_id))
      return KB_VAULT_OP_DEFINITE_FAILURE;
   return snprintf(s->ref, s->ref_cap, REF_PREFIX "%s;arn=%s;since=%lld", cred->access_key_id, arn,
                   (long long)s->aws->now()) < (int)s->ref_cap
              ? KB_VAULT_OP_OK
              : KB_VAULT_OP_DEFINITE_FAILURE;
}

static int aws_resolve(void *ctx, const char *op, const kb_store_vault_rotation_row_t *row,
                       const kb_vault_rotation_lease_t *lease, char *vendor_ref, size_t ref_cap)
{
   step_ctx_t s = {.aws = ctx, .row = row, .lease = lease, .ref = vendor_ref, .ref_cap = ref_cap};
   if (!ctx || !op || !row || !vendor_ref || !ref_cap || heartbeat(lease))
      return KB_VAULT_OP_UNCERTAIN;
   vendor_ref[0] = '\0';
   return with_current(row, op, "rotate-resolve", resolve_with, &s);
}

static int provision_with(const aws_cred_t *cred, void *opaque)
{
   step_ctx_t *s = opaque;
   aws_ref_t old;
   if (ref_parse(s->row->old_vendor_ref, &old) != 0 || !old.arn[0] ||
       strcmp(old.key_id, cred->access_key_id))
      return KB_VAULT_OP_DEFINITE_FAILURE;
   aws_key_t keys[LIST_MAX];
   int n = list_keys(s->aws, cred, keys, LIST_MAX);
   if (n == -2)
      return KB_VAULT_OP_UNCERTAIN;
   if (n < 1)
      return KB_VAULT_OP_DEFINITE_FAILURE;
   int found_old = 0;
   for (int i = 0; i < n; i++)
   {
      if (!strcmp(keys[i].key_id, old.key_id))
      {
         found_old = 1;
         continue;
      }
      /* Only a key this rotation created: an earlier CreateAccessKey whose
       * response was lost. Its secret is gone, so it is revoked, not reused. */
      if (keys[i].created < old.since)
         return KB_VAULT_OP_DEFINITE_FAILURE;
      if (heartbeat(s->lease))
         return KB_VAULT_OP_UNCERTAIN;
      int d = delete_key(s->aws, cred, keys[i].key_id);
      if (d != 0)
         return d == -1 ? KB_VAULT_OP_DEFINITE_FAILURE : KB_VAULT_OP_UNCERTAIN;
   }
   if (!found_old || heartbeat(s->lease))
      return found_old ? KB_VAULT_OP_UNCERTAIN : KB_VAULT_OP_DEFINITE_FAILURE;
   aws_reply_t *r = malloc(sizeof(*r));
   if (!r)
      return KB_VAULT_OP_UNCERTAIN;
   int rc = aws_call(s->aws, "iam", s->aws->iam_authority,
                     "Action=CreateAccessKey&Version=" IAM_VERSION, cred, r);
   int out = KB_VAULT_OP_UNCERTAIN;
   if (rc == 0 && r->status == 200)
   {
      aws_cred_t fresh;
      memset(&fresh, 0, sizeof(fresh));
      if (xml_field(r->body, "AccessKeyId", fresh.access_key_id, sizeof(fresh.access_key_id)) ==
              0 &&
          xml_field(r->body, "SecretAccessKey", fresh.secret_access_key,
                    sizeof(fresh.secret_access_key)) == 0 &&
          key_id_valid(fresh.access_key_id) && fresh.secret_access_key[0])
      {
         int w = snprintf((char *)s->secret, s->secret_cap,
                          "{\"access_key_id\":\"%s\",\"secret_access_key\":\"%s\"}",
                          fresh.access_key_id, fresh.secret_access_key);
         if (w > 0 && (size_t)w < s->secret_cap &&
             snprintf(s->ref, s->ref_cap, REF_PREFIX "%s", fresh.access_key_id) < (int)s->ref_cap)
         {
            *s->secret_len = (size_t)w;
            *s->flag = 1; /* every orphan this rotation could have made is revoked */
            out = KB_VAULT_OP_OK;
         }
         else
         {
            OPENSSL_cleanse(s->secret, s->secret_cap);
            out = KB_VAULT_OP_DEFINITE_FAILURE;
         }
      }
      else
         out = KB_VAULT_OP_DEFINITE_FAILURE;
      cred_clear(&fresh);
   }
   else if (rc == 0 && definite_refusal(r))
      out = KB_VAULT_OP_DEFINITE_FAILURE;
   reply_clear(r);
   free(r);
   return out;
}

static int aws_provision(void *ctx, const char *op, const kb_store_vault_rotation_row_t *row,
                         const kb_vault_rotation_lease_t *lease, unsigned char *secret,
                         size_t secret_cap, size_t *secret_len, char *vendor_ref, size_t ref_cap,
                         int *prior_orphan_reconciled)
{
   if (!ctx || !op || !row || !secret || !secret_len || !vendor_ref || !prior_orphan_reconciled ||
       heartbeat(lease))
      return KB_VAULT_OP_UNCERTAIN;
   *secret_len = 0;
   *prior_orphan_reconciled = 0;
   vendor_ref[0] = '\0';
   step_ctx_t s = {.aws = ctx,
                   .row = row,
                   .lease = lease,
                   .ref = vendor_ref,
                   .ref_cap = ref_cap,
                   .secret = secret,
                   .secret_cap = secret_cap,
                   .secret_len = secret_len,
                   .flag = prior_orphan_reconciled};
   int rc = with_current(row, op, "rotate-provision", provision_with, &s);
   if (rc != KB_VAULT_OP_OK)
   {
      OPENSSL_cleanse(secret, secret_cap);
      *secret_len = 0;
      *prior_orphan_reconciled = 0;
   }
   return rc;
}

static int aws_probe(void *ctx, const char *op, const kb_store_vault_rotation_row_t *row,
                     const kb_vault_rotation_lease_t *lease, const unsigned char *secret,
                     size_t secret_len)
{
   const kb_vault_rotation_aws_t *aws = ctx;
   aws_ref_t old, staged;
   aws_cred_t cred;
   if (!aws || !op || !row || !secret || heartbeat(lease))
      return KB_VAULT_OP_UNCERTAIN;
   if (ref_parse(row->old_vendor_ref, &old) != 0 || !old.arn[0] ||
       ref_parse(row->new_vendor_ref, &staged) != 0 || cred_parse(secret, secret_len, &cred))
      return KB_VAULT_OP_DEFINITE_FAILURE;
   char arn[ARN_MAX];
   int rc =
       strcmp(cred.access_key_id, staged.key_id) ? -1 : caller_arn(aws, &cred, arn, sizeof(arn));
   cred_clear(&cred);
   if (rc == 0)
      return strcmp(arn, old.arn) ? KB_VAULT_OP_DEFINITE_FAILURE : KB_VAULT_OP_OK;
   /* A key IAM has not propagated to STS yet is not a failure: probe again. */
   return rc == 1 || rc == -2 ? KB_VAULT_OP_UNCERTAIN : KB_VAULT_OP_DEFINITE_FAILURE;
}

static int revoke_with(const aws_cred_t *cred, void *opaque)
{
   step_ctx_t *s = opaque;
   aws_ref_t target;
   if (ref_parse(s->target, &target) != 0)
      return KB_VAULT_OP_DEFINITE_FAILURE;
   /* Never revoke the key the vault is using. */
   if (!strcmp(target.key_id, cred->access_key_id))
      return KB_VAULT_OP_DEFINITE_FAILURE;
   int d = delete_key(s->aws, cred, target.key_id);
   if (d != 0)
      return d == -1 ? KB_VAULT_OP_DEFINITE_FAILURE : KB_VAULT_OP_UNCERTAIN;
   if (heartbeat(s->lease))
      return KB_VAULT_OP_UNCERTAIN;
   aws_key_t keys[LIST_MAX];
   int n = list_keys(s->aws, cred, keys, LIST_MAX);
   if (n < 0)
      return n == -2 ? KB_VAULT_OP_UNCERTAIN : KB_VAULT_OP_DEFINITE_FAILURE;
   for (int i = 0; i < n; i++)
      if (!strcmp(keys[i].key_id, target.key_id))
         return KB_VAULT_OP_UNCERTAIN; /* IAM still lists it: confirm again later */
   return snprintf(s->evidence, s->evidence_cap, "aws-iam:deleted:%s;listed-by:%s", target.key_id,
                   cred->access_key_id) < (int)s->evidence_cap
              ? KB_VAULT_OP_OK
              : KB_VAULT_OP_DEFINITE_FAILURE;
}

static int aws_revoke(void *ctx, const char *op, const kb_store_vault_rotation_row_t *row,
                      const kb_vault_rotation_lease_t *lease, const char *vendor_ref, char *receipt,
                      size_t receipt_cap)
{
   if (!ctx || !op || !row || !vendor_ref || !receipt || !receipt_cap || heartbeat(lease))
      return KB_VAULT_OP_UNCERTAIN;
   receipt[0] = '\0';
   step_ctx_t s = {.aws = ctx,
                   .row = row,
                   .lease = lease,
                   .evidence = receipt,
                   .evidence_cap = receipt_cap,
                   .target = vendor_ref};
   return with_current(row, op, "rotate-revoke", revoke_with, &s);
}

static int reconcile_with(const aws_cred_t *cred, void *opaque)
{
   step_ctx_t *s = opaque;
   aws_ref_t old;
   if (ref_parse(s->row->old_vendor_ref, &old) != 0 || !old.arn[0] ||
       strcmp(old.key_id, cred->access_key_id))
      return KB_VAULT_OP_DEFINITE_FAILURE;
   aws_key_t keys[LIST_MAX];
   int n = list_keys(s->aws, cred, keys, LIST_MAX);
   if (n < 0)
      return n == -2 ? KB_VAULT_OP_UNCERTAIN : KB_VAULT_OP_DEFINITE_FAILURE;
   for (int i = 0; i < n; i++)
   {
      if (!strcmp(keys[i].key_id, old.key_id) || keys[i].created < old.since)
         continue;
      *s->flag = 1;
      return snprintf(s->ref, s->ref_cap, REF_PREFIX "%s", keys[i].key_id) < (int)s->ref_cap
                 ? KB_VAULT_OP_OK
                 : KB_VAULT_OP_DEFINITE_FAILURE;
   }
   *s->flag = 0;
   return snprintf(s->evidence, s->evidence_cap, "aws-iam:no-key-since:%lld;listed-by:%s",
                   old.since, cred->access_key_id) < (int)s->evidence_cap
              ? KB_VAULT_OP_OK
              : KB_VAULT_OP_DEFINITE_FAILURE;
}

static int aws_reconcile(void *ctx, const char *op, const kb_store_vault_rotation_row_t *row,
                         const kb_vault_rotation_lease_t *lease, char *vendor_ref, size_t ref_cap,
                         int *exists, char *evidence, size_t evidence_cap)
{
   if (!ctx || !op || !row || !vendor_ref || !exists || !evidence || heartbeat(lease))
      return KB_VAULT_OP_UNCERTAIN;
   vendor_ref[0] = '\0';
   evidence[0] = '\0';
   *exists = 0;
   /* A provision that never recorded the old key made nothing to clean up. */
   if (!row->old_vendor_ref[0])
      return snprintf(evidence, evidence_cap, "aws-iam:not-provisioned") < (int)evidence_cap
                 ? KB_VAULT_OP_OK
                 : KB_VAULT_OP_DEFINITE_FAILURE;
   step_ctx_t s = {.aws = ctx,
                   .row = row,
                   .lease = lease,
                   .ref = vendor_ref,
                   .ref_cap = ref_cap,
                   .flag = exists,
                   .evidence = evidence,
                   .evidence_cap = evidence_cap};
   return with_current(row, op, "rotate-reconcile", reconcile_with, &s);
}

int kb_vault_rotation_aws_identity(const kb_vault_rotation_aws_t *aws, const unsigned char *secret,
                                   size_t secret_len, char *arn, size_t arn_cap)
{
   if (arn && arn_cap)
      arn[0] = '\0';
   aws_cred_t cred;
   if (!aws || !secret || !arn || arn_cap < 2 || cred_parse(secret, secret_len, &cred))
      return KB_VAULT_OP_DEFINITE_FAILURE;
   int rc = caller_arn(aws, &cred, arn, arn_cap);
   cred_clear(&cred);
   if (rc == 0)
      return KB_VAULT_OP_OK;
   return rc == -2 ? KB_VAULT_OP_UNCERTAIN : KB_VAULT_OP_DEFINITE_FAILURE;
}

static const kb_vault_rotation_provider_t g_provider = {aws_resolve, aws_provision, aws_probe,
                                                        aws_revoke, aws_reconcile};

const kb_vault_rotation_provider_t *kb_vault_rotation_aws_provider(void)
{
   return &g_provider;
}
