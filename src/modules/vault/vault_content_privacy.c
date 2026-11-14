#include "vault_content_privacy.h"
#include "vault_crypto.h"
#include "vault_server_key.h"
#include "vault_service.h"
#include "request_context.h"
#include "util.h"
#include "cJSON.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#pragma weak request_context_get
extern cJSON *kb_service_command_context(void) __attribute__((weak));
#include <openssl/crypto.h>
#include <pthread.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_MAX         (1024u * 1024u)
#define VALUE_MAX        (60u * 1024u)
#define REF_PREFIX       "[[vault:v1:"
#define REF_PREFIX_BYTES (sizeof(REF_PREFIX) - 1u)
#define REF_BYTES        (REF_PREFIX_BYTES + 36u + 2u)
/* Bounded recognizers, including explicit labels. They are deliberately local:
 * detection itself must not put raw input on the bus or call a model/provider. */
static const char *patterns[] = {
    "[^[:space:]<>\"'(),;:]+@[^[:space:]<>\"'(),;:]+",
    "(api[_-]?key|password|passwd|token|secret|credential|ssn|social[_. ]security|date[_. ]of[_. "
    "]birth|dob)[[:space:]]*[:=][[:space:]]*[^[:space:]]+",
    "(AKIA[0-9A-Z]{16}|ghp_[A-Za-z0-9]{10,}|github_pat_[A-Za-z0-9_]+)",
    "-----BEGIN[^\r\n]*PRIVATE KEY-----(.|[\r\n])*-----END[^\r\n]*PRIVATE KEY-----",
    "[0-9]{1,3}(\\.[0-9]{1,3}){3}",
    "[[:xdigit:]:]*:[[:xdigit:]:]*:[[:xdigit:]:]*",
    "[+]?[0-9][0-9 ()-]{7,}[0-9]",
    "(person[_ -]?name|full[_ -]?name|postal[_ -]?address|street[_ -]?address|passport([_ "
    "-]?(number|id))?)[[:space:]:=]+[^\r\n]+",
    "[0-9]{1,6}[[:space:]]+([A-Z][a-z]+[[:space:]]+){1,5}(Lane|Street|Road|Avenue|Drive|Way|Court|"
    "Ln|St|Rd|Ave)(,|[[:space:]])[^\r\n]*",
    "[A-Z][a-z]+([ -][A-Z][a-z]+){1,3}",
    "[0-9]{4}([ -][0-9]{4}){3}"};
static regex_t compiled[sizeof(patterns) / sizeof(patterns[0])];
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int ready;
static void initialize(void)
{
   ready = 1;
   for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i)
      if (regcomp(&compiled[i], patterns[i], REG_EXTENDED | (i == 9 ? 0 : REG_ICASE)))
         ready = 0;
}
int vault_content_privacy_ready(void)
{
   pthread_once(&once, initialize);
   return ready ? 0 : -1;
}
static int uuid(const char *s)
{
   if (strlen(s) != 36)
      return 0;
   for (int i = 0; i < 36; ++i)
      if ((i == 8 || i == 13 || i == 18 || i == 23) ? s[i] != '-' : !isxdigit((unsigned char)s[i]))
         return 0;
   return 1;
}
static int locator(const char *s, size_t n)
{
   if (n != REF_BYTES || strncmp(s, REF_PREFIX, REF_PREFIX_BYTES) ||
       memcmp(s + REF_BYTES - 2, "]]", 2))
      return 0;
   char id[37];
   memcpy(id, s + REF_PREFIX_BYTES, 36);
   id[36] = 0;
   return uuid(id);
}
static int inside_uuid(const char *text, size_t start)
{
   size_t from = start >= 36 ? start - 35 : 0;
   for (size_t i = from; i <= start; ++i)
   {
      if (strlen(text + i) < 36)
         continue;
      char candidate[37];
      memcpy(candidate, text + i, 36);
      candidate[36] = 0;
      if (uuid(candidate) && start < i + 36)
         return 1;
   }
   return 0;
}
static int inside_locator(const char *text, size_t start)
{
   size_t from = start >= REF_BYTES ? start - REF_BYTES + 1 : 0;
   for (size_t i = from; i <= start; ++i)
      if (strlen(text + i) >= REF_BYTES && locator(text + i, REF_BYTES) && start < i + REF_BYTES)
         return 1;
   return 0;
}
static int intake_principal(char out[VAULT_PRINCIPAL_MAX])
{
   /* The KB owns verifier/scope resolution independently of the Server TLS
    * slot. Read its trusted host constructor; never request JSON identities. */
   if (kb_service_command_context)
   {
      cJSON *context = kb_service_command_context();
      const char *principal =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(context, "principal"));
      const char *transport =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(context, "transport_identity"));
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(context, "authenticated")) && principal &&
          principal[0] && transport && transport[0])
      {
         size_t n = strlen(principal), m = strlen(transport);
         unsigned char digest[SHA256_DIGEST_LENGTH];
         char *bound = malloc(n + m + 2);
         if (!bound)
         {
            cJSON_Delete(context);
            return -1;
         }
         memcpy(bound, principal, n + 1);
         memcpy(bound + n + 1, transport, m + 1);
         SHA256((unsigned char *)bound, n + m + 2, digest);
         free(bound);
         memcpy(out, "kb-content:", 11);
         for (size_t i = 0; i < sizeof(digest); ++i)
            snprintf(out + 11 + i * 2, 3, "%02x", digest[i]);
         cJSON_Delete(context);
         return 0;
      }
      cJSON_Delete(context);
   }
   const request_context_t *ctx = request_context_get ? request_context_get() : NULL;
   if (!ctx || !ctx->vault_principal[0] || ctx->vault_transport == ATTEST_NONE)
      return -1;
   snprintf(out, VAULT_PRINCIPAL_MAX, "%s", ctx->vault_principal);
   return 0;
}
static int vault_value(const char *value, size_t n, char out[50])
{
   char principal[VAULT_PRINCIPAL_MAX];
   if (intake_principal(principal) || !n || n > VALUE_MAX)
      return -1;
   unsigned char kek[VAULT_KEK_LEN], digest[EVP_MAX_MD_SIZE];
   char key[65], id[37], generation[VAULT_IMMUTABLE_GENERATION_CAP];
   char *secret = malloc(n + 1), *bound = NULL;
   int rc = -1;
   if (!secret)
      return -1;
   memcpy(secret, value, n);
   secret[n] = 0;
   size_t p = strlen(principal);
   bound = malloc(p + n + 2);
   if (!bound || vault_server_kek(kek) || vault_store_content_index_key(kek, key, sizeof(key)))
      goto done;
   memcpy(bound, principal, p + 1);
   memcpy(bound + p + 1, value, n);
   unsigned int length = 0;
   if (!HMAC(EVP_sha256(), key, 64, (unsigned char *)bound, p + 1 + n, digest, &length) ||
       length != 32)
      goto done;
   snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], (digest[6] & 15) | 64,
            digest[7], (digest[8] & 63) | 128, digest[9], digest[10], digest[11], digest[12],
            digest[13], digest[14], digest[15]);
   vault_create_once_result_t result = vault_service_create_server_wrap_once(
       principal, "private-content", id, secret, generation, sizeof(generation));
   if (result != VAULT_CREATE_ONCE_CREATED && result != VAULT_CREATE_ONCE_REPLAY)
      goto done;
   snprintf(out, 50, "[[vault:v1:%s]]", id);
   rc = 0;
done:
   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(key, sizeof(key));
   OPENSSL_cleanse(digest, sizeof(digest));
   OPENSSL_cleanse(secret, n);
   free(secret);
   if (bound)
   {
      OPENSSL_cleanse(bound, p + n + 2);
      free(bound);
   }
   return rc;
}
int vault_content_protect_text(const char *text, char **safe)
{
   if (!safe)
      return -1;
   *safe = NULL;
   if (!text || strlen(text) > TEXT_MAX || !text_is_valid_utf8(text))
      return -1;
   pthread_once(&once, initialize);
   if (!ready)
      return -1;
   size_t n = strlen(text), at = 0, used = 0;
   if (uuid(text) || locator(text, n))
      return 0;
   char *out = malloc(TEXT_MAX + 1);
   if (!out)
      return -1;
   int changed = 0;
   while (at < n)
   {
      size_t first = n, end = n;
      for (size_t i = 0; i < sizeof(compiled) / sizeof(compiled[0]); ++i)
      {
         regmatch_t match;
         size_t offset = at;
         while (offset < n && !regexec(&compiled[i], text + offset, 1, &match, 0))
         {
            size_t a = offset + (size_t)match.rm_so, b = offset + (size_t)match.rm_eo;
            if (i == 0)
               while (b > a && strchr(".!?", text[b - 1]))
                  --b;
            int valid = b > a && !inside_locator(text, a) && !inside_uuid(text, a);
            if (i == 4 || i == 5)
            {
               unsigned char address[16];
               char candidate[64];
               if (b - a >= sizeof(candidate))
                  valid = 0;
               else
               {
                  memcpy(candidate, text + a, b - a);
                  candidate[b - a] = 0;
                  valid = valid &&
                          inet_pton(i == 4 ? AF_INET : AF_INET6, candidate, address) == 1 &&
                          (!a || (!isalnum((unsigned char)text[a - 1]) && text[a - 1] != '_')) &&
                          (b == n || (!isalnum((unsigned char)text[b]) && text[b] != '_'));
               }
            }
            if (i == 6)
            {
               size_t digits = 0;
               for (size_t k = a; k < b; ++k)
                  digits += isdigit((unsigned char)text[k]) != 0;
               /* E.164 phone numbers stop at 15 digits. Longer protocol IDs
                * must retain their bytes; grouped payment cards have their
                * own recognizer rather than borrowing the phone rule. */
               valid = valid && digits >= 9 && digits <= 15;
            }
            if (valid && (a < first || (a == first && b > end)))
            {
               first = a;
               end = b;
            }
            if (valid || b <= offset)
               break;
            offset = b;
         }
      }
      if (used + first - at > TEXT_MAX)
         goto fail;
      memcpy(out + used, text + at, first - at);
      used += first - at;
      if (first == n)
         break;
      at = first;
      while (at < end)
      {
         size_t count = end - at;
         if (count > VALUE_MAX)
            count = VALUE_MAX;
         while (count && ((unsigned char)text[at + count] & 0xc0) == 0x80)
            --count;
         char reference[50];
         if (!count || used + REF_BYTES > TEXT_MAX || vault_value(text + at, count, reference))
            goto fail;
         memcpy(out + used, reference, REF_BYTES);
         used += REF_BYTES;
         at += count;
         changed = 1;
      }
   }
   out[used] = 0;
   if (changed)
      *safe = out;
   else
      free(out);
   return 0;
fail:
   OPENSSL_cleanse(out, TEXT_MAX + 1);
   free(out);
   return -1;
}

static int sensitive_binding(const char *label)
{
   if (!label || !label[0] || strlen(label) > TEXT_MAX || vault_content_privacy_ready())
      return 0;
   size_t n = strlen(label);
   char *probe = malloc(n + 3);
   if (!probe)
      return -1;
   memcpy(probe, label, n);
   memcpy(probe + n, "=x", 3);
   regmatch_t match;
   int result = regexec(&compiled[1], probe, 1, &match, 0) == 0;
   free(probe);
   return result;
}
static int protect_value(const char *value, char **out)
{
   *out = NULL;
   size_t n = strlen(value), at = 0, used = 0;
   if (!n)
      return 0;
   if (n > TEXT_MAX)
      return -1;
   char *safe = malloc(TEXT_MAX + 1);
   if (!safe)
      return -1;
   while (at < n)
   {
      size_t count = n - at;
      if (count > VALUE_MAX)
         count = VALUE_MAX;
      while (count && ((unsigned char)value[at + count] & 0xc0) == 0x80)
         --count;
      char reference[50];
      if (!count || used + REF_BYTES > TEXT_MAX || vault_value(value + at, count, reference))
      {
         free(safe);
         return -1;
      }
      memcpy(safe + used, reference, REF_BYTES);
      used += REF_BYTES;
      at += count;
   }
   safe[used] = 0;
   *out = safe;
   return 0;
}

/* Rewrite selected JSON string tokens, retaining every other original byte,
 * including integer/version tokens above 2^53. This is not an authority parser. */
int vault_content_protect_json(const char *json, char **safe)
{
   if (!safe)
      return -1;
   *safe = NULL;
   if (!json || strlen(json) > 16u * TEXT_MAX || !text_is_valid_utf8(json))
      return -1;
   cJSON *valid = cJSON_ParseWithOpts(json, NULL, 1);
   if (!valid)
      return -1;
   const char *binding = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(valid, "key"));
   int private_binding = sensitive_binding(binding);
   if (private_binding < 0)
   {
      cJSON_Delete(valid);
      return -1;
   }
   cJSON_Delete(valid);
   static const char *fields[] = {"content",        "key",        "new_content", "input",
                                  "prompt",         "message",    "text",        "query",
                                  "queries",        "keywords",   "task",        "task_hint",
                                  "source_session", "session_id", "use_cases",   "entity",
                                  "instructions",   "system",     "description", NULL};
   size_t n = strlen(json), copied = 0, used = 0;
   char field[128] = "", *out = malloc(16u * TEXT_MAX + 1);
   if (!out)
      return -1;
   for (size_t i = 0; i < n; ++i)
   {
      if (json[i] != '"')
         continue;
      size_t end = i + 1;
      while (end < n && json[end] != '"')
      {
         if (json[end] == '\\')
         {
            if (end + 5 < n && json[end + 1] == 'u' && !memcmp(json + end + 2, "0000", 4))
               goto fail;
            ++end;
         }
         ++end;
      }
      if (end >= n)
         goto fail;
      cJSON *token = cJSON_ParseWithLength(json + i, end - i + 1);
      const char *value = cJSON_GetStringValue(token);
      if (!value)
      {
         cJSON_Delete(token);
         goto fail;
      }
      size_t next = end + 1;
      while (next < n && isspace((unsigned char)json[next]))
         ++next;
      int property = next < n && json[next] == ':';
      int selected = property;
      if (!property)
         for (int f = 0; fields[f]; ++f)
            selected |= !strcmp(field, fields[f]);
      char *replacement = NULL;
      int rc = 0;
      if (selected)
      {
         if (private_binding &&
             (!strcmp(field, "content") || !strcmp(field, "new_content") ||
              !strcmp(field, "key")) &&
             !property)
         {
            rc = protect_value(value, &replacement);
         }
         else
            rc = vault_content_protect_text(value, &replacement);
      }
      if (property)
         snprintf(field, sizeof(field), "%s", value);
      if (rc)
      {
         cJSON_Delete(token);
         free(replacement);
         goto fail;
      }
      if (replacement)
      {
         cJSON *escaped = cJSON_CreateString(replacement);
         char *encoded = escaped ? cJSON_PrintUnformatted(escaped) : NULL;
         cJSON_Delete(escaped);
         free(replacement);
         size_t count = encoded ? strlen(encoded) : 0;
         if (!encoded || used + i - copied + count > 16u * TEXT_MAX)
         {
            free(encoded);
            cJSON_Delete(token);
            goto fail;
         }
         memcpy(out + used, json + copied, i - copied);
         used += i - copied;
         memcpy(out + used, encoded, count);
         used += count;
         copied = end + 1;
         free(encoded);
      }
      cJSON_Delete(token);
      i = end;
   }
   if (!copied)
   {
      free(out);
      return 0;
   }
   if (used + n - copied > 16u * TEXT_MAX)
      goto fail;
   memcpy(out + used, json + copied, n - copied);
   used += n - copied;
   out[used] = 0;
   *safe = out;
   return 0;
fail:
   OPENSSL_cleanse(out, 16u * TEXT_MAX + 1);
   free(out);
   return -1;
}
