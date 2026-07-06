/* audit_worm_chain.c: shared WORM chain primitives. See audit_worm_chain.h. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aimee_home.h"
#include "audit_worm_chain.h"
#include "dstr.h"
#include "wfe_def.h" /* wfe_sha256_raw */

void audit_worm_hex32(const unsigned char in[32], char out[65])
{
   static const char *h = "0123456789abcdef";
   for (int i = 0; i < 32; i++)
   {
      out[2 * i] = h[in[i] >> 4];
      out[2 * i + 1] = h[in[i] & 0x0f];
   }
   out[64] = '\0';
}

void audit_worm_row_hash(long long seq, const char *actor_role, const char *actor_principal,
                         const char *action, const char *subject, const char *verdict,
                         const char *key_id, const char *detail, const char *prev_hash,
                         char out_hex[65])
{
   char seqbuf[32];
   snprintf(seqbuf, sizeof seqbuf, "%lld", seq);
   const char *fields[] = {seqbuf,  actor_role, actor_principal, action,
                           subject, verdict,    key_id,          detail};
   dstr_t m;
   dstr_init(&m);
   dstr_append_str(&m, AUDIT_WORM_DOMAIN);
   dstr_append_char(&m, '\n');
   dstr_append_str(&m, prev_hash);
   dstr_append_char(&m, '\n');
   for (int i = 0; i < 8; i++)
   {
      const char *v = fields[i] ? fields[i] : "";
      dstr_appendf(&m, "%zu:", strlen(v));
      dstr_append_str(&m, v);
   }
   unsigned char dig[32];
   wfe_sha256_raw(m.data, dstr_len(&m), dig);
   dstr_free(&m);
   audit_worm_hex32(dig, out_hex);
}

/* HMAC-SHA256 over wfe_sha256_raw (standard construction; the chain key is 32
 * bytes, below the 64-byte block size, so no key pre-hash is needed). */
static void worm_hmac_sha256(const unsigned char *key, size_t keylen, const unsigned char *msg,
                             size_t mlen, unsigned char mac[32])
{
   unsigned char k[64];
   memset(k, 0, sizeof k);
   if (keylen > 64)
   {
      unsigned char kh[32];
      wfe_sha256_raw(key, keylen, kh);
      memcpy(k, kh, 32);
   }
   else
      memcpy(k, key, keylen);
   unsigned char ipad[64], opad[64];
   for (int i = 0; i < 64; i++)
   {
      ipad[i] = k[i] ^ 0x36;
      opad[i] = k[i] ^ 0x5c;
   }
   unsigned char *ib = malloc(64 + mlen);
   unsigned char inner[32];
   if (!ib)
   {
      memset(mac, 0, 32);
      return;
   }
   memcpy(ib, ipad, 64);
   memcpy(ib + 64, msg, mlen);
   wfe_sha256_raw(ib, 64 + mlen, inner);
   free(ib);
   unsigned char ob[96];
   memcpy(ob, opad, 64);
   memcpy(ob + 64, inner, 32);
   wfe_sha256_raw(ob, 96, mac);
}

void audit_worm_ckpt_mac(const unsigned char key[32], const char *head_hash, long long head_seq,
                         const char *key_id, char out_hex[65])
{
   char sb[32];
   snprintf(sb, sizeof sb, "%lld", head_seq);
   dstr_t m;
   dstr_init(&m);
   dstr_append_str(&m, AUDIT_WORM_DOMAIN "|ckpt|");
   const char *f[] = {head_hash, sb, key_id};
   for (int i = 0; i < 3; i++)
   {
      dstr_appendf(&m, "%zu:", strlen(f[i]));
      dstr_append_str(&m, f[i]);
   }
   unsigned char mac[32];
   worm_hmac_sha256(key, 32, (const unsigned char *)m.data, dstr_len(&m), mac);
   dstr_free(&m);
   audit_worm_hex32(mac, out_hex);
}

int audit_worm_chain_key_load(unsigned char key[32], char key_id[17])
{
   char path[1024];
   snprintf(path, sizeof path, "%s/.audit-chain-key", aimee_home());
   int fd = open(path, O_RDONLY);
   if (fd >= 0)
   {
      ssize_t n = read(fd, key, 32);
      close(fd);
      if (n != 32)
         return -1;
   }
   else
   {
      int rf = open("/dev/urandom", O_RDONLY);
      if (rf < 0)
         return -1;
      ssize_t n = read(rf, key, 32);
      close(rf);
      if (n != 32)
         return -1;
      int wf = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (wf < 0)
      {
         /* Lost a create race: another writer made it — read theirs. */
         int r2 = open(path, O_RDONLY);
         if (r2 < 0)
            return -1;
         ssize_t m = read(r2, key, 32);
         close(r2);
         if (m != 32)
            return -1;
      }
      else
      {
         if (write(wf, key, 32) != 32)
         {
            close(wf);
            return -1;
         }
         close(wf);
      }
   }
   unsigned char kh[32];
   wfe_sha256_raw(key, 32, kh);
   char full[65];
   audit_worm_hex32(kh, full);
   memcpy(key_id, full, 16); /* key_id = first 16 hex chars of SHA256(key) */
   key_id[16] = '\0';
   return 0;
}
