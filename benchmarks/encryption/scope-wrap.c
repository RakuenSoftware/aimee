/* Native scope-wrap cost using the repository's Vault crypto primitives.
 * This measures cipher work, excluding Vault authorization, IPC, and custody. */
#include "vault_crypto.h"
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int wrap(const unsigned char keys[3][32], const unsigned char *aad, size_t alen,
                const unsigned char dek[32], unsigned char envelopes[3][116])
{
    for (int i=0; i<3; ++i) {
        const unsigned char *input=i ? envelopes[i-1] : dek;
        size_t n=32+28*i;
        if (vault_secret_encrypt(keys[i],aad,alen,input,n,envelopes[i],
                                 envelopes[i]+28,envelopes[i]+12)) return -1;
    }
    return 0;
}

static int unwrap(const unsigned char keys[3][32], const unsigned char *aad, size_t alen,
                  const unsigned char envelope[116], unsigned char dek[32])
{
    unsigned char scratch[2][116];
    memcpy(scratch[0],envelope,116);
    for (int i=2; i>=0; --i) {
        unsigned char *in=scratch[(2-i)%2], *out=scratch[(3-i)%2];
        size_t n=32+28*i;
        if (vault_secret_decrypt(keys[i],aad,alen,in,in+28,n,in+12,out)) return -1;
    }
    memcpy(dek,scratch[1],32);
    OPENSSL_cleanse(scratch,sizeof(scratch));
    return 0;
}

int main(void)
{
    enum { N=100000, R=5 };
    unsigned char keys[3][32], dek[32], recovered[32], envelopes[3][116];
    const unsigned char aad[]="benchmark:record-42:project-7:body:revision-1";
    if (vault_crypto_random((unsigned char *)keys,sizeof(keys)) ||
        vault_crypto_random(dek,sizeof(dek))) return 1;
    printf("{\"iterations\":%d,\"repetitions\":%d,\"openssl\":\"%s\",\"samples\":[",N,R,
           OpenSSL_version(OPENSSL_VERSION));
    for (int r=0; r<R; ++r) {
        double start=now();
        for (int j=0; j<N; ++j)
            if (vault_crypto_random(dek,sizeof(dek)) || wrap(keys,aad,sizeof(aad),dek,envelopes))
                return 2;
        double write_us=(now()-start)*1e6/N;
        start=now();
        for (int j=0; j<N; ++j)
            if (unwrap(keys,aad,sizeof(aad),envelopes[2],recovered) ||
                CRYPTO_memcmp(dek,recovered,sizeof(dek))) return 3;
        double read_us=(now()-start)*1e6/N;
        printf("%s{\"wrap_us\":%.6f,\"unwrap_us\":%.6f}",r?",":"",write_us,read_us);
    }
    for (int i=0; i<3; ++i) {
        keys[i][0]^=1;
        if (!unwrap(keys,aad,sizeof(aad),envelopes[2],recovered)) return 4;
        keys[i][0]^=1;
    }
    unsigned char wrong_aad[sizeof(aad)];
    memcpy(wrong_aad,aad,sizeof(aad)); wrong_aad[0]^=1;
    if (!unwrap(keys,wrong_aad,sizeof(aad),envelopes[2],recovered)) return 5;
    envelopes[2][115]^=1;
    if (!unwrap(keys,aad,sizeof(aad),envelopes[2],recovered)) return 6;
    OPENSSL_cleanse(keys,sizeof(keys)); OPENSSL_cleanse(dek,sizeof(dek));
    printf("],\"each_wrong_scope_key_rejected\":true,"
           "\"wrong_aad_rejected\":true,\"tampered_ciphertext_rejected\":true}\n");
    return 0;
}
