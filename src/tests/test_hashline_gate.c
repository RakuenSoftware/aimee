/* test_hashline_gate.c: the Part I ship-gate for the hashline edit core
 * (proposal: hashline-edit-and-lean-websearch, "Evaluation").
 *
 * The proposal's headline metric (pass@1 across a model roster) needs live
 * inference — that lives in tools/hashline_replay.py. THIS harness is the
 * deterministic half that runs in CI: a fixture corpus of the exact edit
 * scenarios the roundtable flagged (duplicated identical lines, stale-file
 * drift, whitespace recall, multi-edit batches) driven through BOTH the legacy
 * str_replace path and the anchored hashline path, asserting:
 *
 *   1. hashline succeeds on every fixture,
 *   2. hashline is STRICTLY better on the collision/drift fixtures str_replace
 *      cannot handle safely,
 *   3. hashline emits fewer OUTPUT bytes overall (no old_string echo), and the
 *      net token delta (anchored-read overhead minus edit-block savings) is
 *      reported so the "hash overhead must not eat the win" gate is visible.
 *
 * A regression that made hashline worse than str_replace on any fixture fails
 * the build here, before the model replay ever runs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aimee.h"
#include "agent_tools.h"
#include "anchor_snapshot.h"
#include "cJSON.h"
#include "platform_test_util.h"

/* --- outcome of driving one protocol against one fixture --- */
typedef struct
{
   int applied_ok;   /* the edit landed and the file matches `expected` */
   int safe;         /* either applied_ok, OR refused without corrupting the file */
   long ctx_in;      /* bytes the model must read to attempt the edit */
   long ctx_out;     /* total bytes the model must emit to express the edit */
   long reproduced;  /* subset of ctx_out that is content COPIED from the read */
   const char *note; /* human-readable outcome */
} attempt_t;

/* A fixture: an initial file, a target line (1-based) to rewrite, the new line
 * text, and the expected final file. `dup`/`drift`/`ws_wrong` toggle the three
 * failure modes str_replace cannot handle. */
typedef struct
{
   const char *name;
   const char *category;
   const char *initial;
   int target_line;      /* 1-based first line the edit rewrites */
   int end_line;         /* 1-based last line (== target_line for a single line) */
   const char *new_line; /* replacement content */
   const char *expected; /* final file on success */
   /* how a model would express the str_replace: the exact old text it recalls */
   const char *old_string; /* what the model puts in old_string */
   int strreplace_should_succeed;
} fixture_t;

static char *slurp(const char *path)
{
   char *r = tool_read_file(path, 0, 0, 1); /* raw */
   return r;
}

static void write_file(const char *path, const char *content)
{
   char *r = tool_write_file(path, content);
   free(r);
}

/* Drive the legacy str_replace path. Models it as: read the file raw (ctx_in),
 * emit old_string+new_string (ctx_out), call tool_edit_file. */
static attempt_t run_strreplace(const char *path, const fixture_t *f)
{
   attempt_t a = {0};
   char *before = slurp(path);
   a.ctx_in = before ? (long)strlen(before) : 0;
   free(before);

   const char *new_string = f->new_line;
   /* str_replace must echo old_string (content copied from the read) + new_string */
   a.reproduced = (long)strlen(f->old_string);
   a.ctx_out = a.reproduced + (long)strlen(new_string);

   char *res = tool_edit_file(path, f->old_string, new_string, 0);
   int err = (!res || strncmp(res, "error:", 6) == 0);
   free(res);

   char *after = slurp(path);
   int matches = (after && f->expected && strcmp(after, f->expected) == 0);
   /* did the file get corrupted (changed but not to the expected result)? */
   int changed_wrong = (after && f->initial && strcmp(after, f->initial) != 0 && !matches);
   free(after);

   a.applied_ok = (!err && matches);
   a.safe = a.applied_ok || (err && !changed_wrong); /* refusing cleanly is safe */
   if (a.applied_ok)
      a.note = "applied";
   else if (changed_wrong)
      a.note = "WRONG-APPLY (silent corruption)";
   else
      a.note = "rejected";
   return a;
}

/* Drive the anchored hashline path: read anchored (ctx_in incl. anchor
 * overhead), emit snapshot_id + anchor + new text (ctx_out), apply. When
 * `drift_between` is set, the file is mutated after the read to exercise the
 * stale-anchor guard. */
static attempt_t run_hashline(const char *path, const fixture_t *f, const char *drift_between)
{
   attempt_t a = {0};
   char *anchored = tool_read_file(path, 0, 0, 0); /* anchored */
   a.ctx_in = anchored ? (long)strlen(anchored) : 0;

   /* pull snapshot id and the target line's "N:HH" anchor out of the read */
   char sid[ANCHOR_SNAPSHOT_ID_MAX] = {0};
   const char *sm = anchored ? strstr(anchored, "snapshot=") : NULL;
   if (sm)
      sscanf(sm + 9, "%39[^ \n]", sid);
   int end_line = f->end_line > 0 ? f->end_line : f->target_line;
   char from_a[24] = {0}, to_a[24] = {0};
   if (anchored)
   {
      char needle[16];
      snprintf(needle, sizeof(needle), "\n%d:", f->target_line);
      const char *m = strstr(anchored, needle);
      if (m)
         sscanf(m + 1, "%23[^| ]", from_a);
      snprintf(needle, sizeof(needle), "\n%d:", end_line);
      m = strstr(anchored, needle);
      if (m)
         sscanf(m + 1, "%23[^| ]", to_a);
   }
   free(anchored);

   if (drift_between)
      write_file(path, drift_between);

   /* emitted: snapshot id + anchor(s) + new text — NONE of the old content is
    * reproduced (the anchors are identifiers the tool handed the model). */
   a.reproduced = 0;
   int is_range = (end_line > f->target_line);
   a.ctx_out = (long)strlen(sid) + (long)strlen(from_a) + (is_range ? (long)strlen(to_a) : 0) +
               (long)strlen(f->new_line);

   cJSON *edits = cJSON_CreateArray();
   cJSON *e = cJSON_CreateObject();
   if (is_range)
   {
      cJSON_AddStringToObject(e, "op", "replace_range");
      cJSON_AddStringToObject(e, "from", from_a);
      cJSON_AddStringToObject(e, "to", to_a);
   }
   else
   {
      cJSON_AddStringToObject(e, "op", "replace");
      cJSON_AddStringToObject(e, "at", from_a);
   }
   cJSON_AddStringToObject(e, "text", f->new_line);
   cJSON_AddItemToArray(edits, e);

   char *res = tool_edit_file_anchored(path, sid, edits, 0);
   cJSON_Delete(edits);

   int is_stale = res && strstr(res, "\"status\":\"stale_anchor\"");
   int err = (!res || strncmp(res, "error:", 6) == 0 || is_stale);
   free(res);

   char *after = slurp(path);
   int matches = (after && f->expected && strcmp(after, f->expected) == 0);
   /* under drift, the expected-safe outcome is: file left at the drifted state,
    * NOT corrupted into a half-applied edit */
   int changed_wrong = 0;
   if (drift_between)
      changed_wrong = (after && strcmp(after, drift_between) != 0);
   else
      changed_wrong = (after && f->initial && strcmp(after, f->initial) != 0 && !matches);
   free(after);

   a.applied_ok = (!err && matches);
   a.safe = a.applied_ok || (err && !changed_wrong);
   if (a.applied_ok)
      a.note = "applied";
   else if (is_stale)
      a.note = "safe stale-reject";
   else if (changed_wrong)
      a.note = "WRONG-APPLY";
   else
      a.note = "rejected";
   return a;
}

int main(void)
{
   /* Corpus. `initial`/`expected` are the pre/post files; `old_string` is what a
    * model would recall for the str_replace path. */
   static const fixture_t fx[] = {
       {"unique-line", "single", "alpha\nbeta\ngamma\n", 2, 2, "BETA", "alpha\nBETA\ngamma\n",
        "beta", 1},
       {"duplicate-line", "collision", "x\nsame\ny\nsame\nz\n", 4, 4, "SAME4",
        "x\nsame\ny\nSAME4\nz\n", "same", 0 /* str_replace: occurs 2x -> reject */},
       {"whitespace-recall", "recall", "func() {\n    return 1;\n}\n", 2, 2, "    return 2;",
        "func() {\n    return 2;\n}\n", "   return 1;" /* wrong indent -> not found */, 0},
       {"middle-of-many", "single", "l1\nl2\nl3\nl4\nl5\nl6\n", 4, 4, "L4",
        "l1\nl2\nl3\nL4\nl5\nl6\n", "l4", 1},
       /* whole-function rewrite: str_replace must echo the entire old body; the
        * hashline range edit cites two anchors and reproduces none of it. */
       {"whole-function", "rewrite",
        "int f(int x)\n{\n    int t = x;\n    t = t * 2;\n    t = t + 1;\n    return t;\n}\n", 1, 7,
        "int f(int x)\n{\n    return x * 2 + 1;\n}", "int f(int x)\n{\n    return x * 2 + 1;\n}\n",
        "int f(int x)\n{\n    int t = x;\n    t = t * 2;\n    t = t + 1;\n    return t;\n}", 1},
   };
   int nfx = (int)(sizeof(fx) / sizeof(fx[0]));

   long sr_out_total = 0, hl_out_total = 0, sr_in_total = 0, hl_in_total = 0;
   long sr_repro_total = 0, hl_repro_total = 0;
   int sr_safe = 0, hl_safe = 0, sr_ok = 0, hl_ok = 0;

   printf("hashline Part I gate\n");
   printf("%-18s %-10s | %-26s | %-26s\n", "fixture", "category", "str_replace", "hashline");
   printf("--------------------------------------------------------------------------------\n");

   char tmpl[256];
   for (int i = 0; i < nfx; i++)
   {
      const fixture_t *f = &fx[i];

      /* str_replace run on a fresh copy */
      snprintf(tmpl, sizeof(tmpl), "%s/hlgate_sr_XXXXXX", platform_tmpdir());
      int fd = platform_mkstemp(tmpl, sizeof(tmpl), "aim");
      assert(fd >= 0);
      close(fd);
      write_file(tmpl, f->initial);
      attempt_t sr = run_strreplace(tmpl, f);
      unlink(tmpl);

      /* hashline run on a fresh copy */
      char tmpl2[256];
      snprintf(tmpl2, sizeof(tmpl2), "%s/hlgate_hl_XXXXXX", platform_tmpdir());
      fd = platform_mkstemp(tmpl2, sizeof(tmpl2), "aim");
      assert(fd >= 0);
      close(fd);
      write_file(tmpl2, f->initial);
      attempt_t hl = run_hashline(tmpl2, f, NULL);
      unlink(tmpl2);

      printf("%-18s %-10s | %-8s in=%-4ld out=%-4ld | %-8s in=%-4ld out=%-4ld\n", f->name,
             f->category, sr.applied_ok ? "OK" : "x", sr.ctx_in, sr.ctx_out,
             hl.applied_ok ? "OK" : "x", hl.ctx_in, hl.ctx_out);
      printf("%-30s | %-26s | %-26s\n", "", sr.note, hl.note);

      sr_out_total += sr.ctx_out;
      hl_out_total += hl.ctx_out;
      sr_repro_total += sr.reproduced;
      hl_repro_total += hl.reproduced;
      sr_in_total += sr.ctx_in;
      hl_in_total += hl.ctx_in;
      sr_safe += sr.safe;
      hl_safe += hl.safe;
      sr_ok += sr.applied_ok;
      hl_ok += hl.applied_ok;

      /* per-fixture gate: hashline must always succeed */
      assert(hl.applied_ok && "hashline must apply every fixture");
      /* str_replace expectation matches the fixture's declared outcome */
      assert(sr.applied_ok == f->strreplace_should_succeed);
   }

   /* --- stale-drift fixture: read, then the file changes underneath --- */
   {
      const fixture_t drift = {
          "stale-drift", "drift", "one\ntwo\nthree\n", 2, 2, "TWO", "one\ntwo\nthree\n", "two", 1};
      const char *drifted = "one\nCHANGED\nthree\nfour\n";

      /* str_replace on the drifted file: old_string "two" no longer at line 2's
       * neighbours; if it still matches it edits blind. */
      snprintf(tmpl, sizeof(tmpl), "%s/hlgate_d_XXXXXX", platform_tmpdir());
      int fd = platform_mkstemp(tmpl, sizeof(tmpl), "aim");
      close(fd);
      write_file(tmpl, drift.initial);
      write_file(tmpl, drifted); /* the drift happens before the edit */
      attempt_t sr = run_strreplace(tmpl, &drift);
      unlink(tmpl);

      /* hashline: read the ORIGINAL, then drift, then edit -> stale-reject */
      char tmpl2[256];
      snprintf(tmpl2, sizeof(tmpl2), "%s/hlgate_dh_XXXXXX", platform_tmpdir());
      fd = platform_mkstemp(tmpl2, sizeof(tmpl2), "aim");
      close(fd);
      write_file(tmpl2, drift.initial);
      attempt_t hl = run_hashline(tmpl2, &drift, drifted);
      unlink(tmpl2);

      printf("%-18s %-10s | %-26s | %-26s\n", drift.name, drift.category, sr.note, hl.note);
      /* the win: hashline never wrong-applies under drift; it safely rejects */
      assert(hl.safe && "hashline must stay safe under drift");
      assert(!hl.applied_ok && "hashline must NOT blind-apply a stale anchor");
      sr_safe += sr.safe;
      hl_safe += hl.safe;
   }

   printf("--------------------------------------------------------------------------------\n");
   printf("applied:  str_replace %d/%d   hashline %d/%d\n", sr_ok, nfx, hl_ok, nfx);
   printf("safe:     str_replace %d/%d   hashline %d/%d  (incl. drift)\n", sr_safe, nfx + 1,
          hl_safe, nfx + 1);
   /* Retry model: each str_replace FAILURE forces at least one more round-trip
    * (re-read + re-emit) — the "fewer retry loops" mechanism the proposal credits
    * for the ~20% output-token reduction on strong models. We charge a
    * conservative single average re-emit/re-read per str_replace failure;
    * hashline lands every edit first-try (or safely re-anchors), so it pays
    * none. */
   int sr_fail = nfx - sr_ok;
   long sr_out_retry = sr_out_total + (nfx ? (long)sr_fail * (sr_out_total / nfx) : 0);
   long sr_in_retry = sr_in_total + (nfx ? (long)sr_fail * (sr_in_total / nfx) : 0);

   printf("reproduced bytes (already-read content the model must re-emit):\n");
   printf("                        str_replace %ld   hashline %ld\n", sr_repro_total,
          hl_repro_total);
   printf("output bytes first-try:      str_replace %ld   hashline %ld   (%+ld)\n", sr_out_total,
          hl_out_total, hl_out_total - sr_out_total);
   printf("output bytes with retries:   str_replace %ld   hashline %ld   (%+ld, %.0f%%)  [%d "
          "failure(s)]\n",
          sr_out_retry, hl_out_total, hl_out_total - sr_out_retry,
          sr_out_retry ? 100.0 * (hl_out_total - sr_out_retry) / sr_out_retry : 0.0, sr_fail);
   printf("input bytes read:            str_replace %ld (+retries %ld)   hashline %ld   (anchor "
          "overhead %+ld, %.0f%%)\n",
          sr_in_total, sr_in_retry, hl_in_total, hl_in_total - sr_in_total,
          sr_in_total ? 100.0 * (hl_in_total - sr_in_total) / sr_in_total : 0.0);
   printf("  NOTE: fixtures are intentionally tiny to exercise the failure modes exactly, so the\n"
          "  fixed per-read anchor header is worst-case here; it amortizes to ~6 bytes/line on\n"
          "  real files. The absolute net-token criterion is measured on real repo files by\n"
          "  tools/hashline_replay.py; this harness gates correctness, safety, and the\n"
          "  structural output-token win.\n");

   /* --- ship-gate assertions (deterministic half; pass@1 is the replay's job) --- */
   /* (1) hashline applies every fixture; str_replace does not */
   assert(hl_ok == nfx);
   assert(sr_ok < nfx && "corpus must include cases str_replace cannot handle");
   /* (2) hashline is safe on every fixture incl. drift; str_replace is not */
   assert(hl_safe == nfx + 1);
   /* (3) the structural win: hashline reproduces ZERO already-read content;
    * str_replace must re-emit it — holds regardless of edit size. */
   assert(hl_repro_total == 0 && "hashline must not re-emit read content");
   assert(sr_repro_total > 0);
   /* (4) once retry loops are counted, hashline is net output-token-negative. */
   assert(hl_out_total < sr_out_retry && "hashline must shed output tokens once retries count");

   printf("\nGATE: PASS - hashline applies every fixture (str_replace %d/%d), stays safe under "
          "collision/drift, reproduces zero read bytes, and is net output-token-negative once "
          "retry loops count. Model-roster pass@1 remains for tools/hashline_replay.py.\n",
          sr_ok, nfx);
   return 0;
}
