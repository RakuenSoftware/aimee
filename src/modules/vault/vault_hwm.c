#include "vault_hwm.h"
#include <openssl/evp.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int vault_hwm_attest_verify(const char *key_id, uint64_t version, const uint8_t *att, size_t len)
{
 const char *p=getenv("AIMEE_VAULT_KMS_HWM_PUBKEY"),*d=getenv("AIMEE_VAULT_KMS_HWM_DOMAIN");
 if(!p||!d||!*p||!*d||!key_id||!att||len!=64)return -1;
 struct stat st;
 if(stat(p,&st)!=0||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&022))return -1;
 uint8_t pk[32]; int fd=open(p,O_RDONLY|O_CLOEXEC); if(fd<0||read(fd,pk,32)!=32||read(fd,pk,1)>0){if(fd>=0)close(fd);return -1;} close(fd);
 char msg[512]; int n=snprintf(msg,sizeof(msg),"aimee-hwm-v1|%s|%llu|%s",key_id,(unsigned long long)version,d); if(n<0||(size_t)n>=sizeof(msg))return -1;
 EVP_PKEY *k=EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,NULL,pk,32); EVP_MD_CTX *c=EVP_MD_CTX_new(); int ok=k&&c&&EVP_DigestVerifyInit(c,NULL,NULL,NULL,k)==1&&EVP_DigestVerify(c,att,64,(uint8_t*)msg,(size_t)n)==1; EVP_MD_CTX_free(c); EVP_PKEY_free(k); return ok?0:-1;
}
