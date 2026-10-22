/* Anthropic ingress transport metadata. Native content enters the canonical
 * translation adapters; no direct provider-to-client translator lives here. */
#include "anthropic_ingress.h"
#include <stdio.h>

/* --- Per-request passthrough headers (parity mode) ---------------------------
 *
 * Not part of the pure translation: a thin per-request echo of the two inbound
 * headers that select Anthropic API behavior. server_http captures them off the
 * raw request (the ingress handlers never see HTTP headers); the anthropic-driver
 * passthrough forwards them upstream when the primary speaks the Anthropic API. Thread-local
 * and reset on every request, so a value never leaks between requests on a reused
 * worker thread. */
static __thread char g_req_anthropic_version[64] = "";
static __thread char g_req_anthropic_beta[512] = "";

void anthropic_ingress_set_request_headers(const char *anthropic_version,
                                           const char *anthropic_beta)
{
   snprintf(g_req_anthropic_version, sizeof(g_req_anthropic_version), "%s",
            anthropic_version ? anthropic_version : "");
   snprintf(g_req_anthropic_beta, sizeof(g_req_anthropic_beta), "%s",
            anthropic_beta ? anthropic_beta : "");
}

const char *anthropic_ingress_request_version(void)
{
   return g_req_anthropic_version;
}

const char *anthropic_ingress_request_beta(void)
{
   return g_req_anthropic_beta;
}
