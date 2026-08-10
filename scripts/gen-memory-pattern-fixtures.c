/* gen-memory-pattern-fixtures.c: emit memory_extract_patterns' own output over a
 * generated corpus, as the fixtures the Go port is compared against.
 *
 * The expectations are never hand-written. The Go port of this logic lives in
 * server-go/modules/memory/extract.go, and what makes its conformance test worth
 * anything is that the answers come from the C, not from someone's reading of
 * it. Regenerate after any change to memory_extract_patterns.c or rel_types.c:
 *
 *   cc -std=c11 -Wall -Wextra -Isrc -Isrc/headers -o /tmp/gen-patterns \
 *      scripts/gen-memory-pattern-fixtures.c \
 *      src/modules/memory/memory_extract_patterns.c src/rel_types.c
 *   /tmp/gen-patterns server-go/modules/memory/testdata/value_kinds.tsv \
 *                     server-go/modules/memory/testdata/extract_corpus.tsv
 *
 * The text corpus uses the bounds the production caller gives the C
 * (db2_fact_ingest_text: 16 triples; db2_typed_fact_ingress: a 128-byte
 * attribute buffer), because those bounds are visible in the output. */
#include "modules/memory/memory_extract_patterns.h"
#include "modules/memory/memory_ontology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Write s with tabs/newlines escaped so one case stays one TSV row. */
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

/* ---- value-shape corpus -------------------------------------------------- */

/* Seeds of each shape, then mutations of every seed, so the corpus probes the
 * boundaries of each classifier rather than only its happy path. */
static const char *value_seeds[] = {
    "192.168.1.254", "10.0.0.1", "0.0.0.0", "255.255.255.255", "256.0.0.1", "1.2.3", "1.2.3.4.5",
    "01.02.03.04", "1234.1.1.1", ".1.2.3", "1..2.3",
    "aa:bb:cc:dd:ee:ff", "AA-BB-CC-DD-EE-FF", "aa:bb:cc:dd:ee", "aa:bb:cc:dd:ee:ff:00",
    "aa-bb:cc-dd:ee-ff", "gg:bb:cc:dd:ee:ff", "aabbccddeeff",
    "fe80::1", "::1", "::", "2001:db8:0:0:0:0:0:1", "2001:db8:0:0:0:0:1", "2001:0db8:85a3::8a2e",
    "12:34:56:78:9a:bc", "12345:1:1:1:1:1:1:1", "1:2:3:4:5:6:7:8", ":",
    "theo@example.com", "a.b+c@sub.example.co", "a@b", "a@b.", "@example.com", "a@@b.com",
    "a b@c.com", "not-an-email", "user_name@example.com",
    "2026-06-13", "2026-13-01", "2026-06-32", "2026-00-01", "2026-01-00", "06/13/2026",
    "2026-6-13", "20260613",
    "hello", "", "user", "Theo", "0", "..", "-",
};

static void dump_values(const char *path)
{
   FILE *f = fopen(path, "w");
   if (!f)
   {
      perror(path);
      exit(1);
   }
   for (size_t i = 0; i < sizeof(value_seeds) / sizeof(value_seeds[0]); i++)
   {
      const char *seed = value_seeds[i];
      /* the seed plus mutations: drop the last byte, upper-case it, and append a
       * trailing dot -- each of which is a realistic way a value arrives. */
      char mutated[3][128];
      snprintf(mutated[0], sizeof(mutated[0]), "%s", seed);
      if (mutated[0][0])
         mutated[0][strlen(mutated[0]) - 1] = '\0';
      snprintf(mutated[1], sizeof(mutated[1]), "%s", seed);
      for (char *p = mutated[1]; *p; p++)
         if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 'a' + 'A');
      snprintf(mutated[2], sizeof(mutated[2]), "%s.", seed);

      const char *all[4] = {seed, mutated[0], mutated[1], mutated[2]};
      for (int m = 0; m < 4; m++)
      {
         pattern_value_kind_t k = memory_pattern_classify_value(all[m]);
         put_escaped(f, all[m]);
         fprintf(f, "\t%d\t%d\n", (int)k, (int)memory_pattern_value_node_kind(k));
      }
   }
   fclose(f);
}

/* ---- text corpus --------------------------------------------------------- */

/* Sentence frames crossed with attributes and values: the extractor's job is to
 * find the template inside surrounding text, so the frame varies as well as the
 * fact. %A is the attribute, %V the value. */
static const char *frames[] = {
    "my %A is %V",
    "my %A is %V.",
    "My %A is %V, thanks",
    "hi there, my %A is %V and that's it",
    "my %A is %V\nmy other thing",
    "the army is big but my %A is %V",
    "MY %A IS %V",
    "my %A is %V. my city is Berlin.",
    "forget my %A",
    "forget that my %A is %V",
    "please disregard, my %A is %V",
    "my %A is",
    "my is %V",
    "my %A is .",
    "%A is %V",
    "  my   %A   is   %V  ",
};

static const char *attrs[] = {
    "email", "home ip", "name", "favorite color", "HTTPServer", "work-phone", "x",
    "a very long attribute phrase that keeps going", "",
};

static const char *values[] = {
    "theo@example.com", "192.168.1.254", "Theo", "aa:bb:cc:dd:ee:ff", "2026-06-13", "fe80::1",
    "a thing with spaces", "",
};

/* Substitute %A/%V into a frame. */
static void render(char *out, size_t cap, const char *frame, const char *attr, const char *value)
{
   size_t o = 0;
   for (const char *p = frame; *p && o + 1 < cap; p++)
   {
      const char *sub = NULL;
      if (p[0] == '%' && p[1] == 'A')
         sub = attr;
      else if (p[0] == '%' && p[1] == 'V')
         sub = value;
      if (sub)
      {
         size_t n = strlen(sub);
         if (o + n + 1 >= cap)
            n = cap - o - 1;
         memcpy(out + o, sub, n);
         o += n;
         p++;
         continue;
      }
      out[o++] = *p;
   }
   out[o] = '\0';
}

/* Standalone texts that exercise cues and boundaries no frame reaches. */
static const char *extra_texts[] = {
    "forget that I have a dog",
    "Actually, that's wrong",
    "thats wrong",
    "that is wrong",
    "I NO LONGER work there",
    "scratch that",
    "ignore that",
    "never mind",
    "nevermind",
    "please disregard the last thing",
    "delete that record",
    "delete the record",
    "forget about it",
    "forget what I said",
    "don't forget to call mom",
    "the server crashed last night",
    "the army is large",
    "",
    "my",
    "my ",
    "army is here",
    "my email is a@b.com and my ip is 10.0.0.1",
    "my favorite color please",
    "forget my favorite color please",
    "my email was theo@example.com",
    /* over the caller's 128-byte attribute and value buffers: one word, so the
     * ~3-word cap does not end it first and the truncation itself is exercised. */
    "my aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa is x",
    "my x is bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    /* more templates than the caller's array holds: extraction must stop at max. */
    "my a is 1. my b is 2. my c is 3. my d is 4. my e is 5. my f is 6. my g is 7. my h is 8. "
    "my i is 9. my j is 10. my k is 11. my l is 12. my m is 13. my n is 14. my o is 15. "
    "my p is 16. my q is 17. my r is 18.",
};

static void dump_texts(const char *path)
{
   FILE *f = fopen(path, "w");
   if (!f)
   {
      perror(path);
      exit(1);
   }
   char text[512];
   size_t nframes = sizeof(frames) / sizeof(frames[0]);
   size_t nattrs = sizeof(attrs) / sizeof(attrs[0]);
   size_t nvalues = sizeof(values) / sizeof(values[0]);
   size_t nextra = sizeof(extra_texts) / sizeof(extra_texts[0]);
   size_t total = nframes * nattrs * nvalues + nextra;

   for (size_t i = 0; i < total; i++)
   {
      if (i < nframes * nattrs * nvalues)
      {
         size_t fi = i / (nattrs * nvalues);
         size_t ai = (i / nvalues) % nattrs;
         size_t vi = i % nvalues;
         render(text, sizeof(text), frames[fi], attrs[ai], values[vi]);
      }
      else
      {
         snprintf(text, sizeof(text), "%s", extra_texts[i - nframes * nattrs * nvalues]);
      }

      char attr[128] = {0};
      int has_attr = memory_pattern_possessive_attr(text, attr, sizeof(attr));
      int retraction = memory_pattern_is_retraction(text);
      pattern_triple_t triples[16];
      int n = memory_extract_patterns(text, triples, 16);

      put_escaped(f, text);
      fprintf(f, "\t%d\t%d\t", retraction, has_attr);
      put_escaped(f, attr);
      fprintf(f, "\t%d", n);
      for (int t = 0; t < n; t++)
      {
         fputc('\t', f);
         put_escaped(f, triples[t].subject);
         fputc('\t', f);
         put_escaped(f, triples[t].rel_type);
         fputc('\t', f);
         put_escaped(f, triples[t].object);
         fprintf(f, "\t%d\t%d", (int)triples[t].subject_kind, (int)triples[t].object_kind);
      }
      fputc('\n', f);
   }
   fclose(f);
}

int main(int argc, char **argv)
{
   if (argc != 3)
   {
      fprintf(stderr, "usage: %s <value_kinds.tsv> <extract_corpus.tsv>\n", argv[0]);
      return 2;
   }
   dump_values(argv[1]);
   dump_texts(argv[2]);
   return 0;
}
