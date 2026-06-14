/* wfe_canonical.c -- deterministic canonical form (stable under cycles) +
 * content-hash version. The canonical form orders nodes by id and object keys
 * lexicographically, so a CLI<->web round-trip is byte-stable and the version
 * hash is meaningful. */
#include "wfe_def.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yaml.h"

/* Self-contained SHA-256 (public-domain reference design). Kept dependency-free
 * so the workflow module links into the client/server/kb across all build
 * variants (including no-TLS, where OpenSSL is not linked). */
static uint32_t ror32(uint32_t x, int n)
{
   return (x >> n) | (x << (32 - n));
}

static void sha256_compute(const unsigned char *data, size_t len, unsigned char out[32])
{
   static const uint32_t K[64] = {
       0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
       0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
       0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
       0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
       0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
       0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
       0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
       0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
       0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
       0xc67178f2};
   uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

   size_t total = len + 1;
   while (total % 64 != 56)
      total++;
   size_t padded = total + 8;
   unsigned char *msg = calloc(1, padded);
   if (!msg)
   {
      memset(out, 0, 32);
      return;
   }
   memcpy(msg, data, len);
   msg[len] = 0x80;
   uint64_t bits = (uint64_t)len * 8;
   for (int i = 0; i < 8; i++)
      msg[padded - 1 - i] = (unsigned char)(bits >> (8 * i));

   for (size_t off = 0; off < padded; off += 64)
   {
      uint32_t w[64];
      for (int i = 0; i < 16; i++)
         w[i] = ((uint32_t)msg[off + i * 4] << 24) | ((uint32_t)msg[off + i * 4 + 1] << 16) |
                ((uint32_t)msg[off + i * 4 + 2] << 8) | ((uint32_t)msg[off + i * 4 + 3]);
      for (int i = 16; i < 64; i++)
      {
         uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
         uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
         w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
      for (int i = 0; i < 64; i++)
      {
         uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
         uint32_t ch = (e & f) ^ ((~e) & g);
         uint32_t t1 = hh + S1 + ch + K[i] + w[i];
         uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
         uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
         uint32_t t2 = S0 + maj;
         hh = g;
         g = f;
         f = e;
         e = d + t1;
         d = c;
         c = b;
         b = a;
         a = t1 + t2;
      }
      h[0] += a;
      h[1] += b;
      h[2] += c;
      h[3] += d;
      h[4] += e;
      h[5] += f;
      h[6] += g;
      h[7] += hh;
   }
   free(msg);
   for (int i = 0; i < 8; i++)
   {
      out[i * 4] = (unsigned char)(h[i] >> 24);
      out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
      out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
      out[i * 4 + 3] = (unsigned char)(h[i]);
   }
}

int wfe_sha256_hex(const void *data, size_t len, char out_hex[65])
{
   if (!data || !out_hex)
      return -1;
   unsigned char md[32];
   sha256_compute((const unsigned char *)data, len, md);
   static const char hx[] = "0123456789abcdef";
   for (int i = 0; i < 32; i++)
   {
      out_hex[i * 2] = hx[md[i] >> 4];
      out_hex[i * 2 + 1] = hx[md[i] & 0xf];
   }
   out_hex[64] = '\0';
   return 0;
}

static int cmp_str(const void *a, const void *b)
{
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Recursively rebuild a tree with object keys sorted lexicographically. Array
 * order is preserved (node order is fixed by the caller). */
static cJSON *deep_sorted(const cJSON *in)
{
   if (!in)
      return NULL;
   if (cJSON_IsObject(in))
   {
      int n = 0;
      for (const cJSON *c = in->child; c; c = c->next)
         n++;
      const char **keys = calloc((size_t)(n > 0 ? n : 1), sizeof *keys);
      int i = 0;
      for (const cJSON *c = in->child; c; c = c->next)
         keys[i++] = c->string;
      qsort(keys, (size_t)n, sizeof *keys, cmp_str);
      cJSON *out = cJSON_CreateObject();
      for (i = 0; i < n; i++)
      {
         const cJSON *c = cJSON_GetObjectItemCaseSensitive(in, keys[i]);
         cJSON_AddItemToObject(out, keys[i], deep_sorted(c));
      }
      free(keys);
      return out;
   }
   if (cJSON_IsArray(in))
   {
      cJSON *out = cJSON_CreateArray();
      for (const cJSON *c = in->child; c; c = c->next)
         cJSON_AddItemToArray(out, deep_sorted(c));
      return out;
   }
   return cJSON_Duplicate(in, 1);
}

/* Build a normalized tree: nodes sorted by id; each node's edges/inputs only
 * present when set. */
static cJSON *normalize(const wfe_def_t *def)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "name", def->name);
   cJSON_AddStringToObject(root, "start", def->start);

   int n = def->n_nodes;
   int *idx = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
   for (int i = 0; i < n; i++)
      idx[i] = i;
   for (int i = 1; i < n; i++)
   { /* insertion sort node indices by id */
      int k = idx[i], j = i - 1;
      while (j >= 0 && strcmp(def->nodes[idx[j]].id, def->nodes[k].id) > 0)
      {
         idx[j + 1] = idx[j];
         j--;
      }
      idx[j + 1] = k;
   }

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      const wfe_node_t *nd = &def->nodes[idx[i]];
      cJSON *jn = cJSON_CreateObject();
      cJSON_AddStringToObject(jn, "id", nd->id);
      cJSON_AddStringToObject(jn, "block", wfe_block_name(nd->block));
      if (nd->n_ins > 0)
      {
         cJSON *jin = cJSON_CreateObject();
         int bi[WFE_MAX_INS];
         for (int b = 0; b < nd->n_ins; b++)
            bi[b] = b;
         for (int a = 1; a < nd->n_ins; a++)
         {
            int k = bi[a], j = a - 1;
            while (j >= 0 && strcmp(nd->ins[bi[j]].input_name, nd->ins[k].input_name) > 0)
            {
               bi[j + 1] = bi[j];
               j--;
            }
            bi[j + 1] = k;
         }
         for (int b = 0; b < nd->n_ins; b++)
         {
            const wfe_binding_t *bd = &nd->ins[bi[b]];
            char ref[WFE_ID_LEN + WFE_NAME_LEN + 2];
            snprintf(ref, sizeof ref, "%s.%s", bd->producer_id, bd->output_name);
            cJSON_AddStringToObject(jin, bd->input_name, ref);
         }
         cJSON_AddItemToObject(jn, "in", jin);
      }
      if (nd->params)
         cJSON_AddItemToObject(jn, "params", cJSON_Duplicate(nd->params, 1));
      if (nd->next[0])
         cJSON_AddStringToObject(jn, "next", nd->next);
      if (nd->on_pass[0])
         cJSON_AddStringToObject(jn, "on_pass", nd->on_pass);
      if (nd->on_fail[0])
         cJSON_AddStringToObject(jn, "on_fail", nd->on_fail);
      cJSON_AddItemToArray(arr, jn);
   }
   free(idx);
   cJSON_AddItemToObject(root, "nodes", arr);
   return root;
}

char *wfe_def_canonical(const wfe_def_t *def)
{
   if (!def)
      return NULL;
   cJSON *norm = normalize(def);
   cJSON *sorted = deep_sorted(norm);
   cJSON_Delete(norm);
   char *y = yaml_emit(sorted);
   cJSON_Delete(sorted);
   return y;
}

int wfe_def_compute_version(const wfe_def_t *def, char out_hex[65])
{
   if (!def)
      return -1;
   cJSON *norm = normalize(def);
   cJSON *sorted = deep_sorted(norm);
   cJSON_Delete(norm);
   char *s = cJSON_PrintUnformatted(sorted);
   cJSON_Delete(sorted);
   if (!s)
      return -1;
   int rc = wfe_sha256_hex(s, strlen(s), out_hex);
   free(s);
   return rc;
}
