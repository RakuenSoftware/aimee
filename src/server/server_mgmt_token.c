#include "server_mgmt_token.h"
#include "kb_auth_oidc.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int dec(const char *s, size_t n, unsigned char *o, size_t cap)
{
   static const char a[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   unsigned v=0; int bits=0; size_t z=0;
   for(size_t i=0;i<n;i++){const char*p=strchr(a,s[i]);if(!p)return-1;v=(v<<6)|(unsigned)(p-a);bits+=6;if(bits>=8){bits-=8;if(z>=cap)return-1;o[z++]=(unsigned char)(v>>bits);}}
   return (int)z;
}
int server_mgmt_token_verify(const char *jwt, const char *jwks_json, const char *issuer,
                             const char *audience, long now, kb_verify_result_t *out)
{
   if (!jwt || !jwks_json || !*jwks_json || !out) return 0;
   kb_oidc_config_t c; memset(&c, 0, sizeof(c));
   if (issuer) snprintf(c.issuer, sizeof(c.issuer), "%s", issuer);
   if (audience) snprintf(c.audience, sizeof(c.audience), "%s", audience);
   snprintf(c.jwks_json, sizeof(c.jwks_json), "%s", jwks_json);
   return kb_oidc_verify_jwt(jwt, &c, now, out);
}

int server_mgmt_token_verify_bound(const char *jwt, const char *jwks_json, const char *issuer,
                                   const char *audience, const char *peer_cn, long now,
                                   kb_verify_result_t *out, char *cap, size_t cap_n,
                                   char *jti, size_t jti_n)
{
   if (!peer_cn || !*peer_cn || !cap || !cap_n || !jti || !jti_n) return 0;
   if (!server_mgmt_token_verify(jwt, jwks_json, issuer, audience, now, out)) return 0;
   const char *d1=strchr(jwt,'.'); if(!d1)return 0; const char*d2=strchr(d1+1,'.'); if(!d2)return 0;
   unsigned char raw[8192]; int n=dec(d1+1,(size_t)(d2-d1-1),raw,sizeof(raw)-1); if(n<0)return 0; raw[n]=0;
   cJSON *p=cJSON_Parse((char*)raw); if(!p)return 0;
   cJSON *cn=cJSON_GetObjectItemCaseSensitive(p,"cert_cn");
   cJSON *cc=cJSON_GetObjectItemCaseSensitive(p,"cap");
   cJSON *jj=cJSON_GetObjectItemCaseSensitive(p,"jti");
   int ok=cJSON_IsString(cn)&&cJSON_IsString(cc)&&cJSON_IsString(jj)&&!strcmp(cn->valuestring,peer_cn)&&cc->valuestring[0]&&jj->valuestring[0];
   if(ok){snprintf(cap,cap_n,"%s",cc->valuestring);snprintf(jti,jti_n,"%s",jj->valuestring);}
   cJSON_Delete(p); return ok;
}
