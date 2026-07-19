/* wfe_replay_worktree.c: worktree-grounded evidence-replay backend (see the
 * header). Pure libc file scanning — no index/db2/kb symbol, so it links in
 * every context that links evidence_replay. */
#include "wfe_replay_worktree.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static __thread char g_root[MAX_PATH_LEN];

void wfe_replay_worktree_set_root(const char *root)
{
   snprintf(g_root, sizeof g_root, "%s", root ? root : "");
}

/* Bounds: a gate worktree is one repo checkout, but stay defensive. */
#define WT_MAX_DEPTH      12
#define WT_MAX_FILES      20000
#define WT_MAX_FILE_BYTES (1 << 20)
#define WT_LINE_MAX       4096

typedef struct
{
   const char *needle; /* symbol or search term */
   int want_call;      /* require '(' after the (word-boundary) match */
   int word;           /* require word boundaries around the match */
   int count;          /* total matching lines found */
   int files_seen;     /* walk budget */
   /* hit sinks (any may be NULL); all record repo-relative path + 1-based line */
   term_hit_t *sym_out;
   caller_hit_t *cal_out;
   code_search_hit_t *sea_out;
   int out_max;
   int out_n;
} wt_scan_t;

static int is_word(char c)
{
   return isalnum((unsigned char)c) || c == '_';
}

/* Does `line` contain the needle (per s->word / s->want_call)? */
static int line_matches(const wt_scan_t *s, const char *line)
{
   size_t nl = strlen(s->needle);
   for (const char *p = strstr(line, s->needle); p; p = strstr(p + 1, s->needle))
   {
      if (s->word)
      {
         if (p > line && is_word(p[-1]))
            continue;
         if (is_word(p[nl]))
            continue;
      }
      if (s->want_call)
      {
         const char *q = p + nl;
         while (*q == ' ' || *q == '\t')
            q++;
         if (*q != '(')
            continue;
      }
      return 1;
   }
   return 0;
}

static void record_hit(wt_scan_t *s, const char *rel, int line)
{
   s->count++;
   if (s->out_n >= s->out_max)
      return;
   if (s->sym_out)
   {
      term_hit_t *h = &s->sym_out[s->out_n];
      memset(h, 0, sizeof *h);
      snprintf(h->project, sizeof h->project, "worktree");
      snprintf(h->file_path, sizeof h->file_path, "%s", rel);
      h->line = line;
      snprintf(h->kind, sizeof h->kind, "occurrence");
   }
   else if (s->cal_out)
   {
      caller_hit_t *h = &s->cal_out[s->out_n];
      memset(h, 0, sizeof *h);
      snprintf(h->project, sizeof h->project, "worktree");
      snprintf(h->file_path, sizeof h->file_path, "%s", rel);
      h->line = line;
   }
   else if (s->sea_out)
   {
      code_search_hit_t *h = &s->sea_out[s->out_n];
      memset(h, 0, sizeof *h);
      snprintf(h->file_path, sizeof h->file_path, "%s", rel);
      h->line = line;
   }
   s->out_n++;
}

static void scan_file(wt_scan_t *s, const char *abs, const char *rel)
{
   FILE *fp = fopen(abs, "r");
   if (!fp)
      return;
   char buf[WT_LINE_MAX];
   /* binary sniff: a NUL in the first read chunk disqualifies the file */
   size_t n0 = fread(buf, 1, sizeof buf - 1, fp);
   if (memchr(buf, '\0', n0))
   {
      fclose(fp);
      return;
   }
   rewind(fp);
   int line = 0;
   while (fgets(buf, sizeof buf, fp))
   {
      line++;
      if (line_matches(s, buf))
         record_hit(s, rel, line);
   }
   fclose(fp);
}

/* Skip VCS metadata and heavyweight generated trees; the worktree is otherwise
 * the ground truth (a panel finding may cite generated-but-committed files). */
static int skip_dir(const char *name)
{
   return name[0] == '.' || strcmp(name, "node_modules") == 0 || strcmp(name, "build") == 0;
}

static void walk(wt_scan_t *s, const char *abs, const char *rel, int depth)
{
   if (depth > WT_MAX_DEPTH || s->files_seen > WT_MAX_FILES)
      return;
   DIR *d = opendir(abs);
   if (!d)
      return;
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (e->d_name[0] == '.')
         continue;
      char abs2[MAX_PATH_LEN], rel2[MAX_PATH_LEN];
      if (snprintf(abs2, sizeof abs2, "%s/%s", abs, e->d_name) >= (int)sizeof abs2)
         continue;
      if (snprintf(rel2, sizeof rel2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) >=
          (int)sizeof rel2)
         continue;
      struct stat st;
      if (lstat(abs2, &st) != 0)
         continue; /* never follow symlinks out of the worktree */
      if (S_ISDIR(st.st_mode))
      {
         if (!skip_dir(e->d_name))
            walk(s, abs2, rel2, depth + 1);
      }
      else if (S_ISREG(st.st_mode) && st.st_size <= WT_MAX_FILE_BYTES)
      {
         if (++s->files_seen > WT_MAX_FILES)
            break;
         scan_file(s, abs2, rel2);
      }
   }
   closedir(d);
}

static int wt_scan(wt_scan_t *s)
{
   if (!g_root[0] || !s->needle || !s->needle[0])
      return -1;
   walk(s, g_root, "", 0);
   return s->count;
}

static int wt_find_symbol(const char *identifier, term_hit_t *out, int max)
{
   wt_scan_t s = {.needle = identifier, .word = 1, .sym_out = out, .out_max = max};
   return wt_scan(&s);
}

static int wt_find_callers(const char *project, const char *symbol, caller_hit_t *out, int max)
{
   (void)project; /* single-worktree scope */
   wt_scan_t s = {.needle = symbol, .word = 1, .want_call = 1, .cal_out = out, .out_max = max};
   return wt_scan(&s);
}

static int wt_code_search(const char *query, const char *project, code_search_hit_t *out, int max)
{
   (void)project;
   wt_scan_t s = {.needle = query, .sea_out = out, .out_max = max};
   return wt_scan(&s);
}

static int wt_project_count(void)
{
   /* No root bound on this thread => "no index here": every item DEGRADES
    * (kept, unverified) instead of being falsely contradicted. */
   return g_root[0] ? 1 : 0;
}

const replay_backend_t *wfe_replay_worktree_backend(void)
{
   static const replay_backend_t BE = {
       .find_symbol = wt_find_symbol,
       .find_callers = wt_find_callers,
       .code_search = wt_code_search,
       .project_count = wt_project_count,
   };
   return &BE;
}
