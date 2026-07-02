/* audit_action.c: governed-action audit primitives. See audit_action.h for the
 * args_hash contract. */
#include "audit_action.h"

#include <fcntl.h>
#include <stdint.h>
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

/* Bounds. Truncation markers and length prefixes are folded INTO the hash input
 * so the digest stays stable and verifiable when a limit is hit. */
#define AUDIT_ARGS_MAX_INPUT (256 * 1024) /* skip parsing beyond this */
#define AUDIT_VALUE_MAX_BYTES 8192        /* per allowlisted value */
#define AUDIT_CANON_ALLOC (64 * 1024)     /* canon buffer allocation */
#define AUDIT_CANON_LIMIT (AUDIT_CANON_ALLOC - 32) /* logical fill limit (marker reserve) */

/* Component separator. NOTE: canonical components are LENGTH-PREFIXED (see
 * canon_add), so injectivity does NOT depend on this byte being absent from a
 * value — a value may contain it (e.g. via a  JSON escape) without forging
 * a boundary. The separator is retained only for readability of the hash input. */
#define SEP "\x1f"

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
   int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
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

/* Returns 0 on success, -1 on failure. On failure the caller MUST NOT emit a
 * digest (never an unkeyed hash). */
static int hmac_sha256(const unsigned char *key, size_t keylen, const unsigned char *msg,
                       size_t mlen, unsigned char mac[32])
{
   unsigned char k[64];
   memset(k, 0, sizeof k);
   if (keylen > 64)
      wfe_sha256_raw(key, keylen, k); /* key = H(key): 32 bytes, rest zero */
   else
      memcpy(k, key, keylen);

   unsigned char ipad[64], opad[64];
   for (int i = 0; i < 64; i++)
   {
      ipad[i] = k[i] ^ 0x36;
      opad[i] = k[i] ^ 0x5c;
   }

   if (mlen > SIZE_MAX - 64)
      return -1; /* addition overflow guard */
   unsigned char *inner_in = malloc(64 + mlen);
   if (!inner_in)
      return -1; /* hard fail — never fall back to an unkeyed digest */
   memcpy(inner_in, ipad, 64);
   if (mlen)
      memcpy(inner_in + 64, msg, mlen);
   unsigned char inner[32];
   wfe_sha256_raw(inner_in, 64 + mlen, inner);
   free(inner_in);

   unsigned char outer_in[96]; /* opad(64) || inner(32) */
   memcpy(outer_in, opad, 64);
   memcpy(outer_in + 64, inner, 32);
   wfe_sha256_raw(outer_in, 96, mac);
   return 0;
}

int audit_hmac_sha256_testonly(const unsigned char *key, size_t keylen, const unsigned char *msg,
                               size_t mlen, unsigned char mac[32])
{
   return hmac_sha256(key, keylen, msg, mlen, mac);
}

/* ---- canonical projection (length-prefixed, injective) ------------------- */

/* Append `len` bytes at canon[*pos], capped at AUDIT_CANON_LIMIT. On overflow,
 * fold a stable "<trunc>" marker into the reserved slack (still within the
 * allocation) and freeze so no later component is appended. */
static void canon_append(char *canon, size_t *pos, const char *s, size_t len)
{
   if (*pos >= AUDIT_CANON_LIMIT)
      return; /* frozen */
   size_t room = AUDIT_CANON_LIMIT - *pos;
   size_t take = len < room ? len : room;
   memcpy(canon + *pos, s, take);
   *pos += take;
   if (take < len)
   {
      static const char mark[] = SEP "<trunc>";
      size_t mlen = sizeof(mark) - 1; /* fits in the ALLOC-LIMIT reserve */
      memcpy(canon + *pos, mark, mlen);
      *pos += mlen; /* now >= AUDIT_CANON_LIMIT -> frozen; marker IS hashed */
   }
}

/* Append one length-prefixed component: SEP <declen> ":" <bytes>. The length
 * precedes the bytes, so no in-value byte can forge a component boundary — the
 * canonical form is injective over (tool, projected-fields) regardless of value
 * content. */
static void canon_add(char *canon, size_t *pos, char tag, const char *bytes, size_t len)
{
   char pre[40];
   int n = snprintf(pre, sizeof pre, SEP "%c%zu:", tag, len);
   if (n < 0)
      n = 0;
   if ((size_t)n > sizeof pre)
      n = (int)sizeof pre;
   canon_append(canon, pos, pre, (size_t)n);
   canon_append(canon, pos, bytes, len);
}

/* Materialize a cJSON value to bounded bytes and append it length-prefixed. The
 * 'T'/'F' tag records whether the value was truncated, so a capped 8 KiB value
 * is distinct from an exact 8 KiB value. */
static void canon_add_value(char *canon, size_t *pos, const cJSON *v)
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
      /* Non-scalar (array/object): compact-print. Bounded by the parsed input
       * (<= AUDIT_ARGS_MAX_INPUT); only AUDIT_VALUE_MAX_BYTES enter the hash. */
      owned = cJSON_PrintUnformatted(v);
      text = owned ? owned : "";
      len = strlen(text);
   }
   char tag = 'F';
   if (len > AUDIT_VALUE_MAX_BYTES)
   {
      len = AUDIT_VALUE_MAX_BYTES;
      tag = 'T';
   }
   canon_add(canon, pos, tag, text, len);
   if (owned)
      free(owned);
}

int audit_args_hash(const char *tool_name, const char *args_json, char *out, size_t out_sz)
{
   /* Stable sentinel for any failure path (never a forgeable/unkeyed digest). */
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

   char *canon = malloc(AUDIT_CANON_ALLOC);
   if (!canon)
      return -1;
   size_t pos = 0;

   /* Always lead with the (length-prefixed) tool name; name-only for unknown tools. */
   canon_add(canon, &pos, 'N', tool_name ? tool_name : "", tool_name ? strlen(tool_name) : 0);

   const tool_allowlist_t *al = allowlist_for(tool_name);
   if (al && args_json && *args_json)
   {
      size_t jlen = strnlen(args_json, AUDIT_ARGS_MAX_INPUT + 1);
      if (jlen > AUDIT_ARGS_MAX_INPUT)
      {
         /* Oversize input: fold a stable marker, skip parsing (DoS bound). Uses
          * strnlen so we never scan past the cap. */
         canon_add(canon, &pos, 'O', "oversize", 8);
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
               canon_add(canon, &pos, 'K', *f, strlen(*f));
               canon_add_value(canon, &pos, item);
            }
            cJSON_Delete(root); /* cJSON_Delete(NULL) is a no-op; safe */
         }
         /* Unparseable JSON: tool-name only (values never guessed). */
      }
   }

   unsigned char mac[32];
   int hrc = hmac_sha256(key, AUDIT_KEY_LEN, (const unsigned char *)canon, pos, mac);
   free(canon);
   if (hrc != 0)
      return -1; /* keep the sentinel; never emit an unkeyed digest */

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
