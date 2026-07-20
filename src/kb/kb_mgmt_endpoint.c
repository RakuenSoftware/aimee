#include "kb_mgmt_endpoint.h"
#include <string.h>
#include <ctype.h>
int kb_mgmt_endpoint_validate(const char *e){
 if(!e||strncmp(e,"https://",8))return -1;
 const char*p=e+8;
 if(!*p)return -1;
 size_t n=strlen(p); if(n>500||p[n-1]=='/'||strchr(p,'?')||strchr(p,'#')||strchr(p,'@'))return -1;
 for(size_t i=0;i<n;i++){unsigned char c=(unsigned char)p[i];if(isspace(c)||c<0x20||c==':'||c=='/')continue;if(!(isalnum(c)||c=='.'||c=='-'||c=='['||c==']'))return -1;}
 if(!strncasecmp(p,"localhost",9)||!strncmp(p,"127.0.0.1",9)||!strncmp(p,"[::1]",5)||!strncmp(p,"169.254.169.254",14))return -1;
 if(!strncmp(p,"10.",3)||!strncmp(p,"192.168.",8)||!strncmp(p,"169.254.",8)||!strncmp(p,"172.16.",7)||!strncmp(p,"172.17.",7)||!strncmp(p,"172.18.",7)||!strncmp(p,"172.19.",7)||!strncmp(p,"172.2",5)||!strncmp(p,"172.30.",7)||!strncmp(p,"172.31.",7)||!strncmp(p,"[fc",3)||!strncmp(p,"[fd",3)||!strncmp(p,"[fe8",4))return -1;
 return 0;
}
