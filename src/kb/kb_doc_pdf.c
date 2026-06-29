/* kb_doc_pdf.c: structured-PDF ingestion Phase 1 (first increment). See kb_doc_pdf.h.
 * Pure pipeline over `pdftotext -bbox-layout` XHTML: parse -> normalize -> chunk ->
 * ingest into kb_documents + kb_doc_regions. No subprocess here (the exec wrapper + the
 * upload-route wiring are the next increment); this is exercised by its unit tests only. */
#include "kb_doc_pdf.h"

#include "config.h"
#include "db2/kb_payload.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Guardrail caps so a pathological document cannot exhaust memory. Parsing stops once
 * hit (partial extraction, logged) rather than erroring. */
#define KB_PDF_MAX_PAGES 20000
#define KB_PDF_MAX_LINES 500000

/* ---- small XML scanning helpers (poppler's bbox output is regular, not arbitrary XML) ---- */

/* Parse the value of attribute `name` (e.g. "width") found within [s, e) as a double.
 * Returns 1 and writes *out on success, 0 if the attribute is absent/malformed. */
static int attr_double(const char *s, const char *e, const char *name, double *out)
{
   size_t nlen = strlen(name);
   for (const char *p = s; p && p + nlen + 2 < e; p++)
   {
      /* match `name="` with a preceding space/'<' so "xMin" doesn't match inside a word */
      if ((p == s || isspace((unsigned char)p[-1])) && strncmp(p, name, nlen) == 0 &&
          p[nlen] == '=' && p[nlen + 1] == '"')
      {
         const char *v = p + nlen + 2;
         char *endp = NULL;
         double d = strtod(v, &endp);
         if (endp && endp > v)
         {
            *out = d;
            return 1;
         }
         return 0;
      }
   }
   return 0;
}

/* Append decoded text content of an XML range [s,e) (a <word>'s inner text) to `dst`
 * (a growable buffer). Decodes the handful of entities poppler emits. */
static void append_decoded(char **dst, size_t *len, size_t *cap, const char *s, const char *e)
{
   for (const char *p = s; p < e;)
   {
      char ch;
      size_t adv = 1;
      if (*p == '&')
      {
         if (e - p >= 5 && strncmp(p, "&amp;", 5) == 0)
         {
            ch = '&';
            adv = 5;
         }
         else if (e - p >= 4 && strncmp(p, "&lt;", 4) == 0)
         {
            ch = '<';
            adv = 4;
         }
         else if (e - p >= 4 && strncmp(p, "&gt;", 4) == 0)
         {
            ch = '>';
            adv = 4;
         }
         else if (e - p >= 6 && strncmp(p, "&quot;", 6) == 0)
         {
            ch = '"';
            adv = 6;
         }
         else if (e - p >= 6 && strncmp(p, "&apos;", 6) == 0)
         {
            ch = '\'';
            adv = 6;
         }
         else if (e - p >= 5 && strncmp(p, "&#39;", 5) == 0)
         {
            ch = '\'';
            adv = 5;
         }
         else if (e - p >= 5 && strncmp(p, "&#34;", 5) == 0)
         {
            ch = '"';
            adv = 5;
         }
         else
         {
            ch = '&';
            adv = 1;
         }
      }
      else
      {
         ch = *p;
      }
      if (*len + 2 > *cap)
      {
         size_t ncap = *cap ? *cap * 2 : 64;
         char *nb = realloc(*dst, ncap);
         if (!nb)
            return;
         *dst = nb;
         *cap = ncap;
      }
      (*dst)[(*len)++] = ch;
      p += adv;
   }
   if (*dst)
      (*dst)[*len] = '\0';
}

static int page_add_line(kb_pdf_page_t *pg, int page_no, double x0, double y0, double x1, double y1,
                         char *text)
{
   if (pg->n_lines >= pg->cap_lines)
   {
      int ncap = pg->cap_lines ? pg->cap_lines * 2 : 16;
      kb_pdf_line_t *nl = realloc(pg->lines, (size_t)ncap * sizeof(*nl));
      if (!nl)
         return -1;
      pg->lines = nl;
      pg->cap_lines = ncap;
   }
   kb_pdf_line_t *ln = &pg->lines[pg->n_lines++];
   ln->page_no = page_no;
   ln->x0 = x0;
   ln->y0 = y0;
   ln->x1 = x1;
   ln->y1 = y1;
   ln->text = text;
   return 0;
}

/* Collect the words of a <line> (or a bare <word> run) bounded by [s,e) into one text
 * string (space-joined). Returns a malloc'd string (possibly empty), or NULL on OOM. */
static char *collect_words(const char *s, const char *e)
{
   char *buf = NULL;
   size_t len = 0, cap = 0;
   const char *p = s;
   int first = 1;
   while (p < e)
   {
      const char *w = strstr(p, "<word");
      if (!w || w >= e)
         break;
      const char *gt = memchr(w, '>', (size_t)(e - w));
      if (!gt)
         break;
      const char *close = strstr(gt + 1, "</word>");
      if (!close || close > e)
         break;
      if (!first)
      {
         /* word separator */
         if (len + 2 > cap)
         {
            size_t ncap = cap ? cap * 2 : 64;
            char *nb = realloc(buf, ncap);
            if (!nb)
            {
               free(buf);
               return NULL;
            }
            buf = nb;
            cap = ncap;
         }
         buf[len++] = ' ';
      }
      first = 0;
      append_decoded(&buf, &len, &cap, gt + 1, close);
      p = close + 7; /* strlen("</word>") */
   }
   if (!buf)
   {
      buf = malloc(1);
      if (buf)
         buf[0] = '\0';
   }
   return buf;
}

int kb_pdf_parse_bbox_layout(const char *xhtml, kb_pdf_doc_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!xhtml)
      return 0;

   int total_lines = 0;
   const char *p = xhtml;
   while (1)
   {
      const char *pg_open = strstr(p, "<page");
      if (!pg_open)
         break;
      const char *pg_tag_end = strchr(pg_open, '>');
      if (!pg_tag_end)
         break;
      const char *pg_close = strstr(pg_tag_end, "</page>");
      const char *pg_body_end = pg_close ? pg_close : (xhtml + strlen(xhtml));

      if (out->n_pages >= KB_PDF_MAX_PAGES)
      {
         LOG_WARN("kb_doc_pdf", "page cap (%d) reached; truncating extraction", KB_PDF_MAX_PAGES);
         break;
      }
      if (out->n_pages >= out->cap_pages)
      {
         int ncap = out->cap_pages ? out->cap_pages * 2 : 8;
         kb_pdf_page_t *np = realloc(out->pages, (size_t)ncap * sizeof(*np));
         if (!np)
         {
            kb_pdf_free_doc(out); /* free the partial doc; out left empty/safe */
            return -1;
         }
         out->pages = np;
         out->cap_pages = ncap;
      }
      kb_pdf_page_t *pg = &out->pages[out->n_pages];
      memset(pg, 0, sizeof(*pg));
      int page_no = out->n_pages + 1;
      out->n_pages++;

      if (!attr_double(pg_open, pg_tag_end, "width", &pg->width))
         pg->width = 0;
      if (!attr_double(pg_open, pg_tag_end, "height", &pg->height))
         pg->height = 0;

      /* Lines within the page. */
      const char *lp = pg_tag_end;
      int saw_line = 0;
      while (lp < pg_body_end)
      {
         const char *ln_open = strstr(lp, "<line");
         if (!ln_open || ln_open >= pg_body_end)
            break;
         const char *ln_tag_end = strchr(ln_open, '>');
         if (!ln_tag_end || ln_tag_end >= pg_body_end)
            break;
         const char *ln_close = strstr(ln_tag_end, "</line>");
         const char *ln_body_end = (ln_close && ln_close < pg_body_end) ? ln_close : pg_body_end;
         saw_line = 1;

         double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
         attr_double(ln_open, ln_tag_end, "xMin", &x0);
         attr_double(ln_open, ln_tag_end, "yMin", &y0);
         attr_double(ln_open, ln_tag_end, "xMax", &x1);
         attr_double(ln_open, ln_tag_end, "yMax", &y1);

         char *text = collect_words(ln_tag_end + 1, ln_body_end);
         if (text && text[0] && total_lines < KB_PDF_MAX_LINES)
         {
            if (page_add_line(pg, page_no, x0, y0, x1, y1, text) != 0)
            {
               free(text);
               kb_pdf_free_doc(out);
               return -1;
            }
            total_lines++;
         }
         else
         {
            free(text);
         }
         lp = ln_close ? ln_close + 7 : pg_body_end;
      }

      /* Degraded structure (no <line> wrappers): treat each <word> as its own line so
       * no text is lost. */
      if (!saw_line)
      {
         const char *wp = pg_tag_end;
         while (wp < pg_body_end && total_lines < KB_PDF_MAX_LINES)
         {
            const char *w = strstr(wp, "<word");
            if (!w || w >= pg_body_end)
               break;
            const char *gt = memchr(w, '>', (size_t)(pg_body_end - w));
            if (!gt)
               break;
            const char *close = strstr(gt + 1, "</word>");
            if (!close || close > pg_body_end)
               break;
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            attr_double(w, gt, "xMin", &x0);
            attr_double(w, gt, "yMin", &y0);
            attr_double(w, gt, "xMax", &x1);
            attr_double(w, gt, "yMax", &y1);
            char *text = NULL;
            size_t tl = 0, tc = 0;
            append_decoded(&text, &tl, &tc, gt + 1, close);
            if (text && text[0])
            {
               if (page_add_line(pg, page_no, x0, y0, x1, y1, text) != 0)
               {
                  free(text);
                  kb_pdf_free_doc(out);
                  return -1;
               }
               total_lines++;
            }
            else
            {
               free(text);
            }
            wp = close + 7;
         }
      }

      if (!pg_close)
         break;
      p = pg_close + 7; /* strlen("</page>") */
   }
   return 0;
}

static double clamp01(double v)
{
   if (v < 0)
      return 0;
   if (v > 1)
      return 1;
   return v;
}

void kb_pdf_normalize(kb_pdf_doc_t *doc)
{
   if (!doc || doc->normalized)
      return;
   for (int i = 0; i < doc->n_pages; i++)
   {
      kb_pdf_page_t *pg = &doc->pages[i];
      double w = pg->width, h = pg->height;
      for (int j = 0; j < pg->n_lines; j++)
      {
         kb_pdf_line_t *ln = &pg->lines[j];
         if (w > 0 && h > 0)
         {
            ln->x0 = clamp01(ln->x0 / w);
            ln->y0 = clamp01(ln->y0 / h);
            ln->x1 = clamp01(ln->x1 / w);
            ln->y1 = clamp01(ln->y1 / h);
         }
         else
         {
            ln->x0 = ln->y0 = ln->x1 = ln->y1 = 0;
         }
      }
   }
   doc->normalized = 1;
}

static int count_tokens(const char *s)
{
   int n = 0, in = 0;
   for (; s && *s; s++)
   {
      if (isspace((unsigned char)*s))
         in = 0;
      else if (!in)
      {
         in = 1;
         n++;
      }
   }
   return n;
}

/* Finalize one accumulated chunk: join its line texts with '\n' into content. */
static int finalize_chunk(kb_pdf_chunk_t *c)
{
   size_t total = 1;
   for (int i = 0; i < c->n_lines; i++)
      total += strlen(c->lines[i]->text) + 1;
   c->content = malloc(total);
   if (!c->content)
      return -1;
   size_t off = 0;
   for (int i = 0; i < c->n_lines; i++)
   {
      if (i)
         c->content[off++] = '\n';
      size_t l = strlen(c->lines[i]->text);
      memcpy(c->content + off, c->lines[i]->text, l);
      off += l;
   }
   c->content[off] = '\0';
   c->token_count = count_tokens(c->content);
   return 0;
}

int kb_pdf_chunk(const kb_pdf_doc_t *doc, kb_pdf_chunk_t **chunks_out, int *n_chunks_out)
{
   if (!doc || !chunks_out || !n_chunks_out)
      return -1;
   *chunks_out = NULL;
   *n_chunks_out = 0;

   kb_pdf_chunk_t *chunks = NULL;
   int n = 0, cap = 0;
   kb_pdf_chunk_t cur;
   memset(&cur, 0, sizeof(cur));
   int have_cur = 0;
   int global_idx = 0; /* 0-based ordinal across the whole doc */
   int cur_page = -1;

   for (int pi = 0; pi < doc->n_pages; pi++)
   {
      const kb_pdf_page_t *pg = &doc->pages[pi];
      for (int li = 0; li < pg->n_lines; li++)
      {
         const kb_pdf_line_t *ln = &pg->lines[li];
         /* Break on page change or line cap. */
         if (have_cur && (ln->page_no != cur_page || cur.n_lines >= KB_PDF_MAX_CHUNK_LINES))
         {
            if (finalize_chunk(&cur) != 0)
               goto oom;
            if (n >= cap)
            {
               int ncap = cap ? cap * 2 : 8;
               kb_pdf_chunk_t *nc = realloc(chunks, (size_t)ncap * sizeof(*nc));
               if (!nc)
                  goto oom;
               chunks = nc;
               cap = ncap;
            }
            chunks[n++] = cur;
            memset(&cur, 0, sizeof(cur));
            have_cur = 0;
         }
         if (!have_cur)
         {
            have_cur = 1;
            cur_page = ln->page_no;
            cur.line_start = global_idx;
            cur.page_start = ln->page_no;
            cur.page_end = ln->page_no;
         }
         /* append line pointer */
         {
            const kb_pdf_line_t **nl =
                realloc((void *)cur.lines, (size_t)(cur.n_lines + 1) * sizeof(*cur.lines));
            if (!nl)
               goto oom;
            cur.lines = nl;
            cur.lines[cur.n_lines++] = ln;
         }
         cur.line_end = global_idx;
         if (ln->page_no < cur.page_start)
            cur.page_start = ln->page_no;
         if (ln->page_no > cur.page_end)
            cur.page_end = ln->page_no;
         global_idx++;
      }
   }
   if (have_cur)
   {
      if (finalize_chunk(&cur) != 0)
         goto oom;
      if (n >= cap)
      {
         int ncap = cap ? cap * 2 : 8;
         kb_pdf_chunk_t *nc = realloc(chunks, (size_t)ncap * sizeof(*nc));
         if (!nc)
            goto oom;
         chunks = nc;
         cap = ncap;
      }
      chunks[n++] = cur;
      memset(&cur, 0, sizeof(cur));
   }

   *chunks_out = chunks;
   *n_chunks_out = n;
   return n;

oom:
   free(cur.content);
   free((void *)cur.lines);
   kb_pdf_free_chunks(chunks, n);
   return -1;
}

void kb_pdf_free_chunks(kb_pdf_chunk_t *chunks, int n_chunks)
{
   if (!chunks)
      return;
   for (int i = 0; i < n_chunks; i++)
   {
      free(chunks[i].content);
      free((void *)chunks[i].lines);
   }
   free(chunks);
}

void kb_pdf_free_doc(kb_pdf_doc_t *doc)
{
   if (!doc)
      return;
   for (int i = 0; i < doc->n_pages; i++)
   {
      for (int j = 0; j < doc->pages[i].n_lines; j++)
         free(doc->pages[i].lines[j].text);
      free(doc->pages[i].lines);
   }
   free(doc->pages);
   memset(doc, 0, sizeof(*doc));
}

/* Child resource caps (defense-in-depth against a crafted PDF that makes pdftotext spin or
 * balloon). The wall-clock deadline below is the primary bound; these are belt-and-braces. */
#define KB_PDF_CHILD_CPU_SECS    30
#define KB_PDF_CHILD_AS_BYTES    (1024UL * 1024UL * 1024UL) /* 1 GiB address space */
#define KB_PDF_CHILD_FSIZE_BYTES (256UL * 1024UL * 1024UL)

static long ms_since(const struct timespec *start)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

int kb_pdf_exec_bbox_layout(const unsigned char *bytes, int n, char *out, int out_cap,
                            int timeout_ms)
{
   if (!bytes || n <= 0 || !out || out_cap <= 1)
      return -1;
   out[0] = '\0';

   char tmppath[] = "/tmp/aimee_pdf_XXXXXX";
   int tfd = mkstemp(tmppath);
   if (tfd < 0)
   {
      LOG_WARN("kb_doc_pdf", "mkstemp failed for pdftotext input");
      return -1;
   }
   (void)fchmod(tfd, 0600);
   int rc = -1;
   {
      int off = 0;
      while (off < n)
      {
         ssize_t w = write(tfd, bytes + off, (size_t)(n - off));
         if (w < 0)
         {
            if (errno == EINTR)
               continue; /* retry — a signal must not corrupt valid input */
            break;
         }
         if (w == 0)
            break;
         off += (int)w;
      }
      close(tfd);
      if (off != n)
      {
         unlink(tmppath);
         return -1;
      }
   }

   int pfd[2];
   if (pipe(pfd) != 0)
   {
      unlink(tmppath);
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(pfd[0]);
      close(pfd[1]);
      unlink(tmppath);
      return -1;
   }
   if (pid == 0)
   {
      /* child */
      setsid(); /* own session/group: isolates it from the server's signals and lets the
                 * parent kill the whole group (defense-in-depth if pdftotext ever forks) */
      close(pfd[0]);
      dup2(pfd[1], STDOUT_FILENO);
      close(pfd[1]);
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0)
      {
         dup2(devnull, STDERR_FILENO);
         close(devnull);
      }
      struct rlimit rl;
      rl.rlim_cur = rl.rlim_max = KB_PDF_CHILD_CPU_SECS;
      setrlimit(RLIMIT_CPU, &rl);
      rl.rlim_cur = rl.rlim_max = KB_PDF_CHILD_AS_BYTES;
      setrlimit(RLIMIT_AS, &rl);
      rl.rlim_cur = rl.rlim_max = KB_PDF_CHILD_FSIZE_BYTES;
      setrlimit(RLIMIT_FSIZE, &rl);
      execlp("pdftotext", "pdftotext", "-bbox-layout", tmppath, "-", (char *)NULL);
      _exit(127);
   }

   /* parent */
   close(pfd[1]);
   fcntl(pfd[0], F_SETFL, O_NONBLOCK);

   struct timespec start;
   clock_gettime(CLOCK_MONOTONIC, &start);
   int total = 0, timed_out = 0, over_cap = 0;
   for (;;)
   {
      long elapsed = ms_since(&start);
      long remaining = (long)timeout_ms - elapsed;
      if (remaining <= 0)
      {
         timed_out = 1;
         break;
      }
      struct pollfd p = {pfd[0], POLLIN, 0};
      int pr = poll(&p, 1, (int)remaining);
      if (pr == 0)
      {
         timed_out = 1;
         break;
      }
      if (pr < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      ssize_t r = read(pfd[0], out + total, (size_t)(out_cap - 1 - total));
      if (r == 0)
         break; /* EOF — child closed stdout */
      if (r < 0)
      {
         if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
         break;
      }
      total += (int)r;
      if (total >= out_cap - 1)
      {
         over_cap = 1;
         break;
      }
   }
   out[total < 0 ? 0 : total] = '\0';
   close(pfd[0]);

   if (timed_out || over_cap)
   {
      kill(pid, SIGKILL);  /* the child directly … */
      kill(-pid, SIGKILL); /* … and its process group (post-setsid; ESRCH no-op otherwise) */
   }

   /* Bounded reap: poll waitpid until a short grace deadline, then SIGKILL + block. */
   int status = 0, reaped = 0;
   struct timespec reap_start;
   clock_gettime(CLOCK_MONOTONIC, &reap_start);
   for (;;)
   {
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid)
      {
         reaped = 1;
         break;
      }
      if (w < 0 && errno != EINTR)
         break;
      if (ms_since(&reap_start) > 2000)
      {
         kill(pid, SIGKILL);
         kill(-pid, SIGKILL);
         /* blocking reap, EINTR-safe — never leave a zombie */
         while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
         reaped = 1;
         break;
      }
      struct timespec ts = {0, 5 * 1000 * 1000}; /* 5ms */
      nanosleep(&ts, NULL);
   }

   unlink(tmppath);

   if (timed_out)
      LOG_WARN("kb_doc_pdf", "pdftotext timed out after %dms", timeout_ms);
   else if (over_cap)
      LOG_WARN("kb_doc_pdf", "pdftotext output exceeded %d-byte cap", out_cap);
   else if (reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0)
      rc = 0;
   else
      LOG_WARN("kb_doc_pdf", "pdftotext failed (exit status %d)", status);

   return rc;
}

int kb_pdf_sensitivity_valid(const char *s)
{
   return s &&
          (strcmp(s, "public") == 0 || strcmp(s, "internal") == 0 || strcmp(s, "restricted") == 0);
}

int kb_doc_pdf_ingest(const char *project, const char *file_path, const char *file_hash,
                      const kb_pdf_doc_t *doc, const char *sensitivity_class,
                      kb_pdf_ingest_stats_t *stats)
{
   if (stats)
      memset(stats, 0, sizeof(*stats));
   if (!project || !file_path || !file_hash || !doc)
      return -1;
   /* The upload surface validates the class before reaching here; refuse to ingest under
    * an invalid/empty class so a row can never be written un-tagged. */
   if (!kb_pdf_sensitivity_valid(sensitivity_class))
      return -1;
   const char *quarantine = strcmp(sensitivity_class, "restricted") == 0 ? "pending" : "";

   /* Phase A1: when the PDF-vector capability is on, enqueue an embed_pdf job per
    * chunk so it lands in the isolated kb_pdf_embeddings relation. We do NOT embed
    * quarantined-pending (restricted) docs — they are withheld until an owner
    * confirms, at which point the confirm path enqueues their embed_pdf jobs. */
   config_t pdf_cfg;
   int vector_enabled = (config_load(&pdf_cfg) == 0 && pdf_cfg.kb_pdf_vector_enabled);
   int embed_pdf_vec = vector_enabled && quarantine[0] == '\0';

   kb_pdf_chunk_t *chunks = NULL;
   int n_chunks = 0;
   if (kb_pdf_chunk(doc, &chunks, &n_chunks) < 0)
      return -1;

   /* Nothing extracted (empty / zero-line PDF): do NOT touch the DB. In particular do
    * not run the destructive delete below — a re-ingest whose extraction came back empty
    * must never silently wipe a document's prior rows. */
   if (n_chunks == 0)
   {
      kb_pdf_free_chunks(chunks, n_chunks);
      return 0;
   }

   /* The whole re-ingest is one transaction: the delete (regions cascade via the
    * kb_doc_regions FK ON DELETE CASCADE), every chunk + region insert, the neighbour
    * links, and the embed enqueues are all-or-nothing — a mid-loop failure rolls back so
    * the KB is never left with prior rows gone and only partial new rows. The borrowed
    * line pointers in `chunks` stay valid throughout: `doc` is owned by the caller and is
    * never freed here, and `chunks` is built, consumed, and freed inside this function. */
   if (db2_kb_txn_begin() != 0)
   {
      kb_pdf_free_chunks(chunks, n_chunks);
      return -1;
   }

   db2_kb_documents_delete_for_file(project, file_path); /* void; covered by the txn */

   int n_regions = 0;
   int64_t prev_id = 0; /* per-call local; the first chunk links with 0 (a no-op in
                         * db2_kb_documents_link_neighbours), and nothing carries across calls */
   for (int i = 0; i < n_chunks; i++)
   {
      kb_pdf_chunk_t *c = &chunks[i];
      int64_t id = db2_kb_documents_insert_chunk_pdf(
          project, file_path, file_hash, i, "" /* heading_path: Phase-1 page chunking */,
          c->line_start, c->line_end, c->content ? c->content : "", c->token_count,
          "page" /* chunk_strategy */, c->page_start, c->page_end, sensitivity_class, quarantine);
      if (id <= 0)
         goto fail;
      db2_kb_documents_link_neighbours(id, prev_id); /* void; covered by the txn */

      for (int j = 0; j < c->n_lines; j++)
      {
         const kb_pdf_line_t *ln = c->lines[j];
         if (db2_kb_doc_regions_insert(id, file_path, ln->page_no, ln->x0, ln->y0, ln->x1, ln->y1,
                                       ln->text ? ln->text : "", j, "text", sensitivity_class) <= 0)
            goto fail;
         n_regions++;
      }

      /* Phase A1: enqueue the embed into the ISOLATED kb_pdf_embeddings relation
       * (kind='embed_pdf'), inside this same ingest transaction. Because the job
       * row only becomes visible to the drainer at commit — by which point this
       * chunk's kb_doc_regions are committed too — a vector-retrievable PDF chunk
       * always has its citations (the §A2 LEFT JOIN is the backstop if that ever
       * races). When the capability is off the chunk stays lexical-only, exactly
       * as Phase 2 behaved, and is invisible to the vector-only /v1/search by
       * construction (PDF vectors never enter kb_embeddings). */
      if (embed_pdf_vec)
         db2_kb_async_enqueue("embed_pdf", id, project); /* best-effort; covered by the txn */
      prev_id = id;
   }

   if (db2_kb_txn_commit() != 0)
      goto fail;

   if (stats)
   {
      stats->chunks = n_chunks;
      stats->regions = n_regions;
   }
   kb_pdf_free_chunks(chunks, n_chunks);
   return n_chunks;

fail:
   db2_kb_txn_rollback();
   kb_pdf_free_chunks(chunks, n_chunks);
   return -1;
}

int kb_doc_pdf_ingest_xhtml(const char *project, const char *file_path, const char *file_hash,
                            const char *xhtml, const char *sensitivity_class,
                            kb_pdf_ingest_stats_t *stats)
{
   kb_pdf_doc_t doc;
   if (kb_pdf_parse_bbox_layout(xhtml, &doc) != 0)
      return -1;
   kb_pdf_normalize(&doc);
   int rc = kb_doc_pdf_ingest(project, file_path, file_hash, &doc, sensitivity_class, stats);
   kb_pdf_free_doc(&doc);
   return rc;
}
