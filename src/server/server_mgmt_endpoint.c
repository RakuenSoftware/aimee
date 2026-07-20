#include "server_mgmt_endpoint.h"
#include "server_mgmt_token.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
int server_mgmt_endpoint_dispatch(const char *jwt,const char *jwks,const char *issuer,const char *audience,const char *peer_cn,const char *required_cap,const char *target,const char *digest,server_mgmt_action_fn action,void *ctx,char *actor,size_t actor_cap,char *jti,size_t jti_cap)
{
 if(!jwt||!jwks||!issuer||!audience||!peer_cn||!required_cap||!target||!digest||!action||!actor||!actor_cap||!jti||!jti_cap)return -1;
 kb_verify_result_t vr={0};
 if(server_mgmt_action_authorize(jwt,jwks,issuer,audience,peer_cn,(long)time(NULL),1,required_cap,&vr,jti,jti_cap)!=0)return -1;
 if(snprintf(actor,actor_cap,"%s",vr.subject)>=((int)actor_cap))return -1;
 return server_mgmt_dispatch_audited(actor,target,required_cap,jti,digest,action,ctx);
}
