/* gen-memory-pii-fixtures.c: emit memory_pii_gate's own answers over a generated
 * corpus, as the fixtures the Go port is compared against.
 *
 * The Go port lives in server-go/modules/memory/pii.go. This is a gate that
 * decides whether a private fact reaches a prompt, so its expectations must come
 * from the C rather than from someone's reading of it. Regenerate after any
 * change to memory_pii_gate.c or rel_types.c:
 *
 *   cc -std=c11 -Wall -Wextra -Isrc -Isrc/headers -o /tmp/gen-pii \
 *      scripts/gen-memory-pii-fixtures.c \
 *      src/modules/memory/memory_pii_gate.c src/rel_types.c
 *   /tmp/gen-pii server-go/modules/memory/testdata/pii_turns.tsv \
 *                server-go/modules/memory/testdata/pii_sensitivity.tsv \
 *                server-go/modules/memory/testdata/pii_inject.tsv
 *
 * Confidences are carried as the raw bit pattern of the double, not as decimal
 * text: the floor case and the NaN case both turn on the exact value, and a
 * round-trip through decimal is the sort of approximation that would make the
 * comparison agree for the wrong reason. */
#include "modules/memory/memory_pii_gate.h"
#include "rel_types.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_escaped(FILE *f, const char *s)
{
   for (const char *p = s; *p; p++)
   {
      if (*p == '\n')
         fputs("\\n", f);
      else if (*p == '\t')
         fputs("\\t", f);
      else if (*p == '\\')
         fputs("\\\\", f);
      else
         fputc(*p, f);
   }
}

/* ---- turn texts ---------------------------------------------------------- */

/* Every cue the C scans for, plus near-misses of each, wrapped in frames: the
 * scan is a substring match, so where the cue sits in the sentence and what it
 * is embedded in are what the corpus has to vary. */
static const char *cue_words[] = {
    "address",       "phone",          "email",           "birthday",   "birth date",
    "date of birth", "born on",        "password",        "passphrase", "credential",
    "secret",        "api key",        "ssn",             "social security",
    "where do i live", "where i live", "my number",       "home ip",    "ip address",
    /* near-misses: a prefix, a plural, an embedding, and unrelated words. */
    "addres",        "phones",         "e-mail",          "birthdays",  "apikey",
    "credentials",   "secretary",      "lesson",          "ipa",        "telephone",
    "where do i work", "number",       "born",            "",           "hello there",
};

static const char *turn_frames[] = {
    "%C",
    "what is my %C",
    "What Is My %C?",
    "please tell me the %C now",
    "%C, and also my name",
    "I was talking about a %C yesterday",
    "no sensitive words here at all",
};

static void render(char *out, size_t cap, const char *frame, const char *cue)
{
   size_t o = 0;
   for (const char *p = frame; *p && o + 1 < cap; p++)
   {
      if (p[0] == '%' && p[1] == 'C')
      {
         size_t n = strlen(cue);
         if (o + n + 1 >= cap)
            n = cap - o - 1;
         memcpy(out + o, cue, n);
         o += n;
         p++;
         continue;
      }
      out[o++] = *p;
   }
   out[o] = '\0';
}

static void dump_turns(const char *path)
{
   FILE *f = fopen(path, "w");
   if (!f)
   {
      perror(path);
      exit(1);
   }
   char text[256];
   for (size_t fi = 0; fi < sizeof(turn_frames) / sizeof(turn_frames[0]); fi++)
      for (size_t ci = 0; ci < sizeof(cue_words) / sizeof(cue_words[0]); ci++)
      {
         render(text, sizeof(text), turn_frames[fi], cue_words[ci]);
         put_escaped(f, text);
         fprintf(f, "\t%d\n", memory_pii_turn_requests_sensitive(text));
      }
   fclose(f);
}

/* ---- relation labels ----------------------------------------------------- */

/* Names that are not seeded but must still be gated by shape, plus shapes that
 * must NOT be gated, plus labels that stress normalization on the way in. */
static const char *extra_labels[] = {
    "password",      "user_password",  "db passwd",       "passphrase",     "SECRET",
    "api_key",       "apiKey",         "access_key",      "private_key",    "privkey",
    "ssh_key",       "auth_token",     "credential",      "credentials",
    "ssn",           "social_security", "passport",       "credit_card",    "creditcard",
    "card_number",   "cvv",            "bank_account",    "account_number", "routing_number",
    "tax_id",        "national_id",    "drivers_license", "license_number", "phone",
    "phone_number",  "email",          "work email",      "date_of_birth",  "birthdate",
    "dob",           "home_address",   "street_address",
    /* open by default: nothing in the name denotes a credential or identifier. */
    "favorite_color", "name", "city", "hobby", "pet", "employer", "nickname",
    /* normalization stress: these reach the tokens only after normalizing. */
    "Home Address", "home-address", "homeAddress", "HOME_ADDRESS", "  api key  ",
    /* labels that normalize to nothing, or nearly. */
    "", "___", "---", "!!!", " ", "_a_",
    /* a token embedded in a longer word: substring match, so these still gate. */
    "tokenizer", "secretary_of_state", "phonetics", "dobro",
    /* Names carrying a token from BOTH lists. The secret list is checked first,
     * so these must come back SECRET -- the stricter tier wins. Without them the
     * corpus cannot tell the two lists' order apart, and reversing it (which
     * would let "email_password" be pre-injected whenever the turn mentions an
     * email) would look correct. */
    "email_password", "phone_secret", "password_email", "ssn_api_key",
    "bank_account_passphrase", "dob_token", "credit_card_credential",
};

static void dump_sensitivity(const char *path)
{
   FILE *f = fopen(path, "w");
   if (!f)
   {
      perror(path);
      exit(1);
   }
   /* Every seed row first, asked of the seed table itself rather than listed. */
   for (int i = 0; i < rel_types_seed_count(); i++)
   {
      const char *name = rel_types_seed_at(i)->rel_type;
      put_escaped(f, name);
      fprintf(f, "\t%d\n", (int)memory_pii_rel_sensitivity(name));
      /* the same row reached through a label that only matches after
       * normalization, so the lookup path is exercised too. */
      char mangled[128];
      snprintf(mangled, sizeof(mangled), "  %s  ", name);
      for (char *p = mangled; *p; p++)
         if (*p == '_')
            *p = ' ';
      put_escaped(f, mangled);
      fprintf(f, "\t%d\n", (int)memory_pii_rel_sensitivity(mangled));
   }
   for (size_t i = 0; i < sizeof(extra_labels) / sizeof(extra_labels[0]); i++)
   {
      put_escaped(f, extra_labels[i]);
      fprintf(f, "\t%d\n", (int)memory_pii_rel_sensitivity(extra_labels[i]));
   }
   fclose(f);
}

/* ---- inject decision ----------------------------------------------------- */

static uint64_t bits_of(double v)
{
   uint64_t bits;
   memcpy(&bits, &v, sizeof(bits));
   return bits;
}

static void dump_inject(const char *path)
{
   FILE *f = fopen(path, "w");
   if (!f)
   {
      perror(path);
      exit(1);
   }
   double zero = 0.0;
   double confidences[] = {
       0.0,
       0.1,
       PII_GATE_CONFIDENCE_FLOOR - 0.0000001,
       PII_GATE_CONFIDENCE_FLOOR, /* exactly at the floor: passes */
       PII_GATE_CONFIDENCE_FLOOR + 0.0000001,
       0.5,
       1.0,
       2.0,
       -1.0,
       zero / zero,  /* NaN: must fail closed, not slip past a '<' */
       1.0 / zero,   /* +inf */
       -1.0 / zero,  /* -inf */
   };
   /* Every tier, plus a value outside the enum: a tier the module does not
    * recognize must not open the gate. */
   int tiers[] = {SENS_NORMAL, SENS_PII, SENS_SECRET, 99};
   for (size_t t = 0; t < sizeof(tiers) / sizeof(tiers[0]); t++)
      for (size_t c = 0; c < sizeof(confidences) / sizeof(confidences[0]); c++)
         for (int asked = 0; asked <= 1; asked++)
            fprintf(f, "%d\t%016llx\t%d\t%d\n", tiers[t],
                    (unsigned long long)bits_of(confidences[c]), asked,
                    memory_pii_should_inject((rel_sensitivity_t)tiers[t], confidences[c], asked));
   fclose(f);
}

int main(int argc, char **argv)
{
   if (argc != 4)
   {
      fprintf(stderr, "usage: %s <pii_turns.tsv> <pii_sensitivity.tsv> <pii_inject.tsv>\n",
              argv[0]);
      return 2;
   }
   dump_turns(argv[1]);
   dump_sensitivity(argv[2]);
   dump_inject(argv[3]);
   return 0;
}
