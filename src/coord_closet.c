/* coord_closet.c: Coordinate Closet — verbatim identifier conservation (fold §2, P1).
 *
 * See coord_closet.h for the contract. Implementation notes:
 *   - Nomination is a single left-to-right scan with a fixed matcher priority
 *     (uuid > handle > kv > path > sha > ref) so matches never overlap and the
 *     first-occurrence offset is well defined.
 *   - All character classification is ASCII-only (a_* helpers below), never the
 *     locale-sensitive ctype.h functions — byte-identical output must not depend
 *     on the process locale.
 *   - Determinism comes from the explicit array + total-order sort in render
 *     (never from hash-map iteration or insertion order).
 *   - Rendering sizes every line with snprintf(NULL,0,...) before writing, so a
 *     long coordinate is never silently truncated into a fixed buffer.
 *   - Secrets are matched by value prefix/path patterns AND sensitive label names,
 *     and are redacted at render time (label conserved, value dropped). */
#include "coord_closet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- ASCII ctype */

static int a_lower(int c)
{
   return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}
static int a_alpha(int c)
{
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int a_digit(int c)
{
   return c >= '0' && c <= '9';
}
static int a_alnum(int c)
{
   return a_alpha(c) || a_digit(c);
}
static int is_hex(int c)
{
   return a_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Self-contained string helpers (no POSIX-only strdup/strncasecmp — keeps the
 * module portable to the Windows build gate). */
static char *dup_str(const char *s)
{
   size_t n = strlen(s);
   char *c = malloc(n + 1);
   if (c)
      memcpy(c, s, n + 1);
   return c;
}

static int ci_ncmp(const char *a, const char *b, size_t n)
{
   for (size_t i = 0; i < n; i++)
   {
      int ca = a_lower((unsigned char)a[i]);
      int cb = a_lower((unsigned char)b[i]);
      if (ca != cb)
         return ca - cb;
      if (ca == 0)
         return 0;
   }
   return 0;
}

/* ------------------------------------------------------------------ set mgmt */

void coord_set_init(coord_set_t *set)
{
   if (!set)
      return;
   set->items = NULL;
   set->count = 0;
   set->cap = 0;
}

void coord_set_free(coord_set_t *set)
{
   if (!set)
      return;
   for (size_t i = 0; i < set->count; i++)
   {
      free(set->items[i].value);
      free(set->items[i].label);
   }
   free(set->items);
   set->items = NULL;
   set->count = 0;
   set->cap = 0;
}

static int set_has_value(const coord_set_t *set, const char *value, coord_lane_t lane)
{
   for (size_t i = 0; i < set->count; i++)
      if (set->items[i].prov.lane == lane && strcmp(set->items[i].value, value) == 0)
         return 1;
   return 0;
}

/* Append a coordinate. Copies value/label; dedups by (lane,value) keeping the
 * earliest (lowest-offset) occurrence, which — combined with the left-to-right
 * scan — makes the set a deterministic function of the input bytes. */
static void set_add(coord_set_t *set, const char *value, size_t vlen, const char *label,
                    coord_kind_t kind, const coord_provenance_t *prov, size_t offset)
{
   if (!set || !value || vlen == 0)
      return;
   char *vcopy = malloc(vlen + 1);
   if (!vcopy)
      return;
   memcpy(vcopy, value, vlen);
   vcopy[vlen] = '\0';

   if (set_has_value(set, vcopy, prov->lane))
   {
      free(vcopy);
      return;
   }

   if (set->count == set->cap)
   {
      size_t ncap = set->cap ? set->cap * 2 : 8;
      coord_entry_t *ni = realloc(set->items, ncap * sizeof(*ni));
      if (!ni)
      {
         free(vcopy);
         return;
      }
      set->items = ni;
      set->cap = ncap;
   }

   coord_entry_t *e = &set->items[set->count];
   e->value = vcopy;
   e->label = dup_str(label ? label : "");
   if (!e->label)
   {
      free(vcopy);
      return;
   }
   e->kind = kind;
   e->prov = *prov;
   e->first_offset = offset;
   set->count++;
}

/* ------------------------------------------------------------------ matchers */

/* A char that may not be part of an identifier token (used for left-side word
 * boundaries when deciding where to start a match). */
static int is_ident_boundary(int c)
{
   return !(a_alnum(c) || c == '_' || c == '-' || c == '.' || c == '/' || c == ':');
}

/* Whether a token continues into more identifier text (used for right-side
 * boundary checks: a hex run followed by alnum/_/- is part of a longer token and
 * must not be conserved as a truncated prefix; ':' '.' '/' space terminate it). */
static int token_continues(int c)
{
   return a_alnum(c) || c == '_' || c == '-';
}

/* uuid: 8-4-4-4-12 hex with hyphens. Returns matched length at p, else 0. */
static size_t match_uuid(const char *p, size_t n)
{
   static const int groups[] = {8, 4, 4, 4, 12};
   size_t off = 0;
   for (int g = 0; g < 5; g++)
   {
      for (int i = 0; i < groups[g]; i++)
      {
         if (off >= n || !is_hex((unsigned char)p[off]))
            return 0;
         off++;
      }
      if (g < 4)
      {
         if (off >= n || p[off] != '-')
            return 0;
         off++;
      }
   }
   /* reject if this 36-char form is a prefix of a longer hex/identifier run */
   if (off < n && token_continues((unsigned char)p[off]))
      return 0;
   return off;
}

/* handle:<id> / memory:<id>. Writes label ("handle"/"memory") to lbl. */
static size_t match_handle(const char *p, size_t n, char *lbl, size_t lblsz)
{
   const char *prefixes[] = {"handle:", "memory:"};
   for (int k = 0; k < 2; k++)
   {
      size_t plen = strlen(prefixes[k]);
      if (n >= plen && strncmp(p, prefixes[k], plen) == 0)
      {
         size_t off = plen;
         while (off < n && (a_alnum((unsigned char)p[off]) || p[off] == '_' || p[off] == '-'))
            off++;
         if (off > plen)
         {
            snprintf(lbl, lblsz, "%.*s", (int)(plen - 1), prefixes[k]); /* drop ':' */
            return off;
         }
      }
   }
   return 0;
}

/* digit-bearing key=value. key -> lbl (lowercased); value span returned via
 * voff/vlen (offsets relative to p). Returns full matched length. */
static size_t match_kv(const char *p, size_t n, char *lbl, size_t lblsz, size_t *voff, size_t *vlen)
{
   size_t off = 0;
   if (off >= n || !(a_alpha((unsigned char)p[off]) || p[off] == '_'))
      return 0;
   size_t kstart = off;
   while (off < n && (a_alnum((unsigned char)p[off]) || p[off] == '_'))
      off++;
   size_t kend = off;
   if (off >= n || p[off] != '=')
      return 0;
   off++; /* '=' */
   size_t vstart = off;
   int has_digit = 0;
   while (off < n && (a_alnum((unsigned char)p[off]) || p[off] == '.' || p[off] == '-' ||
                      p[off] == ':' || p[off] == '_'))
   {
      if (a_digit((unsigned char)p[off]))
         has_digit = 1;
      off++;
   }
   if (off == vstart || !has_digit)
      return 0;
   size_t vend = off;
   while (vend > vstart && p[vend - 1] == '.') /* trim trailing punctuation dot */
      vend--;
   size_t klen = kend - kstart;
   if (klen >= lblsz)
      klen = lblsz - 1;
   for (size_t i = 0; i < klen; i++)
      lbl[i] = (char)a_lower((unsigned char)p[kstart + i]);
   lbl[klen] = '\0';
   *voff = vstart;
   *vlen = vend - vstart;
   return off;
}

/* path: starts with '/' or './' or '../', contains a '/', no spaces. */
static size_t match_path(const char *p, size_t n)
{
   if (n == 0)
      return 0;
   int leading = (p[0] == '/') || (n >= 2 && p[0] == '.' && p[1] == '/') ||
                 (n >= 3 && p[0] == '.' && p[1] == '.' && p[2] == '/');
   if (!leading)
      return 0;
   size_t off = 0;
   int slashes = 0;
   while (off < n && (a_alnum((unsigned char)p[off]) || p[off] == '/' || p[off] == '.' ||
                      p[off] == '_' || p[off] == '-'))
   {
      if (p[off] == '/')
         slashes++;
      off++;
   }
   if (slashes < 1 || off < 3)
      return 0;
   /* trim a trailing dot (sentence punctuation) */
   while (off > 0 && p[off - 1] == '.')
      off--;
   return off;
}

/* sha / long hex run: 7..64 hex chars with at least one a-f letter (so plain
 * decimal runs fall through to kv/ref), bounded by a non-identifier char on the
 * right. A run longer than 64 hex, or one that continues into other identifier
 * text, is rejected rather than conserved as a truncated prefix. */
static size_t match_sha(const char *p, size_t n)
{
   size_t off = 0;
   int alpha = 0;
   while (off < n && off <= 64 && is_hex((unsigned char)p[off]))
   {
      if (!a_digit((unsigned char)p[off]))
         alpha = 1;
      off++;
   }
   if (off < 7 || off > 64 || !alpha)
      return 0;
   /* reject only if the run continues into more identifier text; a sha may be
    * legitimately followed by ':' '.' '/' or whitespace */
   if (off < n && token_continues((unsigned char)p[off]))
      return 0;
   return off;
}

/* issue/PR ref: '#' followed by 1+ digits. */
static size_t match_ref(const char *p, size_t n)
{
   if (n < 2 || p[0] != '#' || !a_digit((unsigned char)p[1]))
      return 0;
   size_t off = 1;
   while (off < n && a_digit((unsigned char)p[off]))
      off++;
   return off;
}

size_t coord_closet_nominate(const char *raw, size_t len, const coord_provenance_t *prov,
                             coord_set_t *set)
{
   if (!raw || len == 0 || !set)
      return 0;

   coord_provenance_t pv = {COORD_LANE_AGENT, -1, -1, -1};
   if (prov)
      pv = *prov;

   size_t added_before = set->count;
   size_t i = 0;
   while (i < len)
   {
      /* only attempt a match at an identifier boundary so we never start
       * mid-token (keeps offsets and dedup stable) */
      int at_boundary = (i == 0) || is_ident_boundary((unsigned char)raw[i - 1]);
      if (!at_boundary)
      {
         i++;
         continue;
      }

      const char *p = raw + i;
      size_t rem = len - i;
      size_t m;
      char lbl[96];

      if ((m = match_uuid(p, rem)) > 0)
      {
         set_add(set, p, m, "uuid", COORD_KIND_UUID, &pv, i);
         i += m;
         continue;
      }
      if ((m = match_handle(p, rem, lbl, sizeof(lbl))) > 0)
      {
         set_add(set, p, m, lbl, COORD_KIND_HANDLE, &pv, i);
         i += m;
         continue;
      }
      size_t voff = 0, vlen = 0;
      if ((m = match_kv(p, rem, lbl, sizeof(lbl), &voff, &vlen)) > 0)
      {
         set_add(set, p + voff, vlen, lbl, COORD_KIND_KV, &pv, i);
         i += m;
         continue;
      }
      if ((m = match_path(p, rem)) > 0)
      {
         set_add(set, p, m, "path", COORD_KIND_PATH, &pv, i);
         i += m;
         continue;
      }
      if ((m = match_sha(p, rem)) > 0)
      {
         set_add(set, p, m, "sha", COORD_KIND_SHA, &pv, i);
         i += m;
         continue;
      }
      if ((m = match_ref(p, rem)) > 0)
      {
         set_add(set, p, m, "ref", COORD_KIND_REF, &pv, i);
         i += m;
         continue;
      }
      i++;
   }
   return set->count - added_before;
}

/* ------------------------------------------------------------------ secrets */

static int ci_substr(const char *hay, const char *needle)
{
   if (!hay || !needle || !needle[0])
      return 0;
   size_t nl = strlen(needle);
   for (const char *h = hay; *h; h++)
   {
      if (ci_ncmp(h, needle, nl) == 0)
         return 1;
   }
   return 0;
}

/* Sensitive key names: a value labelled like a credential is redacted even when
 * the value itself has no recognizable prefix (e.g. aws_secret_access_key=...). */
static int label_is_sensitive(const char *label)
{
   /* Substring match (e.g. aws_secret_access_key contains "secret"): err toward
    * over-redaction rather than leaking. Documented escape: not applicable —
    * a non-secret value labelled like a credential is rare and safe to redact. */
   static const char *names[] = {"secret",     "password",    "passwd",       "token",
                                 "apikey",     "api_key",     "api-key",      "authorization",
                                 "credential", "private_key", "session_token"};
   if (!label || !label[0])
      return 0;
   for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
      if (ci_substr(label, names[k]))
         return 1;
   return 0;
}

int coord_closet_is_secret(const char *value, const char *extra_denylist)
{
   if (!value || !value[0])
      return 0;

   /* token prefixes for common credential formats (case-sensitive — these are
    * exact issuer prefixes). sk- covers Anthropic's sk-ant- and OpenAI sk-. */
   static const char *prefixes[] = {
       "ghp_",  "gho_", "ghs_", "ghu_", "github_pat_", "sk-",  "xoxb-",  "xoxp-", "xoxa-",
       "xapp-", "AKIA", "ASIA", "AIza", "ya29.",       "npm_", "glpat-", "eyJ",   "-----BEGIN"};
   for (size_t k = 0; k < sizeof(prefixes) / sizeof(prefixes[0]); k++)
      if (strncmp(value, prefixes[k], strlen(prefixes[k])) == 0)
         return 1;

   /* credential-bearing paths */
   static const char *path_pats[] = {"id_rsa", "id_ed25519", "id_ecdsa",   ".pem",
                                     "/.ssh/", ".env",       "credentials"};
   for (size_t k = 0; k < sizeof(path_pats) / sizeof(path_pats[0]); k++)
      if (ci_substr(value, path_pats[k]))
         return 1;

   /* caller-configured deny-list: comma/space separated substrings */
   if (extra_denylist && extra_denylist[0])
   {
      const char *s = extra_denylist;
      while (*s)
      {
         while (*s == ',' || *s == ' ' || *s == '\t')
            s++;
         const char *start = s;
         while (*s && *s != ',' && *s != ' ' && *s != '\t')
            s++;
         size_t tlen = (size_t)(s - start);
         if (tlen > 0)
         {
            char tok[128];
            if (tlen >= sizeof(tok))
               tlen = sizeof(tok) - 1;
            memcpy(tok, start, tlen);
            tok[tlen] = '\0';
            if (ci_substr(value, tok))
               return 1;
         }
      }
   }
   return 0;
}

/* ------------------------------------------------------------------ render */

static int entry_cmp(const void *a, const void *b)
{
   const coord_entry_t *x = a, *y = b;
   if (x->prov.lane != y->prov.lane)
      return x->prov.lane < y->prov.lane ? -1 : 1;
   int lc = strcmp(x->label, y->label);
   if (lc != 0)
      return lc;
   if (x->first_offset != y->first_offset)
      return x->first_offset < y->first_offset ? -1 : 1;
   if (x->prov.turn_id != y->prov.turn_id)
      return x->prov.turn_id < y->prov.turn_id ? -1 : 1;
   if (x->prov.tool_call_id != y->prov.tool_call_id)
      return x->prov.tool_call_id < y->prov.tool_call_id ? -1 : 1;
   if (x->prov.result_index != y->prov.result_index)
      return x->prov.result_index < y->prov.result_index ? -1 : 1;
   return strcmp(x->value, y->value); /* total order */
}

/* Compose one entry's line into buf (buf may be NULL to size only). Returns the
 * full line length (excluding NUL), exactly like snprintf — so the caller can
 * size precisely and never truncate a long coordinate into a fixed buffer. */
static int closet_line(char *buf, size_t bufcap, const coord_entry_t *e, const char *denylist)
{
   const char *suffix = (e->prov.lane == COORD_LANE_USER) ? " (untrusted)" : "";
   int secret = coord_closet_is_secret(e->value, denylist) || label_is_sensitive(e->label);
   if (secret)
      return snprintf(buf, bufcap, "  [redacted:%s] \xE2\x9F\xA6%s\xE2\x9F\xA7%s\n", e->label,
                      e->label, suffix);
   return snprintf(buf, bufcap, "  %s \xE2\x9F\xA6%s\xE2\x9F\xA7%s\n", e->value, e->label, suffix);
}

char *coord_closet_render(const coord_set_t *set, const coord_closet_config_t *cfg, size_t raw_len,
                          coord_evict_t *why)
{
   if (why)
      *why = COORD_EVICT_NONE;
   if (!set || set->count == 0)
      return NULL;
   if (cfg && !cfg->enabled)
      return NULL;

   const char *denylist = cfg ? cfg->denylist : NULL;

   /* cap = min(budget, raw_len * ratio/100), via a saturating multiply so small
    * inputs are not collapsed to a 1-byte cap. */
   int budget =
       (cfg && cfg->budget_bytes > 0) ? cfg->budget_bytes : COORD_CLOSET_DEFAULT_BUDGET_BYTES;
   int ratio =
       (cfg && cfg->max_ratio_pct > 0) ? cfg->max_ratio_pct : COORD_CLOSET_DEFAULT_MAX_RATIO_PCT;
   size_t ratio_cap;
   if (ratio >= 100)
      ratio_cap = raw_len;
   else
      ratio_cap =
          (raw_len > (size_t)-1 / (size_t)ratio) ? (size_t)-1 : raw_len * (size_t)ratio / 100;
   size_t cap = (size_t)budget;
   if (ratio_cap < cap)
      cap = ratio_cap;

   static const char header[] = "Coordinate Closet (conserved from folded turns):\n";
   static const char header_partial[] =
       "Coordinate Closet (partial — some identifiers omitted, raise budget):\n";
   static const char divider[] = "  -- user-supplied (untrusted) --\n";
   static const char trunc_note[] = "  [...additional identifiers omitted...]\n";
   size_t divider_len = sizeof(divider) - 1;
   size_t note_len = sizeof(trunc_note) - 1;
   /* the partial header is the longest; budget against it so a late switch to
    * the partial header can never overflow the buffer */
   size_t header_budget = sizeof(header_partial) - 1;

   coord_entry_t *sorted = malloc(set->count * sizeof(*sorted));
   if (!sorted)
      return NULL;
   memcpy(sorted, set->items, set->count * sizeof(*sorted));
   qsort(sorted, set->count, sizeof(*sorted), entry_cmp);

   /* Pass 1: how many entries fit. Always reserve room for the truncation note
    * so it can be appended if we drop anything. */
   size_t total = header_budget;
   size_t fit = 0;
   int failed = 0;
   int user_div = 0;
   for (size_t i = 0; i < set->count; i++)
   {
      size_t add = 0;
      if (sorted[i].prov.lane == COORD_LANE_USER && !user_div)
         add += divider_len;
      int need = closet_line(NULL, 0, &sorted[i], denylist);
      if (need < 0)
      {
         failed = 1;
         break;
      }
      add += (size_t)need;
      if (total + add + note_len + 1 > cap)
      {
         failed = 1;
         break;
      }
      total += add;
      fit++;
      if (sorted[i].prov.lane == COORD_LANE_USER)
         user_div = 1;
   }

   if (fit == 0)
   {
      free(sorted);
      if (why)
         *why = COORD_EVICT_FAIL;
      return NULL;
   }

   const char *hdr = failed ? header_partial : header;
   size_t hdr_len = strlen(hdr);
   size_t bufsz = total + note_len + 1; /* generous: header_budget>=hdr_len */

   char *out = malloc(bufsz);
   if (!out)
   {
      free(sorted);
      return NULL;
   }
   memcpy(out, hdr, hdr_len);
   size_t pos = hdr_len;
   user_div = 0;
   for (size_t i = 0; i < fit; i++)
   {
      if (sorted[i].prov.lane == COORD_LANE_USER && !user_div)
      {
         memcpy(out + pos, divider, divider_len);
         pos += divider_len;
         user_div = 1;
      }
      pos += (size_t)closet_line(out + pos, bufsz - pos, &sorted[i], denylist);
   }
   if (failed)
   {
      memcpy(out + pos, trunc_note, note_len);
      pos += note_len;
   }
   out[pos] = '\0';

   free(sorted);
   if (failed && why)
      *why = COORD_EVICT_FAIL;
   return out;
}
