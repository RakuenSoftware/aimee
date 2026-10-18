/* Anthropic ingress HTTP metadata; model content uses canonical IR adapters. */
#ifndef DEC_ANTHROPIC_INGRESS_H
#define DEC_ANTHROPIC_INGRESS_H 1

/* Per-request, thread-local native protocol headers. Reset on every request,
 * including absent headers. Values are forwarded only at native egress. */
void anthropic_ingress_set_request_headers(const char *anthropic_version,
                                           const char *anthropic_beta);
const char *anthropic_ingress_request_version(void);
const char *anthropic_ingress_request_beta(void);

#endif /* DEC_ANTHROPIC_INGRESS_H */
