/* modules/aws/bedrock_policy.c: least-privilege Bedrock session policy. See header.
 * Pure transform; fail-closed; canonical stable-ordered JSON output. */

#include "bedrock_policy.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An ARN or id token is limited to a conservative safe charset so the emitted
 * JSON never needs escaping and a client string can't inject JSON/quotes. */
static int token_safe(const char *s)
{
   if (!s || !s[0])
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
   {
      unsigned char c = *p;
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == ':' || c == '/' || c == '.' || c == '-' || c == '_';
      if (!ok)
         return 0;
   }
   return 1;
}

/* A caller-supplied underlying foundation-model ARN must have the exact shape
 * arn:<partition>:bedrock:<region>::foundation-model/<id> for the SAME partition —
 * so a malformed, wildcard, or cross-service ARN cannot be smuggled verbatim into
 * the least-privilege Resource set. Rejects any '*' and any non-bedrock service. */
static int region_valid(const char *region)
{
   if (!region || !region[0] || strlen(region) > 63)
      return 0;
   for (const unsigned char *p = (const unsigned char *)region; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
         return 0;
   return 1;
}

static int account_valid(const char *account)
{
   if (!account || strlen(account) != 12)
      return 0;
   for (size_t i = 0; i < 12; i++)
      if (account[i] < '0' || account[i] > '9')
         return 0;
   return 1;
}

static int fm_arn_region(const char *arn, const char *part, char region[64])
{
   if (!token_safe(arn)) /* token_safe already forbids '*' and whitespace */
      return 0;
   char prefix[64];
   int pn = snprintf(prefix, sizeof(prefix), "arn:%s:bedrock:", part);
   if (pn <= 0 || (size_t)pn >= sizeof(prefix) || strncmp(arn, prefix, (size_t)pn) != 0)
      return 0;
   const char *start = arn + (size_t)pn;
   const char *suffix = strstr(start, "::foundation-model/");
   if (!suffix || suffix == start || strchr(start, ':') != suffix ||
       !suffix[strlen("::foundation-model/")])
      return 0;
   size_t n = (size_t)(suffix - start);
   if (n >= 64)
      return 0;
   memcpy(region, start, n);
   region[n] = '\0';
   return region_valid(region);
}

static int partition_valid(const char *p)
{
   return p && (strcmp(p, "aws") == 0 || strcmp(p, "aws-us-gov") == 0 || strcmp(p, "aws-cn") == 0);
}

/* A bounded, sorted, de-duplicated string set for the resource ARNs. */
#define MAX_RES 65
#define RES_LEN 512
typedef struct
{
   char v[MAX_RES][RES_LEN];
   size_t n;
} res_set_t;

static int res_add(res_set_t *s, const char *arn)
{
   if (!token_safe(arn) || strlen(arn) >= RES_LEN)
      return -1;
   for (size_t i = 0; i < s->n; i++)
      if (strcmp(s->v[i], arn) == 0)
         return 0; /* dedup */
   if (s->n >= MAX_RES)
      return -1;
   snprintf(s->v[s->n], RES_LEN, "%s", arn);
   s->n++;
   return 0;
}

static int res_addf(res_set_t *s, const char *fmt, ...)
{
   char buf[RES_LEN];
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (n < 0 || (size_t)n >= sizeof(buf))
      return -1;
   return res_add(s, buf);
}

static int cmp_str(const void *a, const void *b)
{
   return strcmp((const char *)a, (const char *)b);
}

int bedrock_session_policy(const bedrock_target_t *t, int is_streaming, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!t || !out || cap == 0)
      return -1;
   if (!partition_valid(t->partition))
      return -1;
   if (!token_safe(t->id))
      return -1;
   if (!region_valid(t->invoke_region) || !t->region_set || t->n_regions == 0 ||
       t->n_regions > 64 || t->n_underlying > 64)
      return -1;
   for (size_t i = 0; i < t->n_regions; i++)
      if (!region_valid(t->region_set[i]))
         return -1;

   res_set_t rs;
   memset(&rs, 0, sizeof(rs));
   const char *part = t->partition;

   switch (t->type)
   {
   case BEDROCK_TARGET_FOUNDATION:
      /* foundation-model ARN has no account segment (::). */
      if (t->n_regions != 1 || strcmp(t->region_set[0], t->invoke_region) != 0 ||
          t->n_underlying != 0)
         return -1;
      if (res_addf(&rs, "arn:%s:bedrock:%s::foundation-model/%s", part, t->invoke_region, t->id) !=
          0)
         return -1;
      break;

   case BEDROCK_TARGET_PROVISIONED:
   case BEDROCK_TARGET_CUSTOM:
   {
      if (!account_valid(t->account))
         return -1;
      if (t->n_regions != 1 || strcmp(t->region_set[0], t->invoke_region) != 0 ||
          t->n_underlying != 0)
         return -1;
      const char *kind =
          (t->type == BEDROCK_TARGET_PROVISIONED) ? "provisioned-model" : "custom-model";
      if (res_addf(&rs, "arn:%s:bedrock:%s:%s:%s/%s", part, t->invoke_region, t->account, kind,
                   t->id) != 0)
         return -1;
      break;
   }

   case BEDROCK_TARGET_APP_INFERENCE_PROFILE:
   case BEDROCK_TARGET_CROSS_REGION_INFERENCE_PROFILE:
   {
      /* A profile with no resolved underlying destination FMs is underivable. */
      if (t->n_underlying == 0 || !t->underlying_fm_arns)
         return -1;
      if (!account_valid(t->account))
         return -1;
      const char *kind = (t->type == BEDROCK_TARGET_APP_INFERENCE_PROFILE)
                             ? "application-inference-profile"
                             : "inference-profile";
      if (res_addf(&rs, "arn:%s:bedrock:%s:%s:%s/%s", part, t->invoke_region, t->account, kind,
                   t->id) != 0)
         return -1;
      /* PLUS every underlying destination-region foundation-model ARN. Each is
       * SHAPE-VALIDATED (arn:<part>:bedrock:<region>::foundation-model/<id>) before
       * emission so a malformed / cross-service / wildcard ARN in the target struct
       * fails closed rather than leaking an unrelated Resource into the IAM policy.
       * (Defence-in-depth atop the P6b invariant that the target is authoritative.) */
      int covered[64] = {0};
      for (size_t i = 0; i < t->n_underlying; i++)
      {
         char region[64];
         if (!fm_arn_region(t->underlying_fm_arns[i], part, region))
            return -1;
         int found = 0;
         for (size_t j = 0; j < t->n_regions; j++)
            if (strcmp(t->region_set[j], region) == 0)
            {
               found = covered[j] = 1;
               break;
            }
         if (!found)
            return -1;
         if (res_add(&rs, t->underlying_fm_arns[i]) != 0)
            return -1;
      }
      for (size_t i = 0; i < t->n_regions; i++)
         if (!covered[i])
            return -1;
      break;
   }

   default:
      return -1; /* unknown type -> fail closed */
   }

   if (rs.n == 0)
      return -1;
   qsort(rs.v, rs.n, RES_LEN, cmp_str);

   const char *action =
       is_streaming ? "bedrock:InvokeModelWithResponseStream" : "bedrock:InvokeModel";

   /* Emit canonical, stable-ordered JSON. */
   size_t o = 0;
   int n = snprintf(out + o, cap - o,
                    "{\"Version\":\"2012-10-17\",\"Statement\":[{\"Effect\":\"Allow\","
                    "\"Action\":[\"%s\"],\"Resource\":[",
                    action);
   if (n < 0 || (size_t)n >= cap - o)
      return (out[0] = '\0'), -1;
   o += (size_t)n;
   for (size_t i = 0; i < rs.n; i++)
   {
      n = snprintf(out + o, cap - o, "%s\"%s\"", i ? "," : "", rs.v[i]);
      if (n < 0 || (size_t)n >= cap - o)
         return (out[0] = '\0'), -1;
      o += (size_t)n;
   }
   n = snprintf(out + o, cap - o, "]}]}");
   if (n < 0 || (size_t)n >= cap - o)
      return (out[0] = '\0'), -1;
   return 0;
}
