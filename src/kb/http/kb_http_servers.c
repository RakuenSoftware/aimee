#include "kb_http_servers.h"
#include "../../db2/server_registry.h"
#include "../kb_mgmt_endpoint.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int q(const char*s,const char*k,char*o,size_t n){if(!s)return 0;size_t l=strlen(k);for(;*s;s=strchr(s,'&')){if(*s=='&')s++;if(!strncmp(s,k,l)&&s[l]=='='){snprintf(o,n,"%s",s+l+1);char*x=strchr(o,'&');if(x)*x=0;return 1;}}return 0;}
int kb_http_servers_route(const char*m,const char*p,const char*qs,char*out,int cap){if(strcmp(p,"/v1/servers"))return-1;if(strcmp(m,"GET"))return 405;char t[32];if(!q(qs,"team",t,sizeof(t))){snprintf(out,cap,"{\"error\":\"team is required\"}");return 400;}char*e;long long team=strtoll(t,&e,10);if(!*t||*e||team<=0){snprintf(out,cap,"{\"error\":\"invalid team\"}");return 400;}db2_server_row_t rows[64];int n=db2_server_registry_list(team,rows,64);if(n<0){snprintf(out,cap,"{\"error\":\"registry unavailable\"}");return 503;}int used=snprintf(out,cap,"{\"servers\":[");for(int i=0;i<n&&used<cap;i++){if(kb_mgmt_endpoint_validate(rows[i].endpoint)!=0){snprintf(out,cap,"{\"error\":\"unsafe registered endpoint\"}");return 500;}used+=snprintf(out+used,cap-used,"%s{\"server_id\":\"%s\",\"cert_cn\":\"%s\",\"mgmt_cert_cn\":\"%s\",\"endpoint\":\"%s\",\"status\":\"%s\",\"health\":\"%s\",\"version\":\"%s\"}",i?",":"",rows[i].server_id,rows[i].cert_cn,rows[i].mgmt_cert_cn,rows[i].endpoint,rows[i].status,rows[i].health,rows[i].version);}if(used<cap)snprintf(out+used,cap-used,"]}");return 200;}
