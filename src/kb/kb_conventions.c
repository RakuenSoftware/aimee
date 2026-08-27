/* kb_conventions.c: Convention-source extraction from the knowledge base.
 * Scans kb_documents for convention files and emits L3 memory candidates.
 * Split from kb.c to keep that file under the 2000-line limit. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "aimee.h"
#include "modules/db2/c/kb_payload.h"
#include "modules/db2/c/kb_service_backend.h"
#include "modules/db2/c/memory_query.h"
#include "modules/db2/c/lifecycle.h"
#include "kb.h"
#include "memory.h"
#include "log.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Convention extraction: KB → memory                                   */
/* ------------------------------------------------------------------ */

/* Is the file_path a known convention source?  Conventions come from the
 * repository documents that describe style rules, contribution norms, and
 * repo-local workflow. */
static int kb_is_convention_source(const char *file_path)
{
   if (!file_path || !file_path[0])
      return 0;
   /* Case-insensitive suffix match on the basename. */
   const char *slash = strrchr(file_path, '/');
   const char *base = slash ? slash + 1 : file_path;
   static const char *patterns[] = {"CONTRIBUTING.md",
                                    "CONTRIBUTING.rst",
                                    "AGENTS.md",
                                    "STYLE.md",
                                    "STYLEGUIDE.md",
                                    "CODING.md",
                                    "CODE_STYLE.md",
                                    "CODING_STANDARDS.md",
                                    ".aimee-rules",
                                    "aimee-rules.md",
                                    NULL};
   for (int i = 0; patterns[i]; i++)
      if (strcasecmp(base, patterns[i]) == 0)
         return 1;
   /* Paths like .aimee/rules.md, .aimee/context.md. */
   if (strstr(file_path, ".aimee/rules.md") || strstr(file_path, ".aimee/context.md"))
      return 1;
   /* Architecture Decision Records under docs/adr or similar paths. */
   if (strstr(file_path, "/adr/") || strstr(file_path, "/ADR/"))
      return 1;
   if (strncasecmp(base, "adr-", 4) == 0)
      return 1;
   return 0;
}

/* First sentence (up to ~200 chars) — used as the candidate memory value. */
static void first_sentence(const char *content, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!content)
      return;
   /* Skip leading whitespace and markdown bullets/markers. */
   const char *p = content;
   while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '-' || *p == '*' || *p == '#'))
      p++;
   size_t i = 0;
   while (*p && i + 1 < out_len && i < 200)
   {
      if (*p == '\n' && (p[1] == '\n' || p[1] == '\0'))
         break;
      if (*p == '.' && (p[1] == ' ' || p[1] == '\n' || p[1] == '\0'))
      {
         out[i++] = *p++;
         break;
      }
      out[i++] = *p++;
   }
   out[i] = '\0';
}

/* Scan kb_documents for convention-source files and emit L3 fact candidates.
 * Each chunk's (heading_path, first-sentence-of-content) becomes a candidate
 * memory keyed by "convention_<project>_<file_basename>_<heading_hash>".
 * Existing keys are skipped (idempotent).  Low confidence (0.6) reflects that
 * the extraction is heuristic and the memory should be treated as KB-adjacent
 * until an operator approves it (see `memory approve <id>`).
 *
 * Returns the number of candidates emitted, or -1 on DB error.  Safe to call
 * from the maintenance cycle. */
int kb_extract_convention_candidates(void)
{
   if (!db2_is_initialized())
      return -1;

   const int row_cap = 500;
   db2_kb_convention_row_t *rows = malloc((size_t)row_cap * sizeof(*rows));
   if (!rows)
      return -1;
   int n_rows = db2_kb_documents_list_convention_candidates(rows, row_cap);

   int emitted = 0;
   for (int i = 0; i < n_rows; i++)
   {
      const char *project = rows[i].project;
      const char *file_path = rows[i].file_path;
      const char *heading = rows[i].heading_path;
      const char *content = rows[i].content;
      if (!project[0] || !file_path[0] || !content[0])
         continue;
      if (!kb_is_convention_source(file_path))
         continue;

      const char *slash = strrchr(file_path, '/');
      const char *base = slash ? slash + 1 : file_path;

      char sentence[256];
      first_sentence(content, sentence, sizeof(sentence));
      if (strlen(sentence) < 10)
         continue;

      char key[300];
      if (heading[0])
         snprintf(key, sizeof(key), "convention_%.32s_%.64s_%.128s", project, base, heading);
      else
         snprintf(key, sizeof(key), "convention_%.32s_%.64s_%.100s", project, base, sentence);
      for (char *c = key; *c; c++)
         if (*c == ' ' || *c == '\t' || *c == ':' || *c == '/' || *c == '#')
            *c = '_';

      char norm_lookup[512];
      normalize_key(key, norm_lookup, sizeof(norm_lookup));
      if (db2_memory_key_exists_in_tier_pair(norm_lookup, TIER_L3, TIER_L4))
         continue;

      memory_t mem = {0};
      if (memory_insert(TIER_L3, KIND_FACT, key, sentence, 0.6, "", &mem) != 0)
         continue;
      emitted++;
   }
   free(rows);
   if (emitted > 0)
      LOG_INFO("kb", "extract_convention_candidates: emitted %d L3 candidates", emitted);
   return emitted;
}
