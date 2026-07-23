/* aimee-witness-verify: the offline witness verifier.
 *
 * Runs entirely off captured bytes — no aimee-kb, no database, no network — so it
 * can verify a retained copy of the emitted witness stream during an incident,
 * from a host the attacker did not control. This is the "detection by comparison"
 * tool.
 *
 *   aimee-witness-verify <stream-file> <anchor-file>
 *
 * <stream-file>  concatenated emitted export frames (records, checkpoints, proofs,
 *                leaf snapshots).
 * <anchor-file>  one trust anchor per line, out of band:
 *                  <32-hex key_id>:<64-hex ed25519 pubkey>[:revoked]
 *                blank lines and lines starting with '#' are ignored.
 *
 * Exit: 0 = verified, no tampering (continuity may be "unproven" — reported, and a
 *           work item, but not a failure by itself);
 *       1 = tampering detected (broken chain, bad/unknown/revoked checkpoint
 *           signature, bad proof, malformed frame);
 *       2 = usage or I/O error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/vault/vault_witness_offline.h"

static int hexval(int c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

/* Decode exactly `nbytes` from `nbytes*2` hex chars. Returns 0/-1. */
static int unhex(const char *s, uint8_t *out, size_t nbytes)
{
   for (size_t i = 0; i < nbytes; i++)
   {
      int hi = hexval((unsigned char)s[i * 2]), lo = hexval((unsigned char)s[i * 2 + 1]);
      if (hi < 0 || lo < 0)
         return -1;
      out[i] = (uint8_t)((hi << 4) | lo);
   }
   return 0;
}

static uint8_t *read_file(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long sz = ftell(f);
   if (sz < 0 || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return NULL;
   }
   uint8_t *buf = malloc((size_t)sz ? (size_t)sz : 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t got = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   if (got != (size_t)sz)
   {
      free(buf);
      return NULL;
   }
   *len = (size_t)sz;
   return buf;
}

/* Parse the anchor file into a growable array. Returns count, or -1 on a bad line. */
static long parse_anchors(const char *path, vault_witness_anchor_t **out)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return -2;
   vault_witness_anchor_t *a = NULL;
   size_t n = 0, cap = 0;
   char line[256];
   long rc = 0;
   while (fgets(line, sizeof line, f))
   {
      size_t l = strlen(line);
      while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' '))
         line[--l] = '\0';
      if (l == 0 || line[0] == '#')
         continue;
      /* Expect 32 hex, ':', 64 hex, optionally ":revoked". */
      if (l < 32 + 1 + 64 || line[32] != ':')
      {
         rc = -1;
         break;
      }
      vault_witness_anchor_t entry;
      memset(&entry, 0, sizeof entry);
      if (unhex(line, entry.key_id, 16) != 0 || unhex(line + 33, entry.ed25519_pub, 32) != 0)
      {
         rc = -1;
         break;
      }
      if (l > 33 + 64)
      {
         if (strcmp(line + 33 + 64, ":revoked") != 0)
         {
            rc = -1;
            break;
         }
         entry.revoked = 1;
      }
      if (n == cap)
      {
         size_t nc = cap ? cap * 2 : 8;
         void *nv = realloc(a, nc * sizeof *a);
         if (!nv)
         {
            rc = -2;
            break;
         }
         a = nv;
         cap = nc;
      }
      a[n++] = entry;
   }
   fclose(f);
   if (rc < 0)
   {
      free(a);
      return rc;
   }
   *out = a;
   return (long)n;
}

int main(int argc, char **argv)
{
   if (argc != 3)
   {
      fprintf(stderr, "usage: %s <stream-file> <anchor-file>\n", argv[0]);
      return 2;
   }
   size_t slen = 0;
   uint8_t *stream = read_file(argv[1], &slen);
   if (!stream)
   {
      fprintf(stderr, "cannot read stream file: %s\n", argv[1]);
      return 2;
   }
   vault_witness_anchor_t *anchors = NULL;
   long na = parse_anchors(argv[2], &anchors);
   if (na < 0)
   {
      fprintf(stderr, "cannot read/parse anchor file: %s\n", argv[2]);
      free(stream);
      return 2;
   }

   vault_witness_offline_report_t r;
   int vr = vault_witness_offline_verify(stream, slen, anchors, (size_t)na, &r);
   free(stream);
   free(anchors);
   if (vr != 0)
   {
      fprintf(stderr, "internal error\n");
      return 2;
   }

   printf("frames=%zu records=%zu checkpoints=%zu proofs=%zu unknown=%zu\n", r.frames, r.records,
          r.checkpoints, r.proofs, r.unknown_frames);
   printf("record-chains: ok=%zu broken=%zu (duplicates=%zu collapsed, seq-conflicts=%zu)\n",
          r.shards_ok, r.shards_broken, r.records_duplicate, r.records_conflict);
   printf("checkpoints: ok=%zu bad_sig=%zu unknown_key=%zu revoked=%zu "
          "(duplicates=%zu collapsed, seq-conflicts=%zu)\n",
          r.checkpoints_ok, r.checkpoints_bad_sig, r.checkpoints_unknown_key,
          r.checkpoints_revoked, r.checkpoints_duplicate, r.checkpoints_conflict);
   printf("proofs: ok=%zu unmatched=%zu bad=%zu\n", r.proofs_ok, r.proofs_unmatched, r.proofs_bad);
   printf("leaf-snapshots: ok=%zu unmatched=%zu bad=%zu\n", r.snapshots_ok, r.snapshots_unmatched,
          r.snapshots_bad);
   const char *cont = r.continuity == VAULT_WITNESS_CONTINUITY_OK        ? "ok"
                      : r.continuity == VAULT_WITNESS_CONTINUITY_UNPROVEN ? "UNPROVEN (work item: "
                                                                           "compare cross-gap leaves)"
                                                                         : "BROKEN";
   printf("continuity: %s\n", cont);
   if (r.malformed)
      printf("WARNING: one or more frames were malformed\n");

   if (r.any_tamper)
   {
      printf("RESULT: TAMPERING DETECTED\n");
      return 1;
   }
   if (r.continuity == VAULT_WITNESS_CONTINUITY_UNPROVEN)
      printf("RESULT: verified, but continuity unproven — retained records across the gap must be "
             "compared before concluding clean\n");
   else
      printf("RESULT: verified, no tampering detected\n");
   return 0;
}
