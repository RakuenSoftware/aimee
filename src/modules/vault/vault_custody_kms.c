#include "vault_custody_kms.h"
#include "vault_crypto.h"
#include <openssl/crypto.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

/* The helper is the cloud-KMS adapter: it receives the operation name and key
 * id via argv/env and must emit exactly one 32-byte decrypted root to stdout.
 * No shell is used, output is bounded, and any short/extra output fails closed. */
typedef struct { int sealed; } kms_ctx;
static kms_ctx g = {1};
static int get_kek(void *v, uint8_t out[VAULT_KEK_LEN])
{
   kms_ctx *c=v; const char *helper=getenv("AIMEE_VAULT_KMS_HELPER");
   const char *key_id=getenv("AIMEE_VAULT_KMS_KEY_ID");
   if (!helper || !*helper || !key_id || !*key_id) return -1;
   struct stat st; if (stat(helper,&st)!=0 || !S_ISREG(st.st_mode) || (st.st_mode & 022) || access(helper,X_OK)!=0) return -1;
   int p[2]; if(pipe(p)!=0)return -1; pid_t pid=fork();
   if(pid<0){close(p[0]);close(p[1]);return -1;}
   if(pid==0){dup2(p[1],STDOUT_FILENO);close(p[0]);close(p[1]);execl(helper,helper,"decrypt",(char*)NULL);_exit(127);}
   close(p[1]); size_t n=0; int bad=0; while(n<VAULT_KEK_LEN){ssize_t r=read(p[0],out+n,VAULT_KEK_LEN-n);if(r<0&&errno==EINTR)continue;if(r<=0){bad=1;break;}n+=(size_t)r;}
   uint8_t extra; if(!bad&&read(p[0],&extra,1)>0)bad=1; close(p[0]); int ws=0;waitpid(pid,&ws,0);
   if(bad||n!=VAULT_KEK_LEN||!WIFEXITED(ws)||WEXITSTATUS(ws)!=0){OPENSSL_cleanse(out,VAULT_KEK_LEN);return -1;} c->sealed=0; return 0;
}
static int sealed(void*v){return ((kms_ctx*)v)->sealed;}
static int unseal(void*v,const void*p,size_t n){(void)p;(void)n;uint8_t k[VAULT_KEK_LEN];int r=get_kek(v,k);OPENSSL_cleanse(k,sizeof(k));return r;}
static int seal(void*v){((kms_ctx*)v)->sealed=1;return 0;}
static int rotate(void*v,const char*a,int*b,int*c,char*d,size_t e,char*f,size_t g){(void)v;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;return -1;}
static const vault_custody_provider_t p={"kms",&g,get_kek,rotate,sealed,unseal,seal};
const vault_custody_provider_t *vault_custody_kms_provider(void){return &p;}
