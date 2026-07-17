/* sandbox_learned.c: capture + persist a delegate project's learned apt toolchain.
 * See sandbox_learned.h. */

#include "sandbox_learned.h"

#include "aimee.h"      /* MAX_PATH_LEN */
#include "aimee_home.h" /* aimee_home() */
#include "cJSON.h"
#include "config.h"     /* config_t, config_load */
#include "guardrails.h" /* git_repo_root */
#include "platform_path.h"
#include "util.h" /* safe_exec_capture_* not needed; kept for platform helpers */

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* --- pure helpers --- */

/* Debian package-name grammar (leading alnum, then [a-z0-9._+:-]). Rejects paths,
 * shell metacharacters, and flag-looking tokens. */
static int pkg_valid(const char *s)
{
   if (!s || !s[0])
      return 0;
   if (!isalnum((unsigned char)s[0]))
      return 0;
   for (const char *c = s; *c; c++)
      if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '_' || *c == '+' || *c == ':' ||
            *c == '-'))
         return 0;
   return 1;
}

/* True when `tok` is a shell operator/separator that ends an apt command segment. */
static int is_operator_tok(const char *tok)
{
   return strcmp(tok, "&&") == 0 || strcmp(tok, "||") == 0 || strcmp(tok, "|") == 0 ||
          strcmp(tok, ";") == 0 || strcmp(tok, "&") == 0 || strcmp(tok, "\n") == 0;
}

/* A leading `VAR=value` environment assignment (skipped before the command word). */
static int is_env_assign(const char *tok)
{
   const char *eq = strchr(tok, '=');
   if (!eq || eq == tok)
      return 0;
   for (const char *c = tok; c < eq; c++)
      if (!(isalnum((unsigned char)*c) || *c == '_'))
         return 0;
   return 1;
}

/* True when `tok` is an apt option whose ARGUMENT is a SEPARATE following token (e.g.
 * `-t bookworm`, `--option Foo::Bar=1`). The attached forms (`-t=x`, `--option=x`,
 * `-oDebug=1`) carry their value in the same token and are just skipped as flags, so
 * they are not listed here. Recognising these prevents an option's value (a release
 * name, an apt config string) from being mis-recorded as a package. */
static int apt_option_takes_arg(const char *tok)
{
   static const char *const with_arg[] = {"-t",
                                          "-o",
                                          "-c",
                                          "-a",
                                          "--target-release",
                                          "--default-release",
                                          "--option",
                                          "--config-file",
                                          "--arch",
                                          NULL};
   for (int i = 0; with_arg[i]; i++)
      if (strcmp(tok, with_arg[i]) == 0)
         return 1;
   return 0;
}

static int already_have(char out[][SBX_PKG_MAX], int n, const char *name)
{
   for (int i = 0; i < n; i++)
      if (strcmp(out[i], name) == 0)
         return 1;
   return 0;
}

/* Tokenise `cmd` into `tokv` (pointers into the mutable copy `buf`), splitting on
 * whitespace and emitting shell operators (&&, ||, |, ;, &, newline) as their own
 * tokens. Returns the token count (capped at cap). */
static int is_delim(char c)
{
   return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';' || c == '|' || c == '&';
}

static int tokenize(char *buf, const char **tokv, int cap)
{
   int n = 0;
   char *p = buf;
   while (*p && n < cap)
   {
      while (*p == ' ' || *p == '\t' || *p == '\r')
         p++;
      if (!*p)
         break;
      /* scan a word up to (not into) the next delimiter */
      char *start = p;
      while (*p && !is_delim(*p))
         p++;
      /* d/d2 capture the delimiter BEFORE we terminate the word in place; operator
       * tokens are emitted as string literals, never as pointers into buf. */
      char d = *p;
      char d2 = d ? p[1] : '\0';
      if (*p)
         *p = '\0'; /* terminate the word (overwrites the delimiter's first char) */
      if (p > start && n < cap)
         tokv[n++] = start;
      if (n >= cap)
         break;
      if (d == '&' && d2 == '&')
      {
         tokv[n++] = "&&";
         p += 2;
      }
      else if (d == '|' && d2 == '|')
      {
         tokv[n++] = "||";
         p += 2;
      }
      else if (d == '|')
      {
         tokv[n++] = "|";
         p++;
      }
      else if (d == ';')
      {
         tokv[n++] = ";";
         p++;
      }
      else if (d == '&')
      {
         tokv[n++] = "&";
         p++;
      }
      else if (d == '\n')
      {
         tokv[n++] = "\n";
         p++;
      }
      else if (d) /* whitespace: consume the one char we terminated on */
         p++;
   }
   return n;
}

int sandbox_learned_parse_apt(const char *cmd, char out[][SBX_PKG_MAX], int max)
{
   if (!cmd || !out || max <= 0)
      return 0;
   size_t len = strlen(cmd);
   char *buf = malloc(len + 1);
   if (!buf)
      return 0;
   memcpy(buf, cmd, len + 1);

   enum
   {
      MAX_TOK = 4096
   };
   const char **tokv = calloc(MAX_TOK, sizeof(char *));
   if (!tokv)
   {
      free(buf);
      return 0;
   }
   int nt = tokenize(buf, tokv, MAX_TOK);

   int n = 0;
   int i = 0;
   while (i < nt && n < max)
   {
      /* Start of a command segment: skip leading env-assignments, `sudo`, and any
       * flags between sudo and the command word (sudo -E / -u / --preserve-env ...). */
      while (i < nt && (is_env_assign(tokv[i]) || strcmp(tokv[i], "sudo") == 0 ||
                        (i > 0 && strcmp(tokv[i - 1], "sudo") == 0 && tokv[i][0] == '-')))
         i++;
      if (i >= nt)
         break;
      /* the command word must be apt / apt-get, else skip to the next segment. */
      int is_apt = (strcmp(tokv[i], "apt") == 0 || strcmp(tokv[i], "apt-get") == 0);
      if (!is_apt)
      {
         while (i < nt && !is_operator_tok(tokv[i]))
            i++;
         if (i < nt)
            i++; /* step past the operator (guarded: never index past nt) */
         continue;
      }
      i++; /* consume apt/apt-get */
      int seen_install = 0;
      while (i < nt && !is_operator_tok(tokv[i]) && n < max)
      {
         const char *t = tokv[i++];
         /* A value-taking option (`-t bookworm`) consumes its following token so the
          * value is never mistaken for a package or a subcommand — in either position. */
         if (t[0] == '-' && apt_option_takes_arg(t))
         {
            if (i < nt && !is_operator_tok(tokv[i]))
               i++;
            continue;
         }
         if (!seen_install)
         {
            if (strcmp(t, "install") == 0)
               seen_install = 1;
            else if (t[0] == '-')
               continue; /* attached/flag option before the subcommand: apt-get -y install */
            else
               break; /* a different subcommand (update/remove/...) — not an install */
            continue;
         }
         if (t[0] == '-')
            continue; /* -y, --no-install-recommends, attached -oDebug=1, etc. */
         /* Reject anything path- or URL-shaped (./x.deb, https://host/pkg): a package
          * name has no '/'. Accept a bare `pkg=version` pin or `pkg:arch` multiarch by
          * taking the name up to '=' (the release/version follows). */
         if (strchr(t, '/'))
            continue;
         char name[SBX_PKG_MAX];
         size_t j = 0;
         for (const char *c = t; *c && *c != '=' && j + 1 < sizeof(name); c++)
            name[j++] = *c;
         name[j] = '\0';
         if (pkg_valid(name) && !already_have(out, n, name))
            snprintf(out[n++], SBX_PKG_MAX, "%s", name);
      }
      if (i < nt && is_operator_tok(tokv[i]))
         i++; /* step past the operator that ended this segment */
   }

   free(tokv);
   free(buf);
   return n;
}

/* --- store (JSON sidecar under AIMEE_HOME) --- */

static int learned_store_path(char *out, size_t cap)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   int nn = snprintf(out, cap, "%s/sandbox-learned.json", home);
   return (nn > 0 && (size_t)nn < cap) ? 0 : -1;
}

static cJSON *learned_load_doc(void)
{
   char path[MAX_PATH_LEN];
   if (learned_store_path(path, sizeof(path)) != 0)
      return NULL;
   FILE *fp = fopen(path, "r");
   if (!fp)
      return NULL;
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return NULL;
   }
   long sz = ftell(fp);
   if (sz <= 0 || sz > (1 << 20) || fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      return NULL;
   }
   char *txt = malloc((size_t)sz + 1);
   if (!txt)
   {
      fclose(fp);
      return NULL;
   }
   size_t rd = fread(txt, 1, (size_t)sz, fp);
   fclose(fp);
   txt[rd] = '\0';
   cJSON *doc = cJSON_Parse(txt);
   free(txt);
   return doc;
}

static int str_cmp_qsort(const void *a, const void *b)
{
   /* Elements are char[SBX_PKG_MAX] buffers, so a/b already point at the strings. */
   return strcmp((const char *)a, (const char *)b);
}

int sandbox_learned_load(const char *git_root, char out[][SBX_PKG_MAX], int max)
{
   if (!git_root || !git_root[0] || !out || max <= 0)
      return 0;
   cJSON *doc = learned_load_doc();
   if (!doc)
      return 0;
   int n = 0;
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(doc, git_root);
   if (cJSON_IsArray(arr))
   {
      cJSON *e;
      cJSON_ArrayForEach(e, arr)
      {
         if (n >= max)
            break;
         if (cJSON_IsString(e) && pkg_valid(e->valuestring))
            snprintf(out[n++], SBX_PKG_MAX, "%s", e->valuestring);
      }
   }
   cJSON_Delete(doc);
   /* Sort so the derived Dockerfile — and thus the content-hash image tag — is stable
    * regardless of insertion order. */
   qsort(out, (size_t)n, sizeof(out[0]), str_cmp_qsort);
   return n;
}

/* Serialises the load-modify-write against concurrent delegate turns in THIS process
 * (fast path); the flock below serialises it across PROCESSES sharing one AIMEE_HOME,
 * so no update — from either — is lost. */
static pthread_mutex_t g_learned_lock = PTHREAD_MUTEX_INITIALIZER;

/* Open+exclusively-flock a stable lock file under AIMEE_HOME. Returns the fd (held for
 * the transaction) or -1 if unavailable — in which case we proceed unlocked rather than
 * fail (best-effort store; the process mutex still covers the common single-process
 * case). Caller closes the fd (which releases the lock). */
static int learned_lock_fd(void)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   char lp[MAX_PATH_LEN];
   if (snprintf(lp, sizeof(lp), "%s/sandbox-learned.lock", home) >= (int)sizeof(lp))
      return -1;
   int fd = open(lp, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
   if (fd < 0)
      return -1;
   if (flock(fd, LOCK_EX) != 0)
   {
      close(fd);
      return -1;
   }
   return fd;
}

int sandbox_learned_record(const char *git_root, const char *const *pkgs, int n)
{
   if (!git_root || !git_root[0] || !pkgs || n <= 0)
      return 0;
   pthread_mutex_lock(&g_learned_lock);
   int lock_fd = learned_lock_fd(); /* cross-process; -1 => proceed best-effort */
   cJSON *doc = learned_load_doc();
   if (!doc)
      doc = cJSON_CreateObject();
   if (!doc)
   {
      if (lock_fd >= 0)
         close(lock_fd);
      pthread_mutex_unlock(&g_learned_lock);
      return -1;
   }

   cJSON *arr = cJSON_GetObjectItemCaseSensitive(doc, git_root);
   if (!cJSON_IsArray(arr))
   {
      arr = cJSON_CreateArray();
      cJSON_AddItemToObject(doc, git_root, arr);
   }

   int changed = 0;
   for (int i = 0; i < n; i++)
   {
      if (!pkgs[i] || !pkg_valid(pkgs[i]))
         continue;
      if (cJSON_GetArraySize(arr) >= SBX_LEARN_MAX)
         break;
      int have = 0;
      cJSON *e;
      cJSON_ArrayForEach(e, arr)
      {
         if (cJSON_IsString(e) && strcmp(e->valuestring, pkgs[i]) == 0)
         {
            have = 1;
            break;
         }
      }
      if (!have)
      {
         cJSON_AddItemToArray(arr, cJSON_CreateString(pkgs[i]));
         changed = 1;
      }
   }

   int rc = 0;
   if (changed)
   {
      char path[MAX_PATH_LEN];
      char *txt = cJSON_PrintUnformatted(doc);
      if (learned_store_path(path, sizeof(path)) == 0 && txt)
      {
         /* atomic write: per-writer-unique tmp + rename (so a concurrent writer in
          * another process cannot interleave into a shared tmp inode). */
         char tmp[MAX_PATH_LEN];
         rc = -1;
         if (snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%lu", path, (int)getpid(),
                      (unsigned long)pthread_self()) < (int)sizeof(tmp))
         {
            FILE *fp = fopen(tmp, "w");
            if (fp)
            {
               if (fputs(txt, fp) >= 0 && fclose(fp) == 0 && rename(tmp, path) == 0)
                  rc = 0;
               else
                  remove(tmp);
            }
         }
      }
      else
         rc = -1;
      free(txt);
   }
   cJSON_Delete(doc);
   if (lock_fd >= 0)
      close(lock_fd); /* releases the flock */
   pthread_mutex_unlock(&g_learned_lock);
   return rc;
}

void sandbox_learned_observe(const char *cwd, const char *cmd)
{
   if (!cwd || !cwd[0] || !cmd || !cmd[0])
      return;
   /* Cheap pre-filter: only pay for anything when the command mentions apt at all. */
   if (!strstr(cmd, "apt"))
      return;

   /* Parse (pure, cheap) BEFORE the config read and the git subprocess, so an
    * incidental "apt" substring (adapter, chapter, ...) costs nothing beyond the parse. */
   char pkgs[SBX_LEARN_MAX][SBX_PKG_MAX];
   int n = sandbox_learned_parse_apt(cmd, pkgs, SBX_LEARN_MAX);
   if (n <= 0)
      return;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.delegate_sandbox_learn_packages)
      return;

   char git_root[MAX_PATH_LEN];
   if (git_repo_root(cwd, git_root, sizeof(git_root)) != 0)
      return;

   const char *ptrs[SBX_LEARN_MAX];
   for (int i = 0; i < n; i++)
      ptrs[i] = pkgs[i];
   (void)sandbox_learned_record(git_root, ptrs, n);
}
