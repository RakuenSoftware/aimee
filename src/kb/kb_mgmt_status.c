#include "kb_mgmt_status.h"
#include "cJSON.h"

#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATUS_DOMAIN "aimee.management.status.v1"

static int bounded(const char *s, size_t min, size_t max)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max + 1);
   return n >= min && n <= max;
}

static int printable(const char *s, size_t min, size_t max)
{
   if (!bounded(s, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      if (*p < 0x20 || *p == 0x7f)
         return 0;
   return 1;
}

static int hex_lower(const char *s, size_t min, size_t max)
{
   if (!bounded(s, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
         return 0;
   return 1;
}

static int ascii_token(const char *s, size_t min, size_t max)
{
   if (!bounded(s, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '.' || *p == '_' || *p == '-'))
         return 0;
   return 1;
}

static int status_shape(const kb_mgmt_status_t *s)
{
   return s && s->version == 1 && ascii_token(s->key_id, 1, 64) &&
          printable(s->caller_issuer, 1, 600) && hex_lower(s->caller_serial_norm, 1, 128) &&
          hex_lower(s->caller_fingerprint, 64, 64) && ascii_token(s->target_server_id, 1, 127) &&
          hex_lower(s->target_mgmt_fingerprint, 64, 64) &&
          strcmp(s->purpose, "management.health.v1") == 0 && s->issued_at > 0 &&
          s->expires_at > s->issued_at && s->expires_at - s->issued_at <= 10 &&
          s->revocation_generation >= 1;
}

static int put_u32(unsigned char *out, size_t cap, size_t *off, uint32_t v)
{
   if (*off > cap || cap - *off < 4)
      return -1;
   out[(*off)++] = (unsigned char)(v >> 24);
   out[(*off)++] = (unsigned char)(v >> 16);
   out[(*off)++] = (unsigned char)(v >> 8);
   out[(*off)++] = (unsigned char)v;
   return 0;
}

static int put_u64(unsigned char *out, size_t cap, size_t *off, uint64_t v)
{
   if (*off > cap || cap - *off < 8)
      return -1;
   for (int i = 7; i >= 0; i--)
      out[(*off)++] = (unsigned char)(v >> (unsigned)(i * 8));
   return 0;
}

static int put_bytes(unsigned char *out, size_t cap, size_t *off, const void *p, size_t n)
{
   if (n > UINT32_MAX || put_u32(out, cap, off, (uint32_t)n) != 0 || *off > cap || cap - *off < n)
      return -1;
   memcpy(out + *off, p, n);
   *off += n;
   return 0;
}

int kb_mgmt_status_transcript(const kb_mgmt_status_t *s, unsigned char *out, size_t cap,
                              size_t *out_len)
{
   if (!status_shape(s) || !out || !out_len)
      return -1;
   size_t o = 0;
   if (put_bytes(out, cap, &o, STATUS_DOMAIN, sizeof(STATUS_DOMAIN) - 1) ||
       put_u32(out, cap, &o, s->version) || put_bytes(out, cap, &o, s->key_id, strlen(s->key_id)) ||
       put_bytes(out, cap, &o, s->nonce, sizeof(s->nonce)) ||
       put_bytes(out, cap, &o, s->caller_issuer, strlen(s->caller_issuer)) ||
       put_bytes(out, cap, &o, s->caller_serial_norm, strlen(s->caller_serial_norm)) ||
       put_bytes(out, cap, &o, s->caller_fingerprint, strlen(s->caller_fingerprint)) ||
       put_bytes(out, cap, &o, s->target_server_id, strlen(s->target_server_id)) ||
       put_bytes(out, cap, &o, s->target_mgmt_fingerprint, strlen(s->target_mgmt_fingerprint)) ||
       put_bytes(out, cap, &o, s->purpose, strlen(s->purpose)) ||
       put_u64(out, cap, &o, s->issued_at) || put_u64(out, cap, &o, s->expires_at) ||
       put_u64(out, cap, &o, s->revocation_generation))
      return -1;
   *out_len = o;
   return 0;
}

int kb_mgmt_status_sign(kb_mgmt_status_t *s, const unsigned char key[KB_MGMT_STATUS_KEY_LEN])
{
   unsigned char msg[1536];
   size_t msg_len = 0, sig_len = sizeof(s->signature);
   if (!s || !key || kb_mgmt_status_transcript(s, msg, sizeof(msg), &msg_len) != 0)
      return -1;
   EVP_PKEY *pkey =
       EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, key, KB_MGMT_STATUS_KEY_LEN);
   EVP_MD_CTX *ctx = pkey ? EVP_MD_CTX_new() : NULL;
   int ok = ctx && EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) == 1 &&
            EVP_DigestSign(ctx, s->signature, &sig_len, msg, msg_len) == 1 &&
            sig_len == sizeof(s->signature);
   EVP_MD_CTX_free(ctx);
   EVP_PKEY_free(pkey);
   return ok ? 0 : -1;
}

int kb_mgmt_status_verify_signature(const kb_mgmt_status_t *s,
                                    const unsigned char key[KB_MGMT_STATUS_KEY_LEN])
{
   unsigned char msg[1536];
   size_t msg_len = 0;
   if (!s || !key || kb_mgmt_status_transcript(s, msg, sizeof(msg), &msg_len) != 0)
      return -1;
   EVP_PKEY *pkey =
       EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, key, KB_MGMT_STATUS_KEY_LEN);
   EVP_MD_CTX *ctx = pkey ? EVP_MD_CTX_new() : NULL;
   int ok = ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) == 1 &&
            EVP_DigestVerify(ctx, s->signature, sizeof(s->signature), msg, msg_len) == 1;
   EVP_MD_CTX_free(ctx);
   EVP_PKEY_free(pkey);
   return ok ? 0 : -1;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64_encode(const unsigned char *in, size_t n, char *out, size_t cap)
{
   size_t need = (n / 3) * 4 + (n % 3 ? n % 3 + 1 : 0);
   if (!in || !out || cap <= need)
      return -1;
   size_t i = 0, o = 0;
   while (i + 3 <= n)
   {
      uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
      out[o++] = B64[(v >> 18) & 63];
      out[o++] = B64[(v >> 12) & 63];
      out[o++] = B64[(v >> 6) & 63];
      out[o++] = B64[v & 63];
      i += 3;
   }
   if (n - i == 1)
   {
      uint32_t v = (uint32_t)in[i] << 16;
      out[o++] = B64[(v >> 18) & 63];
      out[o++] = B64[(v >> 12) & 63];
   }
   else if (n - i == 2)
   {
      uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
      out[o++] = B64[(v >> 18) & 63];
      out[o++] = B64[(v >> 12) & 63];
      out[o++] = B64[(v >> 6) & 63];
   }
   out[o] = '\0';
   return 0;
}

static int b64_value(unsigned char c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   return c == '-' ? 62 : (c == '_' ? 63 : -1);
}

static int b64_decode_exact(const char *in, unsigned char *out, size_t expected)
{
   if (!in || !out)
      return -1;
   size_t n = strlen(in);
   if (n % 4 == 1)
      return -1;
   size_t o = 0;
   uint32_t acc = 0;
   int bits = 0;
   for (size_t i = 0; i < n; i++)
   {
      int v = b64_value((unsigned char)in[i]);
      if (v < 0)
         return -1;
      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (o >= expected)
            return -1;
         out[o++] = (unsigned char)(acc >> (unsigned)bits);
         acc &= bits ? ((1u << (unsigned)bits) - 1u) : 0u;
      }
   }
   if (o != expected || (bits && acc != 0))
      return -1;
   char canonical[128];
   return b64_encode(out, expected, canonical, sizeof(canonical)) == 0 && strcmp(canonical, in) == 0
              ? 0
              : -1;
}

static int u64_decimal(const char *s, uint64_t *out)
{
   if (!s || !s[0] || !out || (s[0] == '0' && s[1]))
      return -1;
   uint64_t v = 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
   {
      if (*p < '0' || *p > '9' || v > (UINT64_MAX - (uint64_t)(*p - '0')) / 10)
         return -1;
      v = v * 10 + (uint64_t)(*p - '0');
   }
   *out = v;
   return 0;
}

static int object_exact(const cJSON *j)
{
   static const char *names[] = {"version",       "key_id",    "nonce",      "caller_issuer",
                                 "caller_serial", "caller_fp", "target",     "target_mgmt_fp",
                                 "purpose",       "issued_at", "expires_at", "generation",
                                 "signature"};
   int seen[13] = {0}, count = 0;
   for (const cJSON *p = j ? j->child : NULL; p; p = p->next)
   {
      int found = -1;
      for (int i = 0; i < 13; i++)
         if (p->string && strcmp(p->string, names[i]) == 0)
            found = i;
      if (found < 0 || seen[found]++)
         return 0;
      count++;
   }
   return count == 13;
}

static const char *json_string(const cJSON *j, const char *name)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
   return cJSON_IsString(v) ? cJSON_GetStringValue(v) : NULL;
}

static int copy_string(char *dst, size_t cap, const char *src)
{
   if (!dst || !cap || !src)
      return -1;
   size_t n = strlen(src);
   if (n >= cap)
      return -1;
   memcpy(dst, src, n + 1);
   return 0;
}

int kb_mgmt_status_nonce_from_json(const char *raw, unsigned char nonce[KB_MGMT_STATUS_NONCE_LEN])
{
   if (!raw || !nonce)
      return -1;
   size_t raw_len = strnlen(raw, KB_MGMT_STATUS_JSON_MAX + 1);
   if (raw_len < 2 || raw_len > KB_MGMT_STATUS_JSON_MAX || raw[0] != '{' || raw[raw_len - 1] != '}')
      return -1;
   const char *end = NULL, *encoded = NULL;
   cJSON *j = cJSON_ParseWithOpts(raw, &end, 1);
   int count = 0;
   if (cJSON_IsObject(j) && end && !*end)
      for (const cJSON *p = j->child; p; p = p->next)
         if (p->string && strcmp(p->string, "nonce") == 0)
         {
            count++;
            encoded = cJSON_IsString(p) ? cJSON_GetStringValue(p) : NULL;
         }
   int rc = count == 1 && encoded && b64_decode_exact(encoded, nonce, KB_MGMT_STATUS_NONCE_LEN) == 0
                ? 0
                : -1;
   cJSON_Delete(j);
   if (rc != 0)
      memset(nonce, 0, KB_MGMT_STATUS_NONCE_LEN);
   return rc;
}

int kb_mgmt_status_to_json(const kb_mgmt_status_t *s, char *out, size_t cap)
{
   if (!status_shape(s) || !out || !cap)
      return -1;
   char nonce[48], sig[96], version[16], issued[32], expires[32], generation[32];
   if (b64_encode(s->nonce, sizeof(s->nonce), nonce, sizeof(nonce)) ||
       b64_encode(s->signature, sizeof(s->signature), sig, sizeof(sig)))
      return -1;
   snprintf(version, sizeof(version), "%u", s->version);
   snprintf(issued, sizeof(issued), "%llu", (unsigned long long)s->issued_at);
   snprintf(expires, sizeof(expires), "%llu", (unsigned long long)s->expires_at);
   snprintf(generation, sizeof(generation), "%llu", (unsigned long long)s->revocation_generation);
   cJSON *j = cJSON_CreateObject();
   if (!j)
      return -1;
   cJSON_AddStringToObject(j, "version", version);
   cJSON_AddStringToObject(j, "key_id", s->key_id);
   cJSON_AddStringToObject(j, "nonce", nonce);
   cJSON_AddStringToObject(j, "caller_issuer", s->caller_issuer);
   cJSON_AddStringToObject(j, "caller_serial", s->caller_serial_norm);
   cJSON_AddStringToObject(j, "caller_fp", s->caller_fingerprint);
   cJSON_AddStringToObject(j, "target", s->target_server_id);
   cJSON_AddStringToObject(j, "target_mgmt_fp", s->target_mgmt_fingerprint);
   cJSON_AddStringToObject(j, "purpose", s->purpose);
   cJSON_AddStringToObject(j, "issued_at", issued);
   cJSON_AddStringToObject(j, "expires_at", expires);
   cJSON_AddStringToObject(j, "generation", generation);
   cJSON_AddStringToObject(j, "signature", sig);
   char *raw = cJSON_PrintUnformatted(j);
   cJSON_Delete(j);
   int rc = raw && strlen(raw) < cap ? (snprintf(out, cap, "%s", raw), 0) : -1;
   free(raw);
   return rc;
}

int kb_mgmt_status_from_json(const char *raw, kb_mgmt_status_t *s)
{
   if (!raw || !s)
      return -1;
   size_t raw_len = strnlen(raw, KB_MGMT_STATUS_JSON_MAX + 1);
   if (raw_len < 2 || raw_len > KB_MGMT_STATUS_JSON_MAX || raw[0] != '{' || raw[raw_len - 1] != '}')
      return -1;
   const char *end = NULL;
   cJSON *j = cJSON_ParseWithOpts(raw, &end, 1);
   if (!cJSON_IsObject(j) || !end || *end || !object_exact(j))
   {
      cJSON_Delete(j);
      return -1;
   }
   const char *version = json_string(j, "version"), *key_id = json_string(j, "key_id"),
              *nonce = json_string(j, "nonce"), *issuer = json_string(j, "caller_issuer"),
              *serial = json_string(j, "caller_serial"), *fp = json_string(j, "caller_fp"),
              *target = json_string(j, "target"), *target_fp = json_string(j, "target_mgmt_fp"),
              *purpose = json_string(j, "purpose"), *issued = json_string(j, "issued_at"),
              *expires = json_string(j, "expires_at"), *generation = json_string(j, "generation"),
              *sig = json_string(j, "signature");
   uint64_t ver = 0;
   memset(s, 0, sizeof(*s));
   int ok = version && key_id && nonce && issuer && serial && fp && target && target_fp &&
            purpose && issued && expires && generation && sig && u64_decimal(version, &ver) == 0 &&
            ver == 1 && copy_string(s->key_id, sizeof(s->key_id), key_id) == 0 &&
            b64_decode_exact(nonce, s->nonce, sizeof(s->nonce)) == 0 &&
            copy_string(s->caller_issuer, sizeof(s->caller_issuer), issuer) == 0 &&
            copy_string(s->caller_serial_norm, sizeof(s->caller_serial_norm), serial) == 0 &&
            copy_string(s->caller_fingerprint, sizeof(s->caller_fingerprint), fp) == 0 &&
            copy_string(s->target_server_id, sizeof(s->target_server_id), target) == 0 &&
            copy_string(s->target_mgmt_fingerprint, sizeof(s->target_mgmt_fingerprint),
                        target_fp) == 0 &&
            copy_string(s->purpose, sizeof(s->purpose), purpose) == 0 &&
            u64_decimal(issued, &s->issued_at) == 0 && u64_decimal(expires, &s->expires_at) == 0 &&
            u64_decimal(generation, &s->revocation_generation) == 0 &&
            b64_decode_exact(sig, s->signature, sizeof(s->signature)) == 0;
   s->version = (uint32_t)ver;
   cJSON_Delete(j);
   if (!ok || !status_shape(s))
   {
      memset(s, 0, sizeof(*s));
      return -1;
   }
   return 0;
}

int kb_mgmt_status_validate(const kb_mgmt_status_t *s, uint64_t now, uint64_t high_water)
{
   if (!status_shape(s) || (s->issued_at > now && s->issued_at - now > 2) ||
       (now > s->expires_at && now - s->expires_at > 2) || s->revocation_generation < high_water)
      return -1;
   return 0;
}

#define CHECKPOINT_DOMAIN "management.action.checkpoint.v1"

static int checkpoint_shape(const kb_mgmt_checkpoint_t *c)
{
   return c && c->version == 1 && hex_lower(c->request_sha256, 64, 64) &&
          (c->revoked == 0 || c->revoked == 1) && c->generation >= 1 && c->issued_at > 0 &&
          c->issued_at <= UINT64_MAX - 5 && c->expires_at == c->issued_at + 5 &&
          ascii_token(c->key_id, 1, 64);
}

int kb_mgmt_checkpoint_transcript(const kb_mgmt_checkpoint_t *c, unsigned char *out, size_t cap,
                                  size_t *out_len)
{
   if (!checkpoint_shape(c) || !out || !out_len)
      return -1;
   size_t o = 0;
   if (put_bytes(out, cap, &o, CHECKPOINT_DOMAIN, sizeof(CHECKPOINT_DOMAIN) - 1) ||
       put_u32(out, cap, &o, c->version) ||
       put_bytes(out, cap, &o, c->request_sha256, strlen(c->request_sha256)) ||
       put_u32(out, cap, &o, (uint32_t)c->revoked) || put_u64(out, cap, &o, c->generation) ||
       put_u64(out, cap, &o, c->issued_at) || put_u64(out, cap, &o, c->expires_at) ||
       put_bytes(out, cap, &o, c->key_id, strlen(c->key_id)))
      return -1;
   *out_len = o;
   return 0;
}

static int checkpoint_crypt(kb_mgmt_checkpoint_t *c, const unsigned char key[32], int sign)
{
   unsigned char msg[512];
   size_t msg_len = 0, sig_len = sizeof(c->signature);
   if (!c || !key || kb_mgmt_checkpoint_transcript(c, msg, sizeof(msg), &msg_len))
      return -1;
   EVP_PKEY *pkey = sign ? EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, key, 32)
                         : EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, key, 32);
   EVP_MD_CTX *ctx = pkey ? EVP_MD_CTX_new() : NULL;
   int ok = sign ? (ctx && EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) == 1 &&
                    EVP_DigestSign(ctx, c->signature, &sig_len, msg, msg_len) == 1 && sig_len == 64)
                 : (ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) == 1 &&
                    EVP_DigestVerify(ctx, c->signature, 64, msg, msg_len) == 1);
   EVP_MD_CTX_free(ctx);
   EVP_PKEY_free(pkey);
   return ok ? 0 : -1;
}

int kb_mgmt_checkpoint_sign(kb_mgmt_checkpoint_t *c, const unsigned char key[32])
{
   return checkpoint_crypt(c, key, 1);
}

int kb_mgmt_checkpoint_verify_signature(const kb_mgmt_checkpoint_t *c, const unsigned char key[32])
{
   return checkpoint_crypt((kb_mgmt_checkpoint_t *)c, key, 0);
}

int kb_mgmt_checkpoint_to_json(const kb_mgmt_checkpoint_t *c, char *out, size_t cap)
{
   char sig[96];
   if (!checkpoint_shape(c) || !out || !cap ||
       b64_encode(c->signature, sizeof(c->signature), sig, sizeof(sig)))
      return -1;
   int n = snprintf(out, cap,
                    "{\"version\":\"1\",\"domain\":\"%s\",\"request_sha256\":\"%s\","
                    "\"revoked\":%s,\"generation\":\"%llu\",\"issued_at\":\"%llu\","
                    "\"expires_at\":\"%llu\",\"key_id\":\"%s\",\"signature\":\"%s\"}",
                    CHECKPOINT_DOMAIN, c->request_sha256, c->revoked ? "true" : "false",
                    (unsigned long long)c->generation, (unsigned long long)c->issued_at,
                    (unsigned long long)c->expires_at, c->key_id, sig);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

int kb_mgmt_checkpoint_from_json(const char *raw, kb_mgmt_checkpoint_t *c)
{
   static const char *names[] = {"version",    "domain",     "request_sha256",
                                 "revoked",    "generation", "issued_at",
                                 "expires_at", "key_id",     "signature"};
   if (!raw || !c || strnlen(raw, KB_MGMT_CHECKPOINT_JSON_MAX + 1) > KB_MGMT_CHECKPOINT_JSON_MAX)
      return -1;
   const char *end = NULL;
   cJSON *j = cJSON_ParseWithOpts(raw, &end, 1);
   const cJSON *p = cJSON_IsObject(j) ? j->child : NULL;
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
   {
      if (!p || !p->string || strcmp(p->string, names[i]))
         goto bad;
      p = p->next;
   }
   if (p || !end || *end)
      goto bad;
   const char *version = json_string(j, names[0]), *domain = json_string(j, names[1]);
   const char *digest = json_string(j, names[2]), *generation = json_string(j, names[4]);
   const char *issued = json_string(j, names[5]), *expires = json_string(j, names[6]);
   const char *key_id = json_string(j, names[7]), *sig = json_string(j, names[8]);
   const cJSON *revoked = cJSON_GetObjectItemCaseSensitive(j, names[3]);
   uint64_t v = 0;
   memset(c, 0, sizeof(*c));
   if (!version || !domain || strcmp(domain, CHECKPOINT_DOMAIN) || !digest || !generation ||
       !issued || !expires || !key_id || !sig ||
       (!cJSON_IsTrue(revoked) && !cJSON_IsFalse(revoked)) || u64_decimal(version, &v) || v != 1 ||
       copy_string(c->request_sha256, sizeof(c->request_sha256), digest) ||
       u64_decimal(generation, &c->generation) || u64_decimal(issued, &c->issued_at) ||
       u64_decimal(expires, &c->expires_at) || copy_string(c->key_id, sizeof(c->key_id), key_id) ||
       b64_decode_exact(sig, c->signature, sizeof(c->signature)))
      goto bad;
   c->version = 1;
   c->revoked = cJSON_IsTrue(revoked) ? 1 : 0;
   cJSON_Delete(j);
   char canonical[KB_MGMT_CHECKPOINT_JSON_MAX + 1];
   if (!checkpoint_shape(c) || kb_mgmt_checkpoint_to_json(c, canonical, sizeof(canonical)) ||
       strcmp(canonical, raw))
   {
      memset(c, 0, sizeof(*c));
      return -1;
   }
   return 0;
bad:
   cJSON_Delete(j);
   memset(c, 0, sizeof(*c));
   return -1;
}

int kb_mgmt_checkpoint_validate(const kb_mgmt_checkpoint_t *c, uint64_t now, uint64_t high_water)
{
   return checkpoint_shape(c) && c->issued_at <= now && now < c->expires_at &&
                  c->generation >= high_water
              ? 0
              : -1;
}
