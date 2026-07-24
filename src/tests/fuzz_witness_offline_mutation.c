/* Mutation fuzz: start from a VALID checkpoint+snapshot+record stream and flip
 * bytes. This reaches the snapshot leaf parser's interior (length prefixes, counts,
 * offsets), which random garbage almost never does. Requirements: never crash, and
 * never report a mutated stream as fully clean-and-verified. */
#include <assert.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#include "modules/vault/vault_witness_checkpoint.h"
#include "modules/vault/vault_witness_export.h"
#include "modules/vault/vault_witness_merkle.h"
#include "modules/vault/vault_witness_offline.h"
#include "modules/vault/vault_witness_record.h"
static uint8_t base[8192]; static size_t base_len;
static void fr(vault_witness_export_kind_t k,const uint8_t*p,size_t n){
  uint8_t f[4096]; size_t fl=0;
  assert(vault_witness_export_frame(k,p,n,f,sizeof f,&fl)==0);
  memcpy(base+base_len,f,fl); base_len+=fl; }
int main(void){
  uint8_t priv[32],pub[32];
  EVP_PKEY*pk=NULL; EVP_PKEY_CTX*c=EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519,NULL);
  assert(c&&EVP_PKEY_keygen_init(c)==1&&EVP_PKEY_keygen(c,&pk)==1);
  size_t l=32; EVP_PKEY_get_raw_private_key(pk,priv,&l); l=32; EVP_PKEY_get_raw_public_key(pk,pub,&l);
  uint8_t head[32]; memset(head,0x5c,32);
  uint8_t snap[256]; size_t o=0;
  snap[o++]=0;snap[o++]=0;snap[o++]=0;snap[o++]=1;
  snap[o++]=0;snap[o++]=3; memcpy(snap+o,"!kb",3); o+=3;
  snap[o++]=0;snap[o++]=6; memcpy(snap+o,"!audit",6); o+=6;
  for(unsigned i=0;i<8;i++) snap[o++]=(uint8_t)(1ULL>>(56-8*i));
  memcpy(snap+o,head,32); o+=32;
  vault_witness_leaf_t leaf;
  assert(vault_witness_shard_key_hash("!kb","!audit",leaf.key)==0);
  assert(vault_witness_leaf_hash("!kb","!audit",1,head,leaf.hash)==0);
  uint8_t root[32]; assert(vault_witness_merkle_root(&leaf,1,root)==0);
  vault_witness_checkpoint_t cp; memset(&cp,0,sizeof cp);
  cp.version=1;cp.seq=1;cp.shard_count=1;cp.sig_alg=VAULT_WITNESS_SIG_ED25519;cp.sig_version=1;
  memcpy(cp.root,root,32); SHA256(snap,o,cp.leaf_snapshot_digest);
  memset(cp.signer_key_id,0xC9,16); snprintf(cp.created_at,sizeof cp.created_at,"2026-07-23T00:00:00Z");
  assert(vault_witness_checkpoint_sign_ed25519(&cp,priv)==0);
  uint8_t cw[VAULT_WITNESS_CHECKPOINT_WIRE_MAX]; size_t cl=0;
  assert(vault_witness_checkpoint_encode(&cp,cw,sizeof cw,&cl)==0);
  fr(VAULT_WITNESS_EXPORT_CHECKPOINT,cw,cl);
  uint8_t pl[512]; for(unsigned i=0;i<8;i++) pl[i]=(uint8_t)(1ULL>>(56-8*i));
  memcpy(pl+8,snap,o); fr(VAULT_WITNESS_EXPORT_SNAPSHOT,pl,8+o);
  vault_witness_anchor_t a; memset(&a,0,sizeof a);
  memset(a.key_id,0xC9,16); memcpy(a.ed25519_pub,pub,32);
  /* baseline must be fully clean, else the fuzz proves nothing */
  vault_witness_offline_report_t r0;
  assert(vault_witness_offline_verify(base,base_len,&a,1,&r0)==0);
  assert(r0.any_tamper==0 && r0.snapshots_ok==1);
  unsigned seed=99; int missed=0;
  uint8_t buf[8192];
  for(int it=0; it<60000; it++){
    memcpy(buf,base,base_len);
    size_t nmut = 1 + ((seed=seed*1103515245u+12345u)%3);
    for(size_t m=0;m<nmut;m++){
      seed=seed*1103515245u+12345u; size_t pos=seed%base_len;
      seed=seed*1103515245u+12345u; buf[pos]^=(uint8_t)(1u<<(seed%8));
    }
    if(!memcmp(buf,base,base_len)) continue;
    vault_witness_offline_report_t r;
    int rc=vault_witness_offline_verify(buf,base_len,&a,1,&r);
    assert(rc==0);
    /* A mutated stream must never look like a fully verified clean one. */
    if(r.any_tamper==0 && r.malformed==0 && r.snapshots_ok==1 && r.checkpoints_ok==1) missed++;
  }
  printf("mutation_fuzz: 60000 mutations, silently-clean=%d\n", missed);
  /* Leak-clean so this passes as a CI unit test under ASAN. */
  EVP_PKEY_free(pk);
  EVP_PKEY_CTX_free(c);
  return missed? 1:0; }
