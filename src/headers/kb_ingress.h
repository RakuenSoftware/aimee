/* kb_ingress.h: deny-by-default identity-header ingress guard (P1 B5). */
#ifndef DEC_KB_INGRESS_H
#define DEC_KB_INGRESS_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* 1 if the raw request's header block contains any spoofable X-Aimee-* identity
    * header (X-Aimee-Principal/-Actor/-Source/-Session-Key/-User), else 0. kb never
    * honors a client-supplied identity header, so the connection handlers reject
    * such a request fail-closed. Pure — unit-testable without a socket. */
   int kb_ingress_identity_header_present(const char *raw_request);

   /* The triple-authenticated aimee-server mTLS boundary may carry exactly one
    * X-Aimee-Caller-Subject assertion, which its connection handler consumes.
    * Every other ingress keeps rejecting it with the spoofable identity set. */
   int kb_ingress_identity_header_present_ex(const char *raw_request,
                                             int allow_service_caller_subject);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_INGRESS_H */
