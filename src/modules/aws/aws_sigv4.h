/* modules/aws/aws_sigv4.h: AWS Signature Version 4 signer (P6a).
 *
 * A PURE, OFFLINE implementation of the AWS SigV4 request-signing algorithm:
 * canonical request -> string-to-sign -> derived signing key (HMAC chain) ->
 * signature -> Authorization header. NO network, NO clock, NO global state —
 * the caller passes the fixed ISO8601 date/time in, so signing is deterministic
 * and unit-testable against AWS's published `aws-sig-v4-test-suite` vectors.
 *
 * Input contract (non-S3 semantics): the canonical URI is the RAW path; each
 * path SEGMENT is RFC3986 percent-encoded exactly ONCE (Bedrock/STS are non-S3,
 * so no S3-style double-encode). Canonical headers are lowercase-name,
 * value-trimmed + inner-whitespace-collapsed, sorted by name. When a session
 * token is supplied it is added to the SIGNED header set as x-amz-security-token.
 *
 * Depends only on libc + OpenSSL (HMAC + SHA-256). */
#ifndef DEC_AWS_SIGV4_H
#define DEC_AWS_SIGV4_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The canonical hex SHA-256 of a zero-length body (the "empty-body digest"). */
#define AWS_SIGV4_EMPTY_BODY_SHA256                                                                \
   "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

/* The literal sentinel for an unsigned payload (streaming / large bodies). */
#define AWS_SIGV4_UNSIGNED_PAYLOAD "UNSIGNED-PAYLOAD"

   /* Bounds. Bedrock/STS requests are small: a handful of headers, a short path,
    * a short query. Canonical request is bounded generously; overflow -> error. */
#define AWS_SIGV4_MAX_HEADERS     32
#define AWS_SIGV4_MAX_QUERY       32
#define AWS_SIGV4_CANONICAL_MAX   8192
#define AWS_SIGV4_STS_MAX         512
#define AWS_SIGV4_SIGNED_HDRS_MAX 1024
#define AWS_SIGV4_AUTH_MAX        2048
#define AWS_SIGV4_TOKEN_MAX       8192

   typedef struct
   {
      const char *name;
      const char *value;
   } aws_sigv4_kv_t;

   /* A request to sign. All pointers are borrowed (not retained). `raw_path` is the
    * un-encoded path (e.g. "/model/anthropic.claude/invoke"); it is percent-encoded
    * by the signer. `payload_hash` is a 64-char lowercase hex SHA-256 of the body,
    * the empty-body digest, or the literal AWS_SIGV4_UNSIGNED_PAYLOAD. `amz_date` is
    * the ISO8601 basic timestamp "YYYYMMDDTHHMMSSZ"; `date` is "YYYYMMDD" (the
    * credential-scope date — MUST match amz_date's date). `session_token` is NULL/""
    * when absent; when present it is signed as x-amz-security-token and echoed. */
   typedef struct
   {
      const char *method;   /* "GET" | "POST" | ... (used verbatim) */
      const char *raw_path; /* RAW, un-encoded path; "" -> "/" */
      const aws_sigv4_kv_t *query;
      size_t n_query;
      const aws_sigv4_kv_t *headers; /* caller supplies host + x-amz-date + any others */
      size_t n_headers;
      const char *payload_hash;
      const char *amz_date;
      const char *date;
      const char *region;
      const char *service;
      const char *access_key_id;
      const char *secret_access_key;
      const char *session_token; /* NULL or "" if none */
   } aws_sigv4_request_t;

   /* The signing result. All buffers are NUL-terminated. `authorization` is the full
    * value for the HTTP `Authorization` header. When `has_security_token` is set the
    * caller MUST also emit `X-Amz-Security-Token: <security_token>`. */
   typedef struct
   {
      char canonical_request[AWS_SIGV4_CANONICAL_MAX];
      char string_to_sign[AWS_SIGV4_STS_MAX];
      char canonical_request_hash[65];
      char signature[65];
      char signed_headers[AWS_SIGV4_SIGNED_HDRS_MAX];
      char authorization[AWS_SIGV4_AUTH_MAX];
      char amz_date[32];
      char security_token[AWS_SIGV4_TOKEN_MAX];
      int has_security_token;
   } aws_sigv4_result_t;

   /* Compute the lowercase hex SHA-256 of `len` bytes at `data` into out[65]. */
   void aws_sha256_hex(const unsigned char *data, size_t len, char out[65]);

   /* RFC3986 percent-encode `in` into out[cap]. When `encode_slash` is 0, '/' is
    * preserved (path mode: encode each segment, keep separators); when non-zero,
    * '/' is encoded too (query key/value mode). Unreserved chars A-Za-z0-9-_.~ pass
    * through. Returns 0 on success, -1 on overflow. */
   int aws_uri_encode(const char *in, int encode_slash, char *out, size_t cap);

   /* Sign `req` into `out`. Returns 0 on success, -1 on any error (NULL field,
    * buffer overflow, too many headers/query params, malformed payload hash). On
    * error `out` contents are unspecified and MUST NOT be used. */
   int aws_sigv4_sign(const aws_sigv4_request_t *req, aws_sigv4_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_AWS_SIGV4_H */
