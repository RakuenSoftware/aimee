/* modules/aws/aws_sigv4.c: AWS Signature Version 4 signer. See aws_sigv4.h.
 *
 * Pure transform: inputs -> canonical request -> string-to-sign -> HMAC signing
 * key chain -> signature -> Authorization header. OpenSSL HMAC + SHA-256 only.
 * No time() call — the timestamp is passed in (deterministic + vector-testable). */

#include "aws_sigv4.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_hex[] = "0123456789abcdef";
static const char k_HEX[] = "0123456789ABCDEF"; /* SigV4 percent-escapes are uppercase */

static void to_hex(const unsigned char *d, size_t n, char *out)
{
   for (size_t i = 0; i < n; i++)
   {
      out[i * 2] = k_hex[d[i] >> 4];
      out[i * 2 + 1] = k_hex[d[i] & 0x0f];
   }
   out[n * 2] = '\0';
}

void aws_sha256_hex(const unsigned char *data, size_t len, char out[65])
{
   unsigned char d[SHA256_DIGEST_LENGTH];
   SHA256(data ? data : (const unsigned char *)"", data ? len : 0, d);
   to_hex(d, SHA256_DIGEST_LENGTH, out);
}

static int is_unreserved(unsigned char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_' || c == '.' || c == '~';
}

int aws_uri_encode(const char *in, int encode_slash, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   size_t o = 0;
   out[0] = '\0';
   if (!in)
      return 0;
   for (const unsigned char *p = (const unsigned char *)in; *p; p++)
   {
      unsigned char c = *p;
      if (is_unreserved(c) || (c == '/' && !encode_slash))
      {
         if (o + 1 >= cap)
            return -1;
         out[o++] = (char)c;
      }
      else
      {
         if (o + 3 >= cap)
            return -1;
         out[o++] = '%';
         out[o++] = k_HEX[c >> 4];
         out[o++] = k_HEX[c & 0x0f];
      }
   }
   out[o] = '\0';
   return 0;
}

/* --- canonical header normalization --- */

typedef struct
{
   char name[256];
   char value[AWS_SIGV4_TOKEN_MAX];
} norm_hdr_t;

static int header_name_char(unsigned char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          strchr("!#$%&'*+-.^_`|~", (int)c) != NULL;
}

static int payload_hash_valid(const char *hash)
{
   if (strcmp(hash, AWS_SIGV4_UNSIGNED_PAYLOAD) == 0)
      return 1;
   if (strlen(hash) != 64)
      return 0;
   for (size_t i = 0; i < 64; i++)
      if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f')))
         return 0;
   return 1;
}

/* lowercase `name`; trim + collapse inner whitespace of `value`. */
static int normalize_header(const char *name, const char *value, norm_hdr_t *out)
{
   size_t n = strlen(name);
   if (n == 0 || n >= sizeof(out->name))
      return -1;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char uc = (unsigned char)name[i];
      if (!header_name_char(uc))
         return -1;
      char c = (char)uc;
      if (c >= 'A' && c <= 'Z')
         c = (char)(c - 'A' + 'a');
      out->name[i] = c;
   }
   out->name[n] = '\0';

   /* Trim leading/trailing, collapse runs of space/tab to a single space. */
   const char *v = value ? value : "";
   while (*v == ' ' || *v == '\t')
      v++;
   size_t end = strlen(v);
   while (end > 0 && (v[end - 1] == ' ' || v[end - 1] == '\t'))
      end--;
   size_t o = 0;
   int in_ws = 0;
   for (size_t i = 0; i < end; i++)
   {
      char c = v[i];
      if (((unsigned char)c < 0x20 && c != ' ' && c != '\t') || (unsigned char)c == 0x7f)
         return -1;
      if (c == ' ' || c == '\t')
      {
         in_ws = 1;
         continue;
      }
      if (in_ws && o > 0)
      {
         if (o + 1 >= sizeof(out->value))
            return -1;
         out->value[o++] = ' ';
      }
      in_ws = 0;
      if (o + 1 >= sizeof(out->value))
         return -1;
      out->value[o++] = c;
   }
   out->value[o] = '\0';
   return 0;
}

static int hdr_cmp(const void *a, const void *b)
{
   return strcmp(((const norm_hdr_t *)a)->name, ((const norm_hdr_t *)b)->name);
}

/* --- canonical query normalization --- */

typedef struct
{
   char kv[1024]; /* "encodedkey=encodedvalue" */
   char key[512]; /* encoded key, for sorting */
} norm_qp_t;

static int qp_cmp(const void *a, const void *b)
{
   const norm_qp_t *x = (const norm_qp_t *)a;
   const norm_qp_t *y = (const norm_qp_t *)b;
   int r = strcmp(x->key, y->key);
   if (r != 0)
      return r;
   return strcmp(x->kv, y->kv); /* stable secondary sort by full pair */
}

/* Append s to out[cap] at *o (NUL-terminated). Returns 0 or -1 on overflow. */
static int app(char *out, size_t cap, size_t *o, const char *s)
{
   size_t l = strlen(s);
   if (*o + l + 1 > cap)
      return -1;
   memcpy(out + *o, s, l);
   *o += l;
   out[*o] = '\0';
   return 0;
}

int aws_sigv4_sign(const aws_sigv4_request_t *req, aws_sigv4_result_t *out)
{
   if (!req || !out)
      return -1;
   if (!req->method || !req->payload_hash || !req->amz_date || !req->date || !req->region ||
       !req->service || !req->access_key_id || !req->secret_access_key)
      return -1;
   if (req->access_key_id_len == 0 || req->access_key_id_len > 128 ||
       req->secret_access_key_len == 0 || req->secret_access_key_len > 255 ||
       memchr(req->access_key_id, '\0', req->access_key_id_len) ||
       memchr(req->secret_access_key, '\0', req->secret_access_key_len) ||
       (!!req->session_token != (req->session_token_len != 0)) ||
       req->session_token_len >= AWS_SIGV4_TOKEN_MAX ||
       (req->session_token && memchr(req->session_token, '\0', req->session_token_len)))
      return -1;
   if (!payload_hash_valid(req->payload_hash) || (req->n_headers && !req->headers) ||
       (req->n_query && !req->query))
      return -1;
   if (strlen(req->amz_date) >= sizeof(out->amz_date) ||
       req->session_token_len >= sizeof(out->security_token))
      return -1;
   if (req->n_headers > AWS_SIGV4_MAX_HEADERS || req->n_query > AWS_SIGV4_MAX_QUERY)
      return -1;

   memset(out, 0, sizeof(*out));
   snprintf(out->amz_date, sizeof(out->amz_date), "%s", req->amz_date);

   int has_token = req->session_token_len != 0;
   if (has_token)
   {
      out->has_security_token = 1;
      memcpy(out->security_token, req->session_token, req->session_token_len);
      out->security_token[req->session_token_len] = '\0';
   }

   /* 1. Canonical URI: percent-encode each path segment once (keep '/'). */
   char canon_uri[2048];
   const char *path = (req->raw_path && req->raw_path[0]) ? req->raw_path : "/";
   if (aws_uri_encode(path, 0, canon_uri, sizeof(canon_uri)) != 0)
      return -1;

   /* 2. Canonical query string. */
   norm_qp_t qps[AWS_SIGV4_MAX_QUERY];
   size_t nq = 0;
   for (size_t i = 0; i < req->n_query; i++)
   {
      char ek[512], ev[512];
      if (aws_uri_encode(req->query[i].name, 1, ek, sizeof(ek)) != 0)
         return -1;
      if (aws_uri_encode(req->query[i].value ? req->query[i].value : "", 1, ev, sizeof(ev)) != 0)
         return -1;
      snprintf(qps[nq].key, sizeof(qps[nq].key), "%s", ek);
      if ((size_t)snprintf(qps[nq].kv, sizeof(qps[nq].kv), "%s=%s", ek, ev) >= sizeof(qps[nq].kv))
         return -1;
      nq++;
   }
   qsort(qps, nq, sizeof(qps[0]), qp_cmp);
   char canon_query[4096];
   size_t cqo = 0;
   canon_query[0] = '\0';
   for (size_t i = 0; i < nq; i++)
   {
      if (i && app(canon_query, sizeof(canon_query), &cqo, "&") != 0)
         return -1;
      if (app(canon_query, sizeof(canon_query), &cqo, qps[i].kv) != 0)
         return -1;
   }

   /* 3. Canonical + signed headers (caller headers + x-amz-security-token). */
   norm_hdr_t hdrs[AWS_SIGV4_MAX_HEADERS + 1];
   size_t nh = 0;
   for (size_t i = 0; i < req->n_headers; i++)
   {
      if (!req->headers[i].name)
         return -1;
      if (normalize_header(req->headers[i].name, req->headers[i].value, &hdrs[nh]) != 0)
         return -1;
      nh++;
   }
   if (has_token)
   {
      if (normalize_header("x-amz-security-token", req->session_token, &hdrs[nh]) != 0)
         return -1;
      nh++;
   }
   if (nh == 0)
      return -1;
   qsort(hdrs, nh, sizeof(hdrs[0]), hdr_cmp);

   char canon_hdrs[AWS_SIGV4_CANONICAL_MAX];
   size_t cho = 0;
   canon_hdrs[0] = '\0';
   for (size_t i = 0; i < nh; i++)
   {
      /* Reject duplicate header names (ambiguous canonicalization). */
      if (i > 0 && strcmp(hdrs[i].name, hdrs[i - 1].name) == 0)
         return -1;
      if (app(canon_hdrs, sizeof(canon_hdrs), &cho, hdrs[i].name) != 0 ||
          app(canon_hdrs, sizeof(canon_hdrs), &cho, ":") != 0 ||
          app(canon_hdrs, sizeof(canon_hdrs), &cho, hdrs[i].value) != 0 ||
          app(canon_hdrs, sizeof(canon_hdrs), &cho, "\n") != 0)
         return -1;
   }
   size_t sho = 0;
   out->signed_headers[0] = '\0';
   for (size_t i = 0; i < nh; i++)
   {
      if (i && app(out->signed_headers, sizeof(out->signed_headers), &sho, ";") != 0)
         return -1;
      if (app(out->signed_headers, sizeof(out->signed_headers), &sho, hdrs[i].name) != 0)
         return -1;
   }

   /* 4. Assemble the canonical request + hash it. */
   size_t co = 0;
   out->canonical_request[0] = '\0';
   if (app(out->canonical_request, sizeof(out->canonical_request), &co, req->method) != 0 ||
       app(out->canonical_request, sizeof(out->canonical_request), &co, "\n") != 0 ||
       app(out->canonical_request, sizeof(out->canonical_request), &co, canon_uri) != 0 ||
       app(out->canonical_request, sizeof(out->canonical_request), &co, "\n") != 0 ||
       app(out->canonical_request, sizeof(out->canonical_request), &co, canon_query) != 0 ||
       app(out->canonical_request, sizeof(out->canonical_request), &co, "\n") != 0 ||
       app(out->canonical_request, sizeof(out->canonical_request), &co, canon_hdrs) != 0 ||
       app(out->canonical_request, sizeof(out->canonical_request), &co, "\n") != 0 ||
       app(out->canonical_request, sizeof(out->canonical_request), &co, out->signed_headers) != 0 ||
       app(out->canonical_request, sizeof(out->canonical_request), &co, "\n") != 0 ||
       app(out->canonical_request, sizeof(out->canonical_request), &co, req->payload_hash) != 0)
      return -1;
   aws_sha256_hex((const unsigned char *)out->canonical_request, co, out->canonical_request_hash);

   /* 5. String to sign. */
   char scope[256];
   if ((size_t)snprintf(scope, sizeof(scope), "%s/%s/%s/aws4_request", req->date, req->region,
                        req->service) >= sizeof(scope))
      return -1;
   if ((size_t)snprintf(out->string_to_sign, sizeof(out->string_to_sign),
                        "AWS4-HMAC-SHA256\n%s\n%s\n%s", req->amz_date, scope,
                        out->canonical_request_hash) >= sizeof(out->string_to_sign))
      return -1;

   /* 6. Derive the signing key (HMAC chain) + sign. */
   unsigned char k_secret[260];
   unsigned char k_date[32] = {0}, k_region[32] = {0}, k_service[32] = {0}, k_signing[32] = {0},
                 sig[32] = {0};
   int rc = -1;
   if (4 + req->secret_access_key_len > sizeof(k_secret))
      goto cleanup;
   memcpy(k_secret, "AWS4", 4);
   memcpy(k_secret + 4, req->secret_access_key, req->secret_access_key_len);
   size_t ksl = 4 + req->secret_access_key_len;
   unsigned int len = 0;
   if (!HMAC(EVP_sha256(), k_secret, (int)ksl, (const unsigned char *)req->date, strlen(req->date),
             k_date, &len))
      goto cleanup;
   if (!HMAC(EVP_sha256(), k_date, 32, (const unsigned char *)req->region, strlen(req->region),
             k_region, &len))
      goto cleanup;
   if (!HMAC(EVP_sha256(), k_region, 32, (const unsigned char *)req->service, strlen(req->service),
             k_service, &len))
      goto cleanup;
   if (!HMAC(EVP_sha256(), k_service, 32, (const unsigned char *)"aws4_request", 12, k_signing,
             &len))
      goto cleanup;
   if (!HMAC(EVP_sha256(), k_signing, 32, (const unsigned char *)out->string_to_sign,
             strlen(out->string_to_sign), sig, &len))
      goto cleanup;
   to_hex(sig, 32, out->signature);

   /* 7. Authorization header. */
   if ((size_t)snprintf(out->authorization, sizeof(out->authorization),
                        "AWS4-HMAC-SHA256 Credential=%.*s/%s, SignedHeaders=%s, Signature=%s",
                        (int)req->access_key_id_len, req->access_key_id, scope, out->signed_headers,
                        out->signature) >= sizeof(out->authorization))
      goto cleanup;
   rc = 0;
cleanup:
   OPENSSL_cleanse(k_secret, sizeof(k_secret));
   OPENSSL_cleanse(k_date, sizeof(k_date));
   OPENSSL_cleanse(k_region, sizeof(k_region));
   OPENSSL_cleanse(k_service, sizeof(k_service));
   OPENSSL_cleanse(k_signing, sizeof(k_signing));
   OPENSSL_cleanse(sig, sizeof(sig));
   return rc;
}
