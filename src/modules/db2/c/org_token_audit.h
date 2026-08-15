#ifndef AIMEE_ORG_TOKEN_AUDIT_H
#define AIMEE_ORG_TOKEN_AUDIT_H
#include <stdint.h>
int db2_org_token_audit_start(const char *,const char *,const char *,const char *,int64_t,int,int64_t,const char *,int64_t,const char *,const char *,int64_t *);
int db2_org_token_audit_settle(const char *,const char *,const char *,const char *,int64_t,int64_t,int64_t,int64_t,const char *,const char *);
#endif
