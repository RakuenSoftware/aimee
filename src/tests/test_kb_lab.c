/* test_kb_lab.c: Unit tests for the ingest lab (kb_lab_run). */
#include "../headers/kb_lab.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define TEST(name) static void name(void)
#define RUN(name)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      name();                                                                                      \
      printf("  PASS  " #name "\n");                                                               \
   } while (0)

/* Write content to a tmp file, return path (caller must unlink). */
static char *write_tmp(const char *content)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/kb_lab_test_XXXXXX.md", platform_tmpdir());
   char *path = strdup(tmpl);
   int fd = mkstemps(path, 3);
   assert(fd >= 0);
   size_t len = strlen(content);
   assert(write(fd, content, len) == (ssize_t)len);
   close(fd);
   return path;
}

/* ── basic markdown ───────────────────────────────────────────────────── */

TEST(test_markdown_single_section)
{
   char *path = write_tmp("# Title\n\nSome content here about things.\n");
   kb_lab_report_t r;
   int rc = kb_lab_run(path, &r);
   assert(rc == 0);
   assert(r.chunk_count >= 1);
   assert(r.total_tokens > 0);
   assert(strcmp(r.doc_kind, "markdown") == 0);
   unlink(path);
   free(path);
}

TEST(test_markdown_multiple_sections)
{
   char *path = write_tmp("# Introduction\n\nFirst section content.\n\n"
                          "## Background\n\nSecond section with more text.\n\n"
                          "### Details\n\nThird section.\n");
   kb_lab_report_t r;
   int rc = kb_lab_run(path, &r);
   assert(rc == 0);
   assert(r.chunk_count >= 2);
   unlink(path);
   free(path);
}

TEST(test_stage_ready)
{
   char *path = write_tmp("# Proposal\n\nThis is a well-structured document.\n\n"
                          "## Approach\n\nThe approach is clear.\n\n"
                          "## Changes\n\nMinimal changes.\n");
   kb_lab_report_t r;
   kb_lab_run(path, &r);
   assert(r.stage == KB_LAB_STAGE_READY);
   unlink(path);
   free(path);
}

TEST(test_flat_text_signal)
{
   char *path = write_tmp("This is a flat text file with no headings.\nJust plain content.\n");
   kb_lab_report_t r;
   kb_lab_run(path, &r);
   int found = 0;
   for (int i = 0; i < r.signal_count; i++)
      if (r.signals[i].kind == KB_LAB_FLAG_FLAT_TEXT)
         found = 1;
   assert(found);
   unlink(path);
   free(path);
}

TEST(test_heading_skip_signal)
{
   char *path = write_tmp("# Title\n\nContent.\n\n### Skipped h2\n\nMore content.\n");
   kb_lab_report_t r;
   kb_lab_run(path, &r);
   int found = 0;
   for (int i = 0; i < r.signal_count; i++)
      if (r.signals[i].kind == KB_LAB_FLAG_HEADING_SKIP)
         found = 1;
   assert(found);
   assert(r.stage >= KB_LAB_STAGE_REVIEW_NEEDED);
   unlink(path);
   free(path);
}

TEST(test_empty_file_reject)
{
   char *path = write_tmp("   \n\n  \n");
   kb_lab_report_t r;
   kb_lab_run(path, &r);
   assert(r.stage == KB_LAB_STAGE_REJECT);
   int found = 0;
   for (int i = 0; i < r.signal_count; i++)
      if (r.signals[i].kind == KB_LAB_FLAG_EMPTY_FILE)
         found = 1;
   assert(found);
   unlink(path);
   free(path);
}

TEST(test_nonexistent_file)
{
   kb_lab_report_t r;
   int rc = kb_lab_run("/tmp/kb_lab_definitely_does_not_exist_xyz.md", &r);
   assert(rc == -1);
   assert(r.stage == KB_LAB_STAGE_REJECT);
}

TEST(test_stage_name)
{
   assert(strcmp(kb_lab_stage_name(KB_LAB_STAGE_READY), "ready") == 0);
   assert(strcmp(kb_lab_stage_name(KB_LAB_STAGE_REVIEW_NEEDED), "review_needed") == 0);
   assert(strcmp(kb_lab_stage_name(KB_LAB_STAGE_REJECT), "reject") == 0);
}

TEST(test_file_stats)
{
   char *path = write_tmp("# Title\n\nLine one.\nLine two.\nLine three.\n");
   kb_lab_report_t r;
   kb_lab_run(path, &r);
   assert(r.file_bytes > 0);
   assert(r.line_count >= 4);
   unlink(path);
   free(path);
}

TEST(test_chunk_preview)
{
   char *path = write_tmp("# Section\n\nContent line.\n");
   kb_lab_report_t r;
   kb_lab_run(path, &r);
   assert(r.chunk_count >= 1);
   assert(r.chunks[0].content_preview[0] != '\0');
   unlink(path);
   free(path);
}

TEST(test_code_file_kind)
{
   /* Write to a .c file so doc_kind detection works */
   char path[256];
   snprintf(path, sizeof path, "%s/kb_lab_test_XXXXXX.c", platform_tmpdir());
   int fd = mkstemps(path, 2);
   assert(fd >= 0);
   const char *content = "/* test */\nint main(void) { return 0; }\n";
   write(fd, content, strlen(content));
   close(fd);
   kb_lab_report_t r;
   kb_lab_run(path, &r);
   assert(strcmp(r.doc_kind, "code") == 0);
   unlink(path);
}

/* ── strategy tests ───────────────────────────────────────────────────── */

TEST(test_strategy_auto_sets_field)
{
   char *path = write_tmp("# Title\n\nContent.\n");
   kb_lab_report_t r;
   kb_lab_run_strategy(path, KB_LAB_STRATEGY_AUTO, &r);
   /* markdown → heading_aware */
   assert(r.strategy == KB_LAB_STRATEGY_HEADING_AWARE);
   unlink(path);
   free(path);
}

TEST(test_strategy_paragraph_splits_on_blanks)
{
   char *path = write_tmp("First paragraph content here.\n\n"
                          "Second paragraph content here.\n\n"
                          "Third paragraph content here.\n");
   kb_lab_report_t r;
   kb_lab_run_strategy(path, KB_LAB_STRATEGY_PARAGRAPH, &r);
   assert(r.strategy == KB_LAB_STRATEGY_PARAGRAPH);
   assert(r.chunk_count >= 1);
   unlink(path);
   free(path);
}

TEST(test_strategy_fixed_size_produces_chunks)
{
   /* Build a large document that forces fixed-size splits */
   char content[8192] = "";
   size_t pos = 0;
   for (int i = 0; i < 50 && pos < sizeof(content) - 40; i++)
   {
      int n = snprintf(content + pos, sizeof(content) - pos,
                       "Line %d with enough content to fill tokens.\n", i);
      if (n > 0)
         pos += (size_t)n;
   }
   char *path = write_tmp(content);
   kb_lab_report_t r;
   kb_lab_run_strategy(path, KB_LAB_STRATEGY_FIXED_SIZE, &r);
   assert(r.strategy == KB_LAB_STRATEGY_FIXED_SIZE);
   assert(r.chunk_count >= 1);
   unlink(path);
   free(path);
}

TEST(test_compare_strategies)
{
   char *path = write_tmp("# Section\n\nContent here.\n\n"
                          "## Sub-section\n\nMore content.\n");
   static const kb_lab_strategy_t strategies[] = {
       KB_LAB_STRATEGY_HEADING_AWARE,
       KB_LAB_STRATEGY_PARAGRAPH,
   };
   kb_lab_compare_t cmp;
   int rc = kb_lab_compare_strategies(path, strategies, 2, &cmp);
   assert(rc == 0);
   assert(cmp.strategy_count == 2);
   assert(cmp.reports[0].strategy == KB_LAB_STRATEGY_HEADING_AWARE);
   assert(cmp.reports[1].strategy == KB_LAB_STRATEGY_PARAGRAPH);
   assert(cmp.reports[0].chunk_count >= 1);
   assert(cmp.reports[1].chunk_count >= 1);
   unlink(path);
   free(path);
}

TEST(test_strategy_name)
{
   assert(strcmp(kb_lab_strategy_name(KB_LAB_STRATEGY_AUTO), "auto") == 0);
   assert(strcmp(kb_lab_strategy_name(KB_LAB_STRATEGY_HEADING_AWARE), "heading") == 0);
   assert(strcmp(kb_lab_strategy_name(KB_LAB_STRATEGY_PARAGRAPH), "paragraph") == 0);
   assert(strcmp(kb_lab_strategy_name(KB_LAB_STRATEGY_FIXED_SIZE), "fixed") == 0);
}

/* ── enrichment and table tests ───────────────────────────────────────── */

TEST(test_context_field_populated)
{
   char *path = write_tmp("# Introduction\n\nThis section covers the basics.\n\n"
                          "## Details\n\nMore detailed content here.\n");
   kb_lab_report_t r;
   kb_lab_run(path, &r);
   assert(r.chunk_count >= 1);
   /* Context should be non-empty for a doc with headings */
   assert(r.chunks[0].context[0] != '\0');
   unlink(path);
   free(path);
}

TEST(test_table_not_split)
{
   /* Small table that fits in one chunk — no table_split signal */
   char *path = write_tmp("# Section\n\n"
                          "| A | B | C |\n"
                          "|---|---|---|\n"
                          "| 1 | 2 | 3 |\n"
                          "| 4 | 5 | 6 |\n");
   kb_lab_report_t r;
   kb_lab_run(path, &r);
   int found = 0;
   for (int i = 0; i < r.signal_count; i++)
      if (r.signals[i].kind == KB_LAB_FLAG_TABLE_SPLIT)
         found = 1;
   assert(!found);
   unlink(path);
   free(path);
}

TEST(test_table_split_large_table)
{
   /* Build a very large table that exceeds the chunk target */
   char content[16384];
   int pos = 0;
   pos += snprintf(content + pos, sizeof(content) - (size_t)pos,
                   "# Big Table\n\n"
                   "| Column A | Column B | Column C | Column D | Column E |\n"
                   "|----------|----------|----------|----------|----------|\n");
   for (int i = 0; i < 100 && pos < (int)sizeof(content) - 80; i++)
      pos +=
          snprintf(content + pos, sizeof(content) - (size_t)pos,
                   "| Row%-6d | Data-%-4d | More-%-4d | Even-%-4d | Last-%-4d |\n", i, i, i, i, i);
   char *path = write_tmp(content);
   kb_lab_report_t r;
   kb_lab_run(path, &r);
   /* Large table forces a split → table_split signal */
   int found = 0;
   for (int i = 0; i < r.signal_count; i++)
      if (r.signals[i].kind == KB_LAB_FLAG_TABLE_SPLIT)
         found = 1;
   assert(found);
   assert(r.chunk_count >= 2);
   unlink(path);
   free(path);
}

TEST(test_stable_chunk_boundaries)
{
   char *path = write_tmp("# Section A\n\nContent A.\n\n"
                          "## Sub A1\n\nDetails here.\n\n"
                          "## Sub A2\n\nMore details.\n\n"
                          "# Section B\n\nContent B.\n");
   kb_lab_report_t r1, r2;
   kb_lab_run(path, &r1);
   kb_lab_run(path, &r2);
   assert(r1.chunk_count == r2.chunk_count);
   assert(r1.chunk_count >= 1);
   assert(r1.chunks[0].line_start == r2.chunks[0].line_start);
   assert(r1.chunks[0].token_count == r2.chunks[0].token_count);
   unlink(path);
   free(path);
}

TEST(test_per_kind_dispatch)
{
   /* AUTO on markdown → HEADING_AWARE */
   {
      char *path = write_tmp("# T\n\nContent.\n");
      kb_lab_report_t r;
      kb_lab_run_strategy(path, KB_LAB_STRATEGY_AUTO, &r);
      assert(r.strategy == KB_LAB_STRATEGY_HEADING_AWARE);
      unlink(path);
      free(path);
   }
   /* AUTO on plain text → PARAGRAPH */
   {
      char tmpl[256];
      snprintf(tmpl, sizeof tmpl, "%s/kb_lab_test_XXXXXX.txt", platform_tmpdir());
      char *path = strdup(tmpl);
      int fd = mkstemps(path, 4);
      assert(fd >= 0);
      const char *c = "Plain text content here.\n";
      write(fd, c, strlen(c));
      close(fd);
      kb_lab_report_t r;
      kb_lab_run_strategy(path, KB_LAB_STRATEGY_AUTO, &r);
      assert(r.strategy == KB_LAB_STRATEGY_PARAGRAPH);
      unlink(path);
      free(path);
   }
}

int main(void)
{
   printf("=== test_kb_lab ===\n");

   RUN(test_markdown_single_section);
   RUN(test_markdown_multiple_sections);
   RUN(test_stage_ready);
   RUN(test_flat_text_signal);
   RUN(test_heading_skip_signal);
   RUN(test_empty_file_reject);
   RUN(test_nonexistent_file);
   RUN(test_stage_name);
   RUN(test_file_stats);
   RUN(test_chunk_preview);
   RUN(test_code_file_kind);
   RUN(test_strategy_auto_sets_field);
   RUN(test_strategy_paragraph_splits_on_blanks);
   RUN(test_strategy_fixed_size_produces_chunks);
   RUN(test_compare_strategies);
   RUN(test_strategy_name);
   RUN(test_context_field_populated);
   RUN(test_table_not_split);
   RUN(test_table_split_large_table);
   RUN(test_stable_chunk_boundaries);
   RUN(test_per_kind_dispatch);

   printf("All kb_lab tests passed.\n");
   return 0;
}
