/* wfe_approval.c: HMAC-SHA256 approval signer + the gate.human executor. */
#include "wfe_approval.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee_home.h"
#include "cJSON.h"
#include "wfe_store.h"
#include "wfe_def.h"
#include "wfe_engine.h"
#include "wfe_iface.h"

#define WFE_KEY_LEN 32

const char *wfe_approval_key_path(char *buf, size_t cap)
{
   snprintf(buf, cap, "%s/.approval-key", aimee_home());
   return buf;
}

static int load_key(unsigned char key[WFE_KEY_LEN])
{
   char path[1024];
   wfe_approval_key_path(path, sizeof path);
   /* Symlink-safe, TOCTOU-free: open with O_NOFOLLOW, then fstat the actual fd
    * (not a path) and refuse anything with looser-than-0600 perms, a non-regular
    * file, or extra hardlinks — a local attacker must not be able to substitute
    * the HMAC trust root. */
   int fd = open(path, O_RDONLY | O_NOFOLLOW);
   if (fd < 0)
      return -1;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0077) || st.st_nlink != 1)
   {
      close(fd);
      return -1;
   }
   ssize_t n = read(fd, key, WFE_KEY_LEN);
   close(fd);
   return n == (ssize_t)WFE_KEY_LEN ? 0 : -1;
}

int wfe_approval_ensure_key(void)
{
   unsigned char key[WFE_KEY_LEN];
   if (load_key(key) == 0)
      return 0;
   char path[1024];
   wfe_approval_key_path(path, sizeof path);
   /* Atomic, 0600-from-creation, no-symlink-follow: no world-readable window and
    * no first-run race corrupting the key. */
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
   if (fd < 0)
   {
      /* Someone else created it concurrently — accept it iff it loads + perms ok. */
      if (load_key(key) == 0)
         return 0;
      return -1;
   }
   FILE *r = fopen("/dev/urandom", "rb");
   if (!r)
   {
      close(fd);
      unlink(path);
      return -1;
   }
   size_t got = fread(key, 1, WFE_KEY_LEN, r);
   fclose(r);
   int ok = 0;
   if (got == WFE_KEY_LEN)
   {
      ssize_t w = write(fd, key, WFE_KEY_LEN);
      ok = (w == (ssize_t)WFE_KEY_LEN);
   }
   close(fd);
   if (!ok)
   {
      unlink(path);
      return -1;
   }
   return 0;
}

/* HMAC-SHA256(key, msg) -> 32-byte mac, using the workflow SHA-256. */
static void hmac_sha256(const unsigned char *key, size_t keylen, const char *msg,
                        unsigned char mac[32])
{
   unsigned char k[64];
   memset(k, 0, sizeof k);
   if (keylen > 64)
   {
      wfe_sha256_raw(key, keylen, k); /* 32 bytes, rest zero */
   }
   else
   {
      memcpy(k, key, keylen);
   }
   unsigned char ipad[64], opad[64];
   for (int i = 0; i < 64; i++)
   {
      ipad[i] = k[i] ^ 0x36;
      opad[i] = k[i] ^ 0x5c;
   }
   size_t mlen = strlen(msg);
   /* approval messages are bounded (~360 bytes); use a fixed stack buffer so the
    * signer has no allocation failure path. */
   unsigned char inner_in[64 + 2048];
   unsigned char inner[32];
   if (mlen > 2048)
      mlen = 2048;
   memcpy(inner_in, ipad, 64);
   memcpy(inner_in + 64, msg, mlen);
   wfe_sha256_raw(inner_in, 64 + mlen, inner);
   /* mac = H(opad || inner) */
   unsigned char outer_in[64 + 32];
   memcpy(outer_in, opad, 64);
   memcpy(outer_in + 64, inner, 32);
   wfe_sha256_raw(outer_in, 96, mac);
}

static void approval_msg(char *buf, size_t cap, const char *wi, const char *gate, const char *hash,
                         const char *actor, const char *ts)
{
   snprintf(buf, cap, "v1|%s|%s|%s|%s|%s", wi ? wi : "", gate ? gate : "", hash ? hash : "",
            actor ? actor : "", ts ? ts : "");
}

int wfe_approval_sign(const char *wi, const char *gate, const char *content_hash, const char *actor,
                      const char *ts, char out_hex[65])
{
   unsigned char key[WFE_KEY_LEN];
   if (load_key(key) != 0)
      return -1;
   char msg[2048];
   approval_msg(msg, sizeof msg, wi, gate, content_hash, actor, ts);
   unsigned char mac[32];
   hmac_sha256(key, WFE_KEY_LEN, msg, mac);
   static const char hx[] = "0123456789abcdef";
   for (int i = 0; i < 32; i++)
   {
      out_hex[i * 2] = hx[mac[i] >> 4];
      out_hex[i * 2 + 1] = hx[mac[i] & 0xf];
   }
   out_hex[64] = '\0';
   return 0;
}

int wfe_approval_verify(const char *wi, const char *gate, const char *content_hash,
                        const char *actor, const char *ts, const char *sig_hex)
{
   char expect[65];
   if (!sig_hex || wfe_approval_sign(wi, gate, content_hash, actor, ts, expect) != 0)
      return 0;
   if (strlen(sig_hex) != 64)
      return 0;
   /* constant-time compare */
   unsigned char diff = 0;
   for (int i = 0; i < 64; i++)
      diff |= (unsigned char)(expect[i] ^ sig_hex[i]);
   return diff == 0 ? 1 : 0;
}

/* approval detail is JSON {"ts":..., "sig":...}; content_hash is the event's
 * content_hash column. */
int wfe_approval_record(const char *wi, const char *gate, const char *content_hash,
                        const char *actor)
{
   /* timestamp: a monotonic-ish marker; the engine stamps created_at in DB. */
   char ts[40];
   snprintf(ts, sizeof ts, "%s", content_hash ? content_hash : "");
   const char *act = actor ? actor : "user";
   char sig[65] = "";
   if (wfe_approval_sign(wi, gate, content_hash, act, ts, sig) != 0)
      return -1;
   char detail[256];
   snprintf(detail, sizeof detail, "{\"ts\":\"%s\",\"sig\":\"%s\"}", ts, sig);
   return db1_lifecycle_event_add(wi, gate, "approve", actor ? actor : "user", detail, content_hash,
                                  0);
}

int wfe_approval_present(const char *wi, const char *gate, const char *content_hash)
{
   db1_lifecycle_event_t *evs = NULL;
   int n = db1_lifecycle_event_list(wi, &evs);
   int ok = 0;
   for (int i = 0; i < n && !ok; i++)
   {
      if (strcmp(evs[i].kind, "approve") != 0)
         continue;
      if (strcmp(evs[i].stage, gate) != 0)
         continue;
      if (strcmp(evs[i].content_hash, content_hash ? content_hash : "") != 0)
         continue; /* stale: artifact changed */
      cJSON *d = cJSON_Parse(evs[i].detail);
      if (d)
      {
         const cJSON *ts = cJSON_GetObjectItemCaseSensitive(d, "ts");
         const cJSON *sig = cJSON_GetObjectItemCaseSensitive(d, "sig");
         /* verify against the RECORDED actor, so the actor field is authenticated
          * by the MAC (a tampered actor invalidates the signature). */
         if (cJSON_IsString(ts) && cJSON_IsString(sig) &&
             wfe_approval_verify(wi, gate, evs[i].content_hash, evs[i].actor, ts->valuestring,
                                 sig->valuestring))
            ok = 1;
         cJSON_Delete(d);
      }
   }
   free(evs);
   return ok;
}

/* gate.human executor: advance iff a valid approval matches the current
 * artifact hash; otherwise park pending_human. */
static wfe_step_result_t exec_human(wfe_ctx *ctx, const wfe_node_t *node)
{
   const char *wi = wfe_ctx_work_item(ctx);
   /* current artifact hash = the work item's recorded content_hash */
   db1_work_item_t row;
   char hash[72] = "";
   if (wi && db1_work_item_get(wi, &row) == 1)
      snprintf(hash, sizeof hash, "%s", row.content_hash);

   if (wfe_approval_present(wi, node->id, hash))
   {
      char handle[80];
      snprintf(handle, sizeof handle, "%s.out", node->id);
      return wfe_step_advanced(handle, hash, 0.0);
   }
   return wfe_step_pending(WFE_PAUSE_PENDING_HUMAN);
}

void wfe_register_human_gate(void)
{
   wfe_register_block_executor(WFE_BLK_GATE_HUMAN, exec_human);
}
