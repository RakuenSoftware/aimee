/* shutdown_forensics.c: durable context for daemon shutdown signals. */
#include "shutdown_forensics.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef _WIN32
#include <signal.h>
#endif

#define FORENSICS_KEEP_DEFAULT 50

static int mkdir_p(const char *path)
{
   if (!path || !path[0])
      return -1;

   char tmp[4096];
   snprintf(tmp, sizeof(tmp), "%s", path);
   size_t len = strlen(tmp);
   if (len == 0 || len >= sizeof(tmp))
      return -1;
   if (tmp[len - 1] == '/')
      tmp[len - 1] = '\0';

   for (char *p = tmp + 1; *p; p++)
   {
      if (*p != '/')
         continue;
      *p = '\0';
      if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
         return -1;
      *p = '/';
   }
   if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
      return -1;
   return 0;
}

const char *shutdown_forensics_dir(void)
{
   static char path[4096];
   const char *override = getenv("AIMEE_FORENSICS_DIR");
   if (override && override[0])
   {
      snprintf(path, sizeof(path), "%s", override);
      return path;
   }

   const char *home = getenv("HOME");
   if (!home || !home[0])
      home = getenv("USERPROFILE");
   if (!home || !home[0])
      return NULL;
   snprintf(path, sizeof(path), "%s/.cache/aimee/forensics", home);
   return path;
}

const char *shutdown_forensics_signal_name(int sig)
{
   switch (sig)
   {
   case SIGINT:
      return "SIGINT";
   case SIGTERM:
      return "SIGTERM";
#ifdef SIGHUP
   case SIGHUP:
      return "SIGHUP";
#endif
#ifdef SIGQUIT
   case SIGQUIT:
      return "SIGQUIT";
#endif
   default:
      return "SIGNAL";
   }
}

static void read_sender_comm(pid_t pid, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
#ifndef _WIN32
   if (pid <= 0)
      return;
   char path[64];
   snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
   int fd = open(path, O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return;
   ssize_t n = read(fd, out, out_len - 1);
   close(fd);
   if (n <= 0)
   {
      out[0] = '\0';
      return;
   }
   out[n] = '\0';
   for (char *p = out; *p; p++)
   {
      if (*p == '\n' || *p == '\r')
      {
         *p = '\0';
         break;
      }
   }
#endif
}

static long current_rss_kb(void)
{
   struct rusage ru;
   memset(&ru, 0, sizeof(ru));
   if (getrusage(RUSAGE_SELF, &ru) != 0)
      return 0;
   return (long)ru.ru_maxrss;
}

void shutdown_forensics_snapshot(shutdown_ctx_t *out, const char *daemon, int sig,
                                 const void *siginfo, time_t started_at, int inflight_turns,
                                 int inflight_jobs, int inflight_workers)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   snprintf(out->signal_name, sizeof(out->signal_name), "%s", shutdown_forensics_signal_name(sig));
   out->signal_number = sig;
   snprintf(out->daemon, sizeof(out->daemon), "%s", daemon && daemon[0] ? daemon : "unknown");
   out->daemon_pid = getpid();
   out->at = time(NULL);
   out->uptime_s = started_at > 0 && out->at >= started_at ? (long)(out->at - started_at) : 0;
   out->inflight_turns = inflight_turns;
   out->inflight_jobs = inflight_jobs;
   out->inflight_workers = inflight_workers;
   out->rss_kb = current_rss_kb();

#ifndef _WIN32
   const siginfo_t *info = (const siginfo_t *)siginfo;
   out->sender_pid = info ? info->si_pid : 0;
#else
   (void)siginfo;
   out->sender_pid = 0;
#endif
   read_sender_comm(out->sender_pid, out->sender_comm, sizeof(out->sender_comm));
}

static void json_escape(char *dst, size_t dst_len, const char *src)
{
   if (!dst || dst_len == 0)
      return;
   size_t off = 0;
   if (!src)
      src = "";
   while (*src && off + 1 < dst_len)
   {
      unsigned char c = (unsigned char)*src++;
      if ((c == '"' || c == '\\') && off + 2 < dst_len)
      {
         dst[off++] = '\\';
         dst[off++] = (char)c;
      }
      else if (c >= 0x20)
      {
         dst[off++] = (char)c;
      }
   }
   dst[off] = '\0';
}

static int record_path(const shutdown_ctx_t *ctx, char *path, size_t path_len)
{
   const char *dir = shutdown_forensics_dir();
   if (!dir)
      return -1;
   struct tm tm_buf;
   gmtime_r(&ctx->at, &tm_buf);
   char ts[32];
   strftime(ts, sizeof(ts), "%Y%m%dT%H%M%SZ", &tm_buf);
   int n =
       snprintf(path, path_len, "%s/%s-%ld-%s.json", dir, ts, (long)ctx->daemon_pid, ctx->daemon);
   return n > 0 && (size_t)n < path_len ? 0 : -1;
}

static int marker_path(const char *daemon, pid_t pid, char *path, size_t path_len)
{
   const char *dir = shutdown_forensics_dir();
   if (!dir || !daemon || !daemon[0])
      return -1;
   int n = snprintf(path, path_len, "%s/running-%s-%ld.json", dir, daemon, (long)pid);
   return n > 0 && (size_t)n < path_len ? 0 : -1;
}

int shutdown_forensics_write_record(const shutdown_ctx_t *ctx)
{
   if (!ctx)
      return -1;
   const char *dir = shutdown_forensics_dir();
   if (!dir || mkdir_p(dir) != 0)
      return -1;

   char path[4096];
   if (record_path(ctx, path, sizeof(path)) != 0)
      return -1;

   char daemon[64];
   char signal_name[32];
   char sender_comm[128];
   char process_tree[1024];
   json_escape(daemon, sizeof(daemon), ctx->daemon);
   json_escape(signal_name, sizeof(signal_name), ctx->signal_name);
   json_escape(sender_comm, sizeof(sender_comm), ctx->sender_comm);
   json_escape(process_tree, sizeof(process_tree), ctx->process_tree);

   char body[4096];
   int n = snprintf(body, sizeof(body),
                    "{\n"
                    "  \"at\": %lld,\n"
                    "  \"daemon\": \"%s\",\n"
                    "  \"daemon_pid\": %ld,\n"
                    "  \"signal\": \"%s\",\n"
                    "  \"signal_number\": %d,\n"
                    "  \"sender_pid\": %ld,\n"
                    "  \"sender_comm\": \"%s\",\n"
                    "  \"uptime_s\": %ld,\n"
                    "  \"inflight_turns\": %d,\n"
                    "  \"inflight_jobs\": %d,\n"
                    "  \"inflight_workers\": %d,\n"
                    "  \"rss_kb\": %ld,\n"
                    "  \"unclean_exit\": %d,\n"
                    "  \"process_tree\": \"%s\"\n"
                    "}\n",
                    (long long)ctx->at, daemon, (long)ctx->daemon_pid, signal_name,
                    ctx->signal_number, (long)ctx->sender_pid, sender_comm, ctx->uptime_s,
                    ctx->inflight_turns, ctx->inflight_jobs, ctx->inflight_workers, ctx->rss_kb,
                    ctx->unclean_exit, process_tree);
   if (n <= 0 || (size_t)n >= sizeof(body))
      return -1;

   /* Write to a per-pid temp file then rename() over the target so the update is
    * atomic: a concurrent reader (shutdown_forensics_list_recent) sees either the
    * old or the new complete record, never a truncated one. This matters because
    * the async diagnostic child rewrites the same record the parent just wrote
    * (to add the process tree); a reader racing an O_TRUNC rewrite could
    * otherwise observe an empty file and miss the record. */
   char tmppath[4160];
   int tn = snprintf(tmppath, sizeof(tmppath), "%s.tmp.%ld", path, (long)getpid());
   if (tn <= 0 || (size_t)tn >= sizeof(tmppath))
      return -1;
   int fd = open(tmppath, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
   if (fd < 0)
      return -1;
   ssize_t wrote = write(fd, body, (size_t)n);
   close(fd);
   if (wrote != n)
   {
      unlink(tmppath);
      return -1;
   }
#ifdef _WIN32
   /* Windows rename() will not replace an existing file; the atomic-replace race
    * it guards against is POSIX-only (the async diagnostic uses /proc), so a
    * non-atomic remove-then-rename is acceptable here. */
   unlink(path);
#endif
   if (rename(tmppath, path) != 0)
   {
      unlink(tmppath);
      return -1;
   }
   return 0;
}

#ifndef _WIN32
static pid_t read_parent_pid(pid_t pid)
{
   if (pid <= 0)
      return 0;
   char path[64];
   snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   long got_pid = 0;
   char comm[256];
   char state = '\0';
   long ppid = 0;
   int n = fscanf(f, "%ld %255s %c %ld", &got_pid, comm, &state, &ppid);
   fclose(f);
   return n == 4 && got_pid == (long)pid ? (pid_t)ppid : 0;
}

static void build_process_tree(pid_t pid, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   size_t off = 0;
   for (int depth = 0; pid > 0 && depth < 8 && off + 1 < out_len; depth++)
   {
      char comm[64];
      read_sender_comm(pid, comm, sizeof(comm));
      int n = snprintf(out + off, out_len - off, "%s%ld%s%s%s", depth == 0 ? "" : " <- ", (long)pid,
                       comm[0] ? "(" : "", comm[0] ? comm : "", comm[0] ? ")" : "");
      if (n <= 0 || (size_t)n >= out_len - off)
         break;
      off += (size_t)n;
      pid = read_parent_pid(pid);
   }
}
#endif

int shutdown_forensics_spawn_async_diagnostic(const shutdown_ctx_t *ctx)
{
   if (!ctx)
      return -1;
#ifdef _WIN32
   (void)ctx;
   return 0;
#else
   pid_t child = fork();
   if (child < 0)
      return -1;
   if (child == 0)
   {
      (void)setsid();
      shutdown_ctx_t copy = *ctx;
      build_process_tree(copy.sender_pid > 0 ? copy.sender_pid : copy.daemon_pid, copy.process_tree,
                         sizeof(copy.process_tree));
      if (!copy.process_tree[0])
         snprintf(copy.process_tree, sizeof(copy.process_tree), "unavailable");
      (void)shutdown_forensics_write_record(&copy);
      (void)shutdown_forensics_rotate(FORENSICS_KEEP_DEFAULT);
      _exit(0);
   }
   return 0;
#endif
}

int shutdown_forensics_record_signal(const char *daemon, int sig, const void *siginfo,
                                     time_t started_at, int inflight_turns, int inflight_jobs,
                                     int inflight_workers)
{
   shutdown_ctx_t ctx;
   shutdown_forensics_snapshot(&ctx, daemon, sig, siginfo, started_at, inflight_turns,
                               inflight_jobs, inflight_workers);
   int rc = shutdown_forensics_write_record(&ctx);
   (void)shutdown_forensics_mark_stopped(ctx.daemon, ctx.daemon_pid);
   (void)shutdown_forensics_spawn_async_diagnostic(&ctx);
   return rc;
}

int shutdown_forensics_mark_started(const char *daemon, time_t started_at)
{
   const char *dir = shutdown_forensics_dir();
   if (!daemon || !daemon[0] || !dir || mkdir_p(dir) != 0)
      return -1;
   char path[4096];
   pid_t pid = getpid();
   if (marker_path(daemon, pid, path, sizeof(path)) != 0)
      return -1;
   char daemon_escaped[64];
   json_escape(daemon_escaped, sizeof(daemon_escaped), daemon);
   time_t at = started_at > 0 ? started_at : time(NULL);
   char body[512];
   int n = snprintf(body, sizeof(body),
                    "{\n"
                    "  \"at\": %lld,\n"
                    "  \"daemon\": \"%s\",\n"
                    "  \"daemon_pid\": %ld\n"
                    "}\n",
                    (long long)at, daemon_escaped, (long)pid);
   if (n <= 0 || (size_t)n >= sizeof(body))
      return -1;
   int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
   if (fd < 0)
      return -1;
   ssize_t wrote = write(fd, body, (size_t)n);
   close(fd);
   return wrote == n ? 0 : -1;
}

int shutdown_forensics_mark_stopped(const char *daemon, pid_t pid)
{
   char path[4096];
   if (marker_path(daemon, pid > 0 ? pid : getpid(), path, sizeof(path)) != 0)
      return -1;
   return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

static int has_json_suffix(const char *name)
{
   size_t len = name ? strlen(name) : 0;
   return len > 5 && strcmp(name + len - 5, ".json") == 0;
}

static int cmp_names(const void *a, const void *b)
{
   const char *const *sa = (const char *const *)a;
   const char *const *sb = (const char *const *)b;
   return strcmp(*sa, *sb);
}

static int parse_string_field(const char *json, const char *key, char *out, size_t out_len)
{
   if (!json || !key || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   char needle[64];
   snprintf(needle, sizeof(needle), "\"%s\"", key);
   const char *p = strstr(json, needle);
   if (!p)
      return -1;
   p = strchr(p + strlen(needle), ':');
   if (!p)
      return -1;
   p++;
   while (*p && isspace((unsigned char)*p))
      p++;
   if (*p != '"')
      return -1;
   p++;
   size_t off = 0;
   while (*p && *p != '"' && off + 1 < out_len)
   {
      if (*p == '\\' && p[1])
         p++;
      out[off++] = *p++;
   }
   out[off] = '\0';
   return 0;
}

static long parse_long_field(const char *json, const char *key)
{
   char needle[64];
   snprintf(needle, sizeof(needle), "\"%s\"", key);
   const char *p = strstr(json, needle);
   if (!p)
      return 0;
   p = strchr(p + strlen(needle), ':');
   if (!p)
      return 0;
   p++;
   while (*p && isspace((unsigned char)*p))
      p++;
   return strtol(p, NULL, 10);
}

static int parse_record_file(const char *path, shutdown_ctx_t *out)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return -1;
   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   memset(out, 0, sizeof(*out));
   parse_string_field(buf, "daemon", out->daemon, sizeof(out->daemon));
   parse_string_field(buf, "signal", out->signal_name, sizeof(out->signal_name));
   parse_string_field(buf, "sender_comm", out->sender_comm, sizeof(out->sender_comm));
   parse_string_field(buf, "process_tree", out->process_tree, sizeof(out->process_tree));
   out->at = (time_t)parse_long_field(buf, "at");
   out->daemon_pid = (pid_t)parse_long_field(buf, "daemon_pid");
   out->signal_number = (int)parse_long_field(buf, "signal_number");
   out->sender_pid = (pid_t)parse_long_field(buf, "sender_pid");
   out->uptime_s = parse_long_field(buf, "uptime_s");
   out->inflight_turns = (int)parse_long_field(buf, "inflight_turns");
   out->inflight_jobs = (int)parse_long_field(buf, "inflight_jobs");
   out->inflight_workers = (int)parse_long_field(buf, "inflight_workers");
   out->rss_kb = parse_long_field(buf, "rss_kb");
   out->unclean_exit = (int)parse_long_field(buf, "unclean_exit");
   return out->at > 0 && out->daemon[0] ? 0 : -1;
}

static int collect_names(char names[][256], int max_names)
{
   const char *dir = shutdown_forensics_dir();
   if (!dir)
      return 0;
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   int count = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && count < max_names)
   {
      if (strncmp(ent->d_name, "running-", 8) == 0 || !has_json_suffix(ent->d_name))
         continue;
      snprintf(names[count], 256, "%s", ent->d_name);
      count++;
   }
   closedir(d);
   return count;
}

static int collect_prefixed_names(char names[][256], int max_names, const char *prefix)
{
   const char *dir = shutdown_forensics_dir();
   if (!dir)
      return 0;
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   int count = 0;
   size_t prefix_len = strlen(prefix);
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && count < max_names)
   {
      if (strncmp(ent->d_name, prefix, prefix_len) != 0 || !has_json_suffix(ent->d_name))
         continue;
      snprintf(names[count], 256, "%s", ent->d_name);
      count++;
   }
   closedir(d);
   return count;
}

static int process_alive(pid_t pid)
{
   if (pid <= 0)
      return 0;
#ifdef _WIN32
   return 0;
#else
   return kill(pid, 0) == 0 || errno == EPERM;
#endif
}

static int has_clean_record(pid_t pid)
{
   shutdown_ctx_t rows[50];
   int count = shutdown_forensics_list_recent(rows, 50);
   for (int i = 0; i < count; i++)
   {
      if (rows[i].daemon_pid == pid && !rows[i].unclean_exit)
         return 1;
   }
   return 0;
}

int shutdown_forensics_record_unclean_exits(void)
{
   char raw[256][256];
   int count = collect_prefixed_names(raw, 256, "running-");
   const char *dir = shutdown_forensics_dir();
   if (count <= 0 || !dir)
      return 0;
   int recorded = 0;
   for (int i = 0; i < count; i++)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/%s", dir, raw[i]);
      shutdown_ctx_t ctx;
      if (parse_record_file(path, &ctx) != 0)
         continue;
      if (process_alive(ctx.daemon_pid))
         continue;
      if (!has_clean_record(ctx.daemon_pid))
      {
         snprintf(ctx.signal_name, sizeof(ctx.signal_name), "UNCLEAN");
         ctx.signal_number = 0;
         ctx.sender_pid = -1;
         snprintf(ctx.sender_comm, sizeof(ctx.sender_comm), "no clean shutdown record");
         ctx.uptime_s = ctx.at > 0 ? (long)(time(NULL) - ctx.at) : 0;
         ctx.inflight_turns = 0;
         ctx.inflight_jobs = 0;
         ctx.inflight_workers = 0;
         ctx.rss_kb = 0;
         ctx.unclean_exit = 1;
         snprintf(ctx.process_tree, sizeof(ctx.process_tree), "process no longer exists");
         ctx.at = time(NULL);
         if (shutdown_forensics_write_record(&ctx) == 0)
            recorded++;
      }
      unlink(path);
   }
   if (recorded > 0)
      (void)shutdown_forensics_rotate(FORENSICS_KEEP_DEFAULT);
   return recorded;
}

int shutdown_forensics_list_recent(shutdown_ctx_t *rows, size_t cap)
{
   if (!rows || cap == 0)
      return 0;
   char raw[256][256];
   int count = collect_names(raw, 256);
   if (count <= 0)
      return 0;
   char *names[256];
   for (int i = 0; i < count; i++)
      names[i] = raw[i];
   qsort(names, (size_t)count, sizeof(names[0]), cmp_names);

   const char *dir = shutdown_forensics_dir();
   int out = 0;
   for (int i = count - 1; i >= 0 && (size_t)out < cap; i--)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
      if (parse_record_file(path, &rows[out]) == 0)
         out++;
   }
   return out;
}

int shutdown_forensics_rotate(int keep)
{
   if (keep <= 0)
      keep = FORENSICS_KEEP_DEFAULT;
   char raw[256][256];
   int count = collect_names(raw, 256);
   if (count <= keep)
      return 0;
   char *names[256];
   for (int i = 0; i < count; i++)
      names[i] = raw[i];
   qsort(names, (size_t)count, sizeof(names[0]), cmp_names);

   const char *dir = shutdown_forensics_dir();
   for (int i = 0; i < count - keep; i++)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
      unlink(path);
   }
   return 0;
}

void shutdown_forensics_format_summary(const shutdown_ctx_t *ctx, char *buf, size_t buflen)
{
   if (!buf || buflen == 0)
      return;
   if (!ctx)
   {
      buf[0] = '\0';
      return;
   }
   struct tm tm_buf;
   gmtime_r(&ctx->at, &tm_buf);
   char ts[32];
   strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm_buf);
   if (ctx->unclean_exit)
   {
      snprintf(buf, buflen, "%s  %-8s UNCLEAN  exited without a clean shutdown record", ts,
               ctx->daemon);
      return;
   }
   snprintf(buf, buflen, "%s  %-8s %-7s from %ld%s%s  %d turns, %d jobs, %d workers", ts,
            ctx->daemon, ctx->signal_name, (long)ctx->sender_pid, ctx->sender_comm[0] ? " (" : "",
            ctx->sender_comm[0] ? ctx->sender_comm : "", ctx->inflight_turns, ctx->inflight_jobs,
            ctx->inflight_workers);
   if (ctx->sender_comm[0] && strlen(buf) + 2 < buflen)
      strncat(buf, ")", buflen - strlen(buf) - 1);
}
