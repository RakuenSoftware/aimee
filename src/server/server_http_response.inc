/* server_http_response.inc — buffered HTTP response writers for server_http.c.
 *
 * Textually included by server_http.c (not a separate TU): these helpers share
 * its file-static state (g_*, write_all_fd) and are split out only to keep
 * server_http.c under the per-file line cap. Include this before the first use
 * of send_response / send_rate_limited. */

/* Build the optional `X-Request-ID: <id>\r\n` header line into dst (or ""). */
static void request_id_header(char *dst, size_t n, const char *request_id)
{
   if (request_id && request_id[0])
      snprintf(dst, n, "X-Request-ID: %s\r\n", request_id);
   else if (n)
      dst[0] = '\0';
}

/* Auditable-correctness P1: build the optional `X-Aimee-Retrieval-Event:
 * <turn_id>\r\n` header line (or "") from the ingress thread-local. Non-empty
 * only when a turn id was minted for this request (OpenAI-family ingress with
 * kb_evidence_emit_enabled on), so it matches the id keyed onto the emitted
 * retrieval_event; clients use it to query /v1/audit/trace. */
static void retrieval_event_header(char *dst, size_t n)
{
   const char *tid = ingress_preinject_turn_id();
   if (tid && tid[0])
      snprintf(dst, n, "X-Aimee-Retrieval-Event: %s\r\n", tid);
   else if (n)
      dst[0] = '\0';
}

/* Build the optional `Retry-After: <seconds>\r\n` header line (or "") from the
 * Anthropic ingress thread-local — non-empty only when the parity passthrough
 * relayed an upstream 429/529 that carried a Retry-After, so the client's SDK
 * backs off exactly as it would against api.anthropic.com. */
static void retry_after_header(char *dst, size_t n)
{
   int ra = anthropic_http_response_retry_after();
   if (ra > 0)
      snprintf(dst, n, "Retry-After: %d\r\n", ra);
   else if (n)
      dst[0] = '\0';
}

static void send_response(int fd, int status, const char *body, const char *request_id)
{
   const char *reason = status == 200   ? "OK"
                        : status == 400 ? "Bad Request"
                        : status == 401 ? "Unauthorized"
                        : status == 403 ? "Forbidden"
                        : status == 404 ? "Not Found"
                        : status == 429 ? "Too Many Requests"
                        : status == 500 ? "Internal Server Error"
                        : status == 503 ? "Service Unavailable"
                                        : "OK";
   char rid[96];
   request_id_header(rid, sizeof(rid), request_id);
   char reh[80];
   retrieval_event_header(reh, sizeof(reh));
   char rah[48];
   retry_after_header(rah, sizeof(rah));
   char head[480];
   int blen = body ? (int)strlen(body) : 0;
   int hlen = snprintf(head, sizeof(head),
                       "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                       "Content-Length: %d\r\n%s%s%sConnection: close\r\n\r\n",
                       status, reason, blen, rid, reh, rah);
   write_all_fd(fd, head, hlen);
   if (blen > 0)
      write_all_fd(fd, body, blen);
}

/* 429 Too Many Requests with a Retry-After header (seconds until the rate
 * window resets). */
static void send_rate_limited(int fd, int retry_after, const char *request_id)
{
   const char *body =
       "{\"error\":{\"message\":\"rate limit exceeded\",\"type\":\"rate_limit_error\"}}";
   int blen = (int)strlen(body);
   char rid[96];
   request_id_header(rid, sizeof(rid), request_id);
   char head[320];
   int hlen = snprintf(head, sizeof(head),
                       "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\n"
                       "Retry-After: %d\r\nContent-Length: %d\r\n%sConnection: close\r\n\r\n",
                       retry_after, blen, rid);
   write_all_fd(fd, head, hlen);
   write_all_fd(fd, body, blen);
}
