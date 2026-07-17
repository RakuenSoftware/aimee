/* aimee_errors.h -- aimee-specific error codes (>=1000).
 *
 * HTTP status codes (<600) describe the TRANSPORT: a 502 says "the upstream gave
 * me a bad reply", a 503 says "this service can't serve the request right now".
 * When the fault is aimee's OWN doing -- no primary agent configured, a provider
 * circuit breaker is open, credentials can't be resolved -- reusing an HTTP 5xx
 * that connotes an EXTERNAL problem sends operators chasing the wrong thing (a
 * fast-failing circuit breaker looks identical to a dead upstream under a 502).
 *
 * So aimee-internal failures carry a code in a range HTTP never uses (>=1000),
 * surfaced in the JSON error body ("code") and every log line ("aimee_err="),
 * while the WIRE status stays a standard, honest 3-digit code (503 for "this
 * service is unavailable", 500 for an internal fault) -- never 502, which is
 * reserved for a genuine upstream provider failure. The 4-digit code stays OUT
 * of the status line so strict clients (the Anthropic SDK, proxies) don't choke.
 *
 * Ranges: 10xx = ingress / routing / provider control-plane. Grow by family. */
#ifndef DEC_AIMEE_ERRORS_H
#define DEC_AIMEE_ERRORS_H 1

enum aimee_error_code
{
   /* No usable primary agent: default unset/disabled and no enabled agent. 503. */
   AIMEE_ERR_NO_PRIMARY = 1001,
   /* The primary agent exists but its endpoint or credentials could not be
    * resolved (e.g. a codex-oauth seat with no available token). NOT an upstream
    * failure -- the request never left the box. 503. */
   AIMEE_ERR_ROUTE_UNRESOLVED = 1002,
   /* The gateway request pipeline (memory injection / tool policing) hard-failed
    * before any provider call. Internal fault. 500. */
   AIMEE_ERR_REQUEST_PIPELINE = 1003,
   /* A provider circuit breaker is open (agent health marked DOWN): aimee is
    * declining to call a provider it believes is failing. 503. */
   AIMEE_ERR_BREAKER_OPEN = 1010,
   /* The provider's per-agent concurrency slots are exhausted (max_parallel).
    * Transient/aimee-side back-pressure, not an upstream error. 503. */
   AIMEE_ERR_CONCURRENCY_LIMIT = 1011,
};

/* Short, stable, greppable slug for a code -- used in log lines. */
static inline const char *aimee_err_slug(int code)
{
   switch (code)
   {
   case AIMEE_ERR_NO_PRIMARY:
      return "no_primary";
   case AIMEE_ERR_ROUTE_UNRESOLVED:
      return "route_unresolved";
   case AIMEE_ERR_REQUEST_PIPELINE:
      return "request_pipeline";
   case AIMEE_ERR_BREAKER_OPEN:
      return "breaker_open";
   case AIMEE_ERR_CONCURRENCY_LIMIT:
      return "concurrency_limit";
   default:
      return "unknown";
   }
}

#endif /* DEC_AIMEE_ERRORS_H */
