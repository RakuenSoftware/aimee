#ifndef AIMEE_SERVER_MGMT_AUDIT_H
#define AIMEE_SERVER_MGMT_AUDIT_H
int server_mgmt_audit_intent(const char *actor, const char *target, const char *cap,
                             const char *jti, const char *request_digest);
int server_mgmt_audit_outcome(const char *actor, const char *target, const char *cap,
                               const char *jti, const char *request_digest, int status);
typedef int (*server_mgmt_action_fn)(void *ctx);
int server_mgmt_dispatch_audited(const char *actor, const char *target, const char *cap,
                                 const char *jti, const char *request_digest,
                                 server_mgmt_action_fn action, void *ctx);
#endif
