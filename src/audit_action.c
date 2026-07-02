/* audit_action.c: governed-action audit primitives. See audit_action.h for the
 * args_hash contract. */
#include "audit_action.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee_home.h"
#include "cJSON.h"
#include "platform_path.h"
#include "wfe_def.h" /* wfe_sha256_raw */

#define AUDIT_KEY_LEN 32

/* Bounds (fold truncation markers into the hash input so the digest stays
 * stable and verifiable when a limit is hit). */
#define AUDIT_ARGS_MAX_INPUT (256 * 1024) /* skip parsing beyond this */
#define AUDIT_VALUE_MAX_BYTES 8192        /* per allowlisted value */
#define AUDIT_CANON_MAX_BYTES (64 * 1024) /* total canonical form */

/* Field separators kept out of ordinary JSON text so they cannot be forged from
 * within a value. */
#define SEP_FIELD "\x1f"  /* between fields */
#define SEP_KV "\x1e"     /* between key and value */

/* ---- per-tool allowlist -------------------------------------------------- */

typedef struct
{
   const char *tool;
   const char *fields[6]; /* NULL-terminated; decision-relevant args only */
} tool_allowlist_t;

/* Only decision-relevant fields per governed tool. A tool absent here hashes its
 * NAME ONLY. Field order here IS the canonical order — do not reorder without
 * bumping the version prefix. */
static const tool_allowlist_t ALLOWLIST[] = {
    {"Write", {"file_path", "content", NULL}},
    {"Edit", {"file_path", "old_string", "new_string", NULL}},
    {"NotebookEdit", {"notebook_path", "new_source", NULL}},
    {"Read", {"file_path", NULL}},
    {"Bash", {"command", NULL}},
    {"execute_script", {"command", "script", NULL}},
    {"WebFetch", {"url", NULL}},
    {"WebSearch", {"query", NULL}},
};

static const tool_allowlist_t *allowlist_for(const char *tool)
{
   if (!tool)
      return NULL;
   for (size_t i = 0; i < sizeof(ALLOWLIST) / sizeof(ALLOWLIST[0]); i++)
      if (strcmp(ALLOWLIST[i].tool, tool) == 0)
         return &ALLOWLIST[i];
   return NULL;
}

/* ---- key management (mirrors wfe_approval_ensure_key) --------------------- */

static void audit_key_path(char *buf, size_t cap)
{
   snprintf(buf, cap, "%s/.audit-key", aimee_home());
}

static int audit_load_key(unsigned char key[AUDIT_KEY_LEN])
{
   char path[1024];
   audit_key_path(path, sizeof path);
   int fd = open(path, O_RDONLY | O_NOFOLLOW);
   if (fd < 0)
      return -1;
   ssize_t n = read(fd, key, AUDIT_KEY_LEN);
   close(fd);
   return n == (ssize_t)AUDIT_KEY_LEN ? 0 : -1;
}

int audit_ensure_key(void)
{
   unsigned char key[AUDIT_KEY_LEN];
   if (audit_load_key(key) == 0)
      return 0;
   char path[1024];
   audit_key_path(path, sizeof path);
   /* Atomic, 0600-from-creation, no-symlink-follow: no world-readable window and
    * no first-run race corrupting the key. */
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
   if (fd < 0)
   {
      /* Someone else created it concurrently — accept iff it now loads. */
      return audit_load_key(key) == 0 ? 0 : -1;
   }
   FILE *r = fopen("/dev/urandom", "rb");
   if (!r)
   {
      close(fd);
      unlink(path);
      return -1;
   }
   size_t got = fread(key, 1, AUDIT_KEY_LEN, r);
   fclose(r);
   int ok = 0;
   if (got == AUDIT_KEY_LEN)
   {
      ssize_t w = write(fd, key, AUDIT_KEY_LEN);
      ok = (w == (ssize_t)AUDIT_KEY_LEN);
   }
   close(fd);
   if (!ok)
   {
      unlink(path);
      return -1;
   }
   platform_set_permissions(path, 0600);
   return 0;
}

/* ---- HMAC-SHA256 over wfe_sha256_raw ------------------------------------- */

static void hmac_sha256(const unsigned char *key, size_t keylen, const unsigned char *msg,
                        size_t mlen, unsigned char mac[32])
{
   unsigned char k[64];
   memset(k, 0, sizeof k);
   if (keylen > 64)
      wfe_sha256_raw(key, keylen, k);
   else
      memcpy(k, key, keylen);

   unsigned char ipad[64], opad[64];
   for (int i = 0; i < 64; i++)
   {
      ipad[i] = k[i] ^ 0x36;
      opad[i] = k[i] ^ 0x5c;
   }
   /* inner = H(ipad || msg) */
   unsigned char *inner_in = malloc(64 + mlen);
   unsigned char inner[32];
   if (!inner_in)
   {
      /* Degrade to unkeyed over the message length only; caller treats a hash
       * failure as best-effort. Still deterministic. */
      unsigned char lenbuf[8];
      for (int i = 0; i < 8; i++)
         lenbuf[i] = (unsigned char)((mlen >> (8 * i)) & 0xff);
      wfe_sha256_raw(lenbuf, sizeof lenbuf, mac);
      return;
   }
   memcpy(inner_in, ipad, 64);
   memcpy(inner_in + 64, msg, mlen);
   wfe_sha256_raw(inner_in, 64 + mlen, inner);
   free(inner_in);
   /* mac = H(opad || inner) */
   unsigned char outer_in[64 + 32];
   memcpy(outer_in, opad, 64);
   memcpy(outer_in + 64, inner, 32);
   wfe_sha256_raw(outer_in, 96, mac);
}

/* ---- canonical projection ------------------------------------------------ */

/* Append `s` (len bytes) to canon[*pos], capped at AUDIT_CANON_MAX_BYTES. If the
 * append would overflow, append a stable truncation marker once and stop. */
static void canon_append(char *canon, size_t *pos, const char *s, size_t len)
{
   if (*pos >= AUDIT_CANON_MAX_BYTES)
      return;
   size_t room = AUDIT_CANON_MAX_BYTES - *pos;
   if (len > room)
   {
      static const char mark[] = SEP_FIELD "<trunc>";
      size_t mlen = sizeof(mark) - 1;
      size_t take = room > mlen ? room - mlen : 0;
      memcpy(canon + *pos, s, take);
      *pos += take;
      memcpy(canon + *pos, mark, room - take > mlen ? mlen : room - take);
      *pos = AUDIT_CANON_MAX_BYTES; /* freeze */
      return;
   }
   memcpy(canon + *pos, s, len);
   *pos += len;
}

/* Serialize a cJSON value to a bounded string for the canonical form. Strings use
 * their raw text; everything else uses compact JSON. Values are capped at
 * AUDIT_VALUE_MAX_BYTES with a stable marker. */
static void canon_append_value(char *canon, size_t *pos, const cJSON *v)
{
   char *owned = NULL;
   const char *text;
   size_t len;
   char numbuf[64];
   if (cJSON_IsString(v) && v->valuestring)
   {
      text = v->valuestring;
      len = strlen(text);
   }
   else if (cJSON_IsNumber(v))
   {
      snprintf(numbuf, sizeof numbuf, "%.17g", v->valuedouble);
      text = numbuf;
      len = strlen(numbuf);
   }
   else if (cJSON_IsBool(v))
   {
      text = cJSON_IsTrue(v) ? "true" : "false";
      len = strlen(text);
   }
   else if (cJSON_IsNull(v))
   {
      text = "null";
      len = 4;
   }
   else
   {
      owned = cJSON_PrintUnformatted(v);
      text = owned ? owned : "";
      len = strlen(text);
   }
   if (len > AUDIT_VALUE_MAX_BYTES)
   {
      canon_append(canon, pos, text, AUDIT_VALUE_MAX_BYTES);
      canon_append(canon, pos, SEP_KV "<vtrunc>", 9);
   }
   else
   {
      canon_append(canon, pos, text, len);
   }
   if (owned)
      free(owned);
}

int audit_args_hash(const char *tool_name, const char *args_json, char *out, size_t out_sz)
{
   /* Stable sentinel for any failure path. */
   if (out && out_sz >= AUDIT_ARGS_HASH_LEN)
   {
      memcpy(out, "v1-", 3);
      memset(out + 3, '0', 64);
      out[67] = '\0';
   }
   if (!out || out_sz < AUDIT_ARGS_HASH_LEN)
      return -1;

   unsigned char key[AUDIT_KEY_LEN];
   if (audit_load_key(key) != 0)
      return -1; /* no key -> caller skips the row (never HMAC-over-empty) */

   char *canon = malloc(AUDIT_CANON_MAX_BYTES + 16);
   if (!canon)
      return -1;
   size_t pos = 0;

   /* Always lead with the tool name (name-only for unknown tools). */
   canon_append(canon, &pos, tool_name ? tool_name : "", tool_name ? strlen(tool_name) : 0);

   const tool_allowlist_t *al = allowlist_for(tool_name);
   if (al && args_json && *args_json)
   {
      size_t jlen = strlen(args_json);
      if (jlen > AUDIT_ARGS_MAX_INPUT)
      {
         /* Oversize input: fold a stable marker + length, skip parsing (DoS bound). */
         char marker[64];
         int n = snprintf(marker, sizeof marker, SEP_FIELD "<oversize:%zu>", jlen);
         canon_append(canon, &pos, marker, (size_t)n);
      }
      else
      {
         cJSON *root = cJSON_Parse(args_json);
         if (root)
         {
            for (const char *const *f = al->fields; *f; f++)
            {
               cJSON *item = cJSON_GetObjectItemCaseSensitive(root, *f);
               if (!item)
                  continue; /* absent field contributes nothing */
               canon_append(canon, &pos, SEP_FIELD, 1);
               canon_append(canon, &pos, *f, strlen(*f));
               canon_append(canon, &pos, SEP_KV, 1);
               canon_append_value(canon, &pos, item);
            }
            cJSON_Delete(root);
         }
         /* Unparseable JSON: hash tool-name only (values never guessed). */
      }
   }

   unsigned char mac[32];
   hmac_sha256(key, AUDIT_KEY_LEN, (const unsigned char *)canon, pos, mac);
   free(canon);

   static const char hx[] = "0123456789abcdef";
   memcpy(out, "v1-", 3);
   for (int i = 0; i < 32; i++)
   {
      out[3 + i * 2] = hx[(mac[i] >> 4) & 0xf];
      out[3 + i * 2 + 1] = hx[mac[i] & 0xf];
   }
   out[67] = '\0';
   return 0;
}
