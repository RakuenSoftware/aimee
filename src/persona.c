/* persona.c: server-owned persona registry (see persona.h).
 *
 * Server-side only — the thin client reaches personas over the /v1 HTTP API,
 * never by reading these files. So this freely uses prompts.o / yaml.o /
 * config.o (all server-linked). Built-in PROSE stays in prompts.c (enum path,
 * byte-identical engineer); this file owns built-in METADATA and loads custom
 * single-file persona definitions. */
#include "persona.h"
#include "aimee.h"   /* MAX_PATH_LEN */
#include "prompts.h" /* aimee_mode_t, prompt_persona_text, prompt_principles_text */
#include "config.h"  /* config_default_dir */
#include "yaml.h"    /* yaml_parse */
#include "util.h"    /* safe_strdup */
#include "cJSON.h"
#include "platform_path.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Built-in persona metadata (prose lives in prompts.c) ---------------- */

typedef struct
{
   const char *name;
   aimee_mode_t mode;
   const char *description;
   const char *roles_csv;
   const char *check_role;
   const char *check_marker;
   const char *delegates; /* "full" | "readonly" | "none" */
} builtin_persona_t;

static const char *NOVEL_BRIEF =
    "- Use `aimee index find <name>` / `aimee memory search <terms>` to locate scenes and "
    "recall established canon (characters, places, events).\n"
    "- Recall the relevant world state before writing or revising a scene.\n"
    "- You write the prose yourself; use READ-ONLY delegates only — `aimee delegate continuity "
    "\"check this scene\"` to reconcile a draft against the world bible.\n";

static const char *SONG_BRIEF =
    "- Use `aimee index find <name>` / `aimee memory search <terms>` to locate sections and "
    "recall the song's voice, hook, and structure.\n"
    "- Recall the song's concept and rhyme scheme before writing a section.\n"
    "- You do all the work yourself: drafting, hooks, and checking meter/rhyme. You do not have "
    "delegates.\n";

/* Reviewer-mode briefs. All four are read-only: they investigate and report,
 * using read-only delegates only. */
static const char *QA_BRIEF =
    "- Use `aimee index find <name>` / `aimee memory search <terms>` to locate the code under "
    "review and any prior QA findings.\n"
    "- Establish the intended behaviour and existing tests before probing for defects.\n"
    "- You produce the review yourself; use READ-ONLY delegates only — `aimee delegate validate "
    "\"...\"` (or review, diagnose, research) to investigate in parallel.\n";

static const char *SECURITY_BRIEF =
    "- Use `aimee index find <name>` / `aimee memory search <terms>` to locate the code under "
    "review and any prior security findings.\n"
    "- Establish the trust boundaries and untrusted inputs before tracing for vulnerabilities.\n"
    "- You produce the review yourself; use READ-ONLY delegates only — `aimee delegate review "
    "\"...\"` (or diagnose, validate, research) to investigate in parallel.\n";

static const char *REVIEWER_BRIEF =
    "- Use `aimee index find <name>` / `aimee memory search <terms>` to locate the change under "
    "review and the code around it.\n"
    "- Review the whole change breadth-first; challenge the happy-path explanation before "
    "accepting it.\n"
    "- You produce the review yourself; use READ-ONLY delegates only — `aimee delegate review "
    "\"...\"` (or diagnose, validate, research) to investigate in parallel.\n";

static const char *ARCHITECT_BRIEF =
    "- Use `aimee index find <name>` / `aimee memory search <terms>` to locate the change and the "
    "modules and contracts it touches.\n"
    "- Map the affected boundaries and dependencies before judging the design.\n"
    "- You produce the review yourself; use READ-ONLY delegates only — `aimee delegate review "
    "\"...\"` (or diagnose, validate, research) to investigate in parallel.\n";

static const char *REVIEWER_CONSTRUCTIVE_BRIEF =
    "- Use `aimee index find <name>` / `aimee memory search <terms>` to locate the change under "
    "review and the code around it.\n"
    "- Assess the change as written: confirm what it gets right and establish whether it meets its "
    "stated goal before listing what is missing.\n"
    "- You produce the review yourself; use READ-ONLY delegates only — `aimee delegate review "
    "\"...\"` (or diagnose, validate, research) to investigate in parallel.\n";

/* Built-ins. Roles are the delegate roles the persona may use, consistent with
 * its delegate policy (engineer: full set; novel: read-only checks only;
 * songwriter: none). */
static const builtin_persona_t g_builtins[] = {
    {"engineer", AIMEE_MODE_ENGINEER, "Autonomous software engineer (default)",
     "code,review,deploy,validate,test,diagnose,execute,refactor", "", "", "full"},
    {"novel", AIMEE_MODE_NOVEL, "Fiction author / worldbuilder",
     "continuity,beat-check,review,research", "continuity", "CONTINUITY", "readonly"},
    {"songwriter", AIMEE_MODE_SONGWRITER, "Songwriter / lyricist", "", "", "", "none"},
    {"qa", AIMEE_MODE_QA, "Senior QA / test reviewer", "review,diagnose,validate,research", "", "",
     "readonly"},
    {"security", AIMEE_MODE_SECURITY, "Senior application-security reviewer",
     "review,diagnose,validate,research", "", "", "readonly"},
    {"reviewer", AIMEE_MODE_REVIEWER, "Senior contrarian code reviewer (thorough, comprehensive)",
     "review,diagnose,validate,research", "", "", "readonly"},
    {"architect", AIMEE_MODE_ARCHITECT, "Software-architecture reviewer",
     "review,diagnose,validate,research", "", "", "readonly"},
    {"reviewer-constructive", AIMEE_MODE_REVIEWER_CONSTRUCTIVE,
     "Senior constructive code reviewer (assess as written)", "review,diagnose,validate,research",
     "", "", "readonly"},
    {NULL, 0, NULL, NULL, NULL, NULL, NULL},
};

int persona_delegates_enabled(const persona_t *p)
{
   return p && strcmp(p->delegates, PERSONA_DELEGATES_NONE) != 0;
}

int persona_delegates_writes(const persona_t *p)
{
   return p && strcmp(p->delegates, PERSONA_DELEGATES_FULL) == 0;
}

void persona_delegation_block(const persona_t *p, char *buf, size_t n)
{
   if (!p || !buf || !n)
      return;
   char roles[256] = "";
   size_t rp = 0;
   for (int i = 0; i < p->roles_count && rp < sizeof(roles) - 1; i++)
      rp += (size_t)snprintf(roles + rp, sizeof(roles) - rp, "%s%s", rp ? ", " : "", p->roles[i]);

   if (!persona_delegates_enabled(p))
      snprintf(buf, n,
               "# Delegation (IMPORTANT)\n"
               "You do NOT have delegates — `aimee delegate` is disabled for the %s persona. Do "
               "all of the work yourself.\n\n",
               p->name);
   else if (!persona_delegates_writes(p))
      snprintf(buf, n,
               "# Delegation (IMPORTANT)\n"
               "You do the writing and editing yourself. You may use READ-ONLY delegates ONLY, "
               "for review and checks: `aimee delegate <role> --persona <name> \"prompt\"`. A "
               "persona is REQUIRED (e.g. engineer, qa, security, reviewer, architect). Write "
               "delegates are refused.\n"
               "Read-only roles: %s.\n\n",
               roles[0] ? roles : "review, research");
   else
      snprintf(buf, n,
               "# Delegation (IMPORTANT)\n"
               "ALWAYS delegate multi-file changes, infrastructure work, deployments, service "
               "checks, and parallel work via `aimee delegate <role> --persona <name> \"prompt\"` "
               "— do NOT do that work inline. A persona is REQUIRED (e.g. engineer, qa, security, "
               "reviewer, architect). Always use aimee delegates over the Agent tool (the Agent "
               "tool is disabled in aimee).\n"
               "Roles: %s. Options: --persona <name> (required), --tools, --background, --retry N, "
               "--verify CMD.\n\n",
               roles[0] ? roles
                        : "code, review, deploy, validate, test, diagnose, execute, "
                          "refactor");
}

static const char *builtin_brief(const builtin_persona_t *b)
{
   if (b->mode == AIMEE_MODE_NOVEL)
      return NOVEL_BRIEF;
   if (b->mode == AIMEE_MODE_SONGWRITER)
      return SONG_BRIEF;
   if (b->mode == AIMEE_MODE_QA)
      return QA_BRIEF;
   if (b->mode == AIMEE_MODE_SECURITY)
      return SECURITY_BRIEF;
   if (b->mode == AIMEE_MODE_REVIEWER)
      return REVIEWER_BRIEF;
   if (b->mode == AIMEE_MODE_ARCHITECT)
      return ARCHITECT_BRIEF;
   if (b->mode == AIMEE_MODE_REVIEWER_CONSTRUCTIVE)
      return REVIEWER_CONSTRUCTIVE_BRIEF;
   return "";
}

static const builtin_persona_t *builtin_find(const char *name)
{
   if (!name)
      return NULL;
   for (int i = 0; g_builtins[i].name; i++)
      if (strcmp(g_builtins[i].name, name) == 0)
         return &g_builtins[i];
   return NULL;
}

int persona_is_builtin(const char *name)
{
   return builtin_find(name) != NULL;
}

/* --- helpers ------------------------------------------------------------- */

static char *read_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long len = ftell(f);
   if (len < 0 || len > PERSONA_FILE_MAX_SIZE)
   {
      fclose(f);
      return NULL;
   }
   rewind(f);
   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)len, f);
   fclose(f);
   buf[n] = '\0';
   return buf;
}

/* Trim trailing whitespace/newlines in place. */
static void rtrim(char *s)
{
   size_t n = strlen(s);
   while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
      s[--n] = '\0';
}

/* Extract the body of a "## <header>" section: text after the header line up
 * to (but not including) the next "## " header or EOF. Returns a heap copy
 * (trailing whitespace trimmed) or NULL if the section is absent. */
static char *extract_section(const char *body, const char *header)
{
   char needle[64];
   snprintf(needle, sizeof(needle), "## %s", header);
   const char *p = body;
   size_t nlen = strlen(needle);
   while (p && *p)
   {
      const char *line = p;
      int at_bol = (line == body || line[-1] == '\n');
      if (at_bol && strncmp(line, needle, nlen) == 0 &&
          (line[nlen] == '\n' || line[nlen] == '\r' || line[nlen] == '\0'))
      {
         const char *start = strchr(line, '\n');
         if (!start)
            return NULL;
         start++;
         /* find next "## " at beginning of a line */
         const char *end = start;
         while (*end)
         {
            if ((end == start || end[-1] == '\n') && strncmp(end, "## ", 3) == 0)
               break;
            end++;
         }
         size_t len = (size_t)(end - start);
         char *out = malloc(len + 1);
         if (!out)
            return NULL;
         memcpy(out, start, len);
         out[len] = '\0';
         rtrim(out);
         return out[0] ? out : (free(out), NULL);
      }
      p = strchr(line, '\n');
      if (p)
         p++;
   }
   return NULL;
}

static void set_roles_from_csv(persona_t *out, const char *csv)
{
   out->roles_count = 0;
   if (!csv)
      return;
   char buf[512];
   snprintf(buf, sizeof(buf), "%s", csv);
   char *save = NULL;
   for (char *tok = strtok_r(buf, ", \t[]", &save); tok && out->roles_count < PERSONA_MAX_ROLES;
        tok = strtok_r(NULL, ", \t[]", &save))
   {
      if (tok[0])
         snprintf(out->roles[out->roles_count++], PERSONA_NAME_MAX, "%s", tok);
   }
}

/* --- path resolution ----------------------------------------------------- */

int persona_path(const char *project_root, const char *name, char *buf, size_t bufsz)
{
   if (!name || !buf || bufsz == 0)
      return -1;
   struct stat st;
   if (project_root && project_root[0])
   {
      snprintf(buf, bufsz, "%s/.aimee/personas/%s.md", project_root, name);
      if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
         return 0;
   }
   snprintf(buf, bufsz, "%s/personas/%s.md", config_default_dir(), name);
   if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
      return 0;
   buf[0] = '\0';
   return -1;
}

/* --- load ---------------------------------------------------------------- */

static void load_builtin(const builtin_persona_t *b, persona_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->name, sizeof(out->name), "%s", b->name);
   snprintf(out->description, sizeof(out->description), "%s", b->description);
   set_roles_from_csv(out, b->roles_csv);
   snprintf(out->check_role, sizeof(out->check_role), "%s", b->check_role);
   snprintf(out->check_marker, sizeof(out->check_marker), "%s", b->check_marker);
   snprintf(out->delegates, sizeof(out->delegates), "%s", b->delegates);
   const char *brief = builtin_brief(b);
   out->brief_text = (brief && brief[0]) ? safe_strdup(brief) : NULL;
   out->builtin = 1;
   /* prose (persona_text/principles_text) left NULL: callers use the enum path
    * (prompts.c) for built-in prose. */
}

static int load_file(const char *path, const char *name, persona_t *out)
{
   char *raw = read_file(path);
   if (!raw)
      return -1;
   memset(out, 0, sizeof(*out));
   snprintf(out->name, sizeof(out->name), "%s", name);

   /* Frontmatter: a leading "---\n ... \n---" YAML block. */
   const char *body = raw;
   if (strncmp(raw, "---\n", 4) == 0 || strncmp(raw, "---\r\n", 5) == 0)
   {
      const char *fm_start = strchr(raw, '\n') + 1;
      const char *fm_end = strstr(fm_start, "\n---");
      if (fm_end)
      {
         size_t fm_len = (size_t)(fm_end - fm_start);
         char *fm = malloc(fm_len + 1);
         if (fm)
         {
            memcpy(fm, fm_start, fm_len);
            fm[fm_len] = '\0';
            cJSON *meta = yaml_parse(fm);
            free(fm);
            if (meta)
            {
               cJSON *d = cJSON_GetObjectItemCaseSensitive(meta, "description");
               if (cJSON_IsString(d))
                  snprintf(out->description, sizeof(out->description), "%s", d->valuestring);
               cJSON *cr = cJSON_GetObjectItemCaseSensitive(meta, "check_role");
               if (cJSON_IsString(cr))
                  snprintf(out->check_role, sizeof(out->check_role), "%s", cr->valuestring);
               cJSON *cm = cJSON_GetObjectItemCaseSensitive(meta, "check_marker");
               if (cJSON_IsString(cm))
                  snprintf(out->check_marker, sizeof(out->check_marker), "%s", cm->valuestring);
               cJSON *dg = cJSON_GetObjectItemCaseSensitive(meta, "delegates");
               if (cJSON_IsString(dg) && dg->valuestring[0])
                  snprintf(out->delegates, sizeof(out->delegates), "%s", dg->valuestring);
               cJSON *roles = cJSON_GetObjectItemCaseSensitive(meta, "roles");
               if (cJSON_IsArray(roles))
               {
                  cJSON *r = NULL;
                  cJSON_ArrayForEach(r, roles)
                  {
                     if (cJSON_IsString(r) && out->roles_count < PERSONA_MAX_ROLES)
                        snprintf(out->roles[out->roles_count++], PERSONA_NAME_MAX, "%s",
                                 r->valuestring);
                  }
               }
               else if (cJSON_IsString(roles))
                  set_roles_from_csv(out, roles->valuestring);
               cJSON_Delete(meta);
            }
         }
         /* advance body past the closing "---" line */
         const char *after = strchr(fm_end + 1, '\n');
         body = after ? after + 1 : "";
      }
   }

   out->persona_text = extract_section(body, "Persona");
   out->principles_text = extract_section(body, "Principles");
   out->brief_text = extract_section(body, "Brief");
   if (!out->delegates[0])
      snprintf(out->delegates, sizeof(out->delegates), "%s", PERSONA_DELEGATES_FULL);
   /* A built-in seeded to disk stays flagged built-in: the flag means "this is a
    * built-in persona name" (so clients label it as such), while its prose now
    * comes from the file. Custom personas are not built-ins. */
   out->builtin = persona_is_builtin(name) ? 1 : 0;
   free(raw);
   return 0;
}

int persona_load(const char *project_root, const char *name, persona_t *out)
{
   if (!name || !out)
      return -1;

   char path[PERSONA_PATH_MAX];
   if (persona_path(project_root, name, path, sizeof(path)) == 0)
   {
      if (load_file(path, name, out) == 0)
         return 0;
   }

   const builtin_persona_t *b = builtin_find(name);
   if (b)
   {
      load_builtin(b, out);
      return 0;
   }

   /* Unknown -> engineer fallback (always usable). */
   load_builtin(builtin_find("engineer"), out);
   return 0;
}

void persona_free(persona_t *p)
{
   if (!p)
      return;
   free(p->persona_text);
   free(p->principles_text);
   free(p->brief_text);
   p->persona_text = p->principles_text = p->brief_text = NULL;
}

/* --- delegate/session prompt composition --------------------------------- */

/* Substitute the first "%s" in persona prose with cwd, without printf so a
 * stray % in a user persona file cannot inject a format directive. Returns a
 * heap copy (caller frees); "" for NULL input. */
static char *persona_apply_cwd(const char *text, const char *cwd)
{
   if (!text)
      return safe_strdup("");
   const char *pct = strstr(text, "%s");
   if (!pct)
      return safe_strdup(text);
   const char *c = (cwd && cwd[0]) ? cwd : ".";
   size_t prefix = (size_t)(pct - text), clen = strlen(c), suffix = strlen(pct + 2);
   char *out = malloc(prefix + clen + suffix + 1);
   if (!out)
      return safe_strdup("");
   memcpy(out, text, prefix);
   memcpy(out + prefix, c, clen);
   memcpy(out + prefix + clen, pct + 2, suffix + 1);
   return out;
}

char *persona_identity_prose(const persona_t *p, const char *cwd)
{
   if (!p)
      return NULL;
   /* Return the persona's COMPLETE identity prose. Built-ins keep their full
    * block in code (prompts.c) — a built-in's file `## Persona` may be only a
    * thin one-liner, with the rest of the framing in sibling sections that
    * load_file does not extract — so a built-in uses its enum prose; a custom
    * persona uses its file `## Persona` body. */
   if (p->builtin)
   {
      const char *prose = prompt_persona_text(aimee_mode_from_string(p->name));
      return (prose && prose[0]) ? persona_apply_cwd(prose, cwd) : NULL;
   }
   if (p->persona_text && p->persona_text[0])
      return persona_apply_cwd(p->persona_text, cwd);
   return NULL;
}

char *persona_compose_delegate_prompt(const char *name, const char *cwd, const char *base_prompt)
{
   const char *base = base_prompt ? base_prompt : "";
   persona_t p;
   persona_load(NULL, name, &p); /* always yields a usable persona (engineer fallback) */

   /* Identity prose (the "You are ..." block) + principles for the persona.
    * Source of truth is the persona's config file: prefer the file-loaded prose
    * (set once the persona is seeded/edited on disk), and fall back to the
    * built-in prose in code only when no file backs it.
    *
    * The engineer built-in is special: its persona text is the full primary
    * system prompt, which is wrong to inject into a small delegate. So engineer
    * contributes only principles — no identity prose — keeping an engineer
    * delegate's principles-only framing regardless of whether engineer.md
    * exists. */
   aimee_mode_t mode = aimee_mode_from_string(p.name);
   const char *principles = p.principles_text ? p.principles_text : prompt_principles_text(mode);
   char *identity = NULL;
   int is_engineer = (strcmp(p.name, "engineer") == 0);
   if (!is_engineer)
   {
      if (p.persona_text)
         identity = persona_apply_cwd(p.persona_text, cwd); /* from config file */
      else if (p.builtin)
         identity = persona_apply_cwd(prompt_persona_text(mode), cwd); /* fallback */
   }

   int has_identity = identity && identity[0];
   size_t plen = strlen(principles);
   size_t ilen = has_identity ? strlen(identity) : 0;
   size_t blen = strlen(base);
   /* principles (ends in a blank line) + [identity + blank line] + base */
   char *out = malloc(plen + (has_identity ? ilen + 1 : 0) + blen + 1);
   if (out)
   {
      size_t off = 0;
      memcpy(out + off, principles, plen);
      off += plen;
      if (has_identity)
      {
         memcpy(out + off, identity, ilen);
         off += ilen;
         out[off++] = '\n';
      }
      memcpy(out + off, base, blen + 1);
   }
   free(identity);
   persona_free(&p);
   return out ? out : safe_strdup("");
}

/* --- list ---------------------------------------------------------------- */

static void scan_dir(const char *dir, char names[][PERSONA_NAME_MAX], int *count, int max)
{
   DIR *d = opendir(dir);
   if (!d)
      return;
   struct dirent *e;
   while ((e = readdir(d)) != NULL && *count < max)
   {
      size_t n = strlen(e->d_name);
      if (n <= 3 || strcmp(e->d_name + n - 3, ".md") != 0)
         continue;
      char base[PERSONA_NAME_MAX];
      size_t bn = n - 3;
      if (bn >= sizeof(base))
         continue;
      memcpy(base, e->d_name, bn);
      base[bn] = '\0';
      int dup = 0;
      for (int i = 0; i < *count; i++)
         if (strcmp(names[i], base) == 0)
         {
            dup = 1;
            break;
         }
      if (!dup)
         snprintf(names[(*count)++], PERSONA_NAME_MAX, "%s", base);
   }
   closedir(d);
}

int persona_list(const char *project_root, char names_out[][PERSONA_NAME_MAX], int max_names)
{
   if (!names_out || max_names <= 0)
      return 0;
   int count = 0;
   if (project_root && project_root[0])
   {
      char dir[PERSONA_PATH_MAX];
      snprintf(dir, sizeof(dir), "%s/.aimee/personas", project_root);
      scan_dir(dir, names_out, &count, max_names);
   }
   char dir[PERSONA_PATH_MAX];
   snprintf(dir, sizeof(dir), "%s/personas", config_default_dir());
   scan_dir(dir, names_out, &count, max_names);
   for (int i = 0; g_builtins[i].name && count < max_names; i++)
   {
      int dup = 0;
      for (int j = 0; j < count; j++)
         if (strcmp(names_out[j], g_builtins[i].name) == 0)
         {
            dup = 1;
            break;
         }
      if (!dup)
         snprintf(names_out[count++], PERSONA_NAME_MAX, "%s", g_builtins[i].name);
   }
   return count;
}

/* --- install defaults ---------------------------------------------------- */

int persona_install_defaults(const char *dir)
{
   if (!dir || !dir[0])
      return -1;

   struct stat st;
   if (stat(dir, &st) != 0)
      platform_mkdir_p(dir, 0755);

   int written = 0;
   for (int i = 0; g_builtins[i].name; i++)
   {
      const builtin_persona_t *b = &g_builtins[i];
      char path[PERSONA_PATH_MAX];
      snprintf(path, sizeof(path), "%s/%s.md", dir, b->name);
      if (stat(path, &st) == 0)
         continue;
      FILE *f = fopen(path, "w");
      if (!f)
         return -1;
      fprintf(f, "---\nname: %s\ndescription: %s\ndelegates: %s\nroles: [%s]\n", b->name,
              b->description, b->delegates, b->roles_csv);
      if (b->check_role[0])
         fprintf(f, "check_role: %s\ncheck_marker: %s\n", b->check_role, b->check_marker);
      fprintf(f, "---\n\n## Persona\n%s\n", prompt_persona_text(b->mode));
      fprintf(f, "\n## Principles\n%s\n", prompt_principles_text(b->mode));
      const char *brief = builtin_brief(b);
      if (brief && brief[0])
         fprintf(f, "\n## Brief\n%s\n", brief);
      fclose(f);
      written++;
   }
   return written;
}

/* --- write / delete (Aimee- and user-editable personas) ------------------ */

int persona_name_valid(const char *name)
{
   if (!name || !name[0])
      return 0;
   size_t n = strlen(name);
   if (n >= PERSONA_NAME_MAX)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)name[i];
      if (!(isalnum(c) || c == '.' || c == '_' || c == '-'))
         return 0;
   }
   /* Reject "." / ".." and any leading-dot dotfile to keep names file-safe. */
   if (name[0] == '.')
      return 0;
   return 1;
}

/* Serialize a persona_t to <path> in the single-file markdown format that
 * load_file() reads back (frontmatter + ## Persona / ## Principles / ## Brief).
 * Overwrites any existing file. */
static int persona_write_file(const char *path, const persona_t *p)
{
   FILE *f = fopen(path, "w");
   if (!f)
      return -1;
   fprintf(f, "---\nname: %s\n", p->name);
   if (p->description[0])
      fprintf(f, "description: %s\n", p->description);
   fprintf(f, "delegates: %s\n", p->delegates[0] ? p->delegates : PERSONA_DELEGATES_FULL);
   fprintf(f, "roles: [");
   for (int i = 0; i < p->roles_count; i++)
      fprintf(f, "%s%s", i ? ", " : "", p->roles[i]);
   fprintf(f, "]\n");
   if (p->check_role[0])
      fprintf(f, "check_role: %s\ncheck_marker: %s\n", p->check_role, p->check_marker);
   fprintf(f, "---\n");
   if (p->persona_text && p->persona_text[0])
      fprintf(f, "\n## Persona\n%s\n", p->persona_text);
   if (p->principles_text && p->principles_text[0])
      fprintf(f, "\n## Principles\n%s\n", p->principles_text);
   if (p->brief_text && p->brief_text[0])
      fprintf(f, "\n## Brief\n%s\n", p->brief_text);
   if (fclose(f) != 0)
      return -1;
   return 0;
}

int persona_write(const persona_t *p)
{
   if (!p || !persona_name_valid(p->name))
      return -1;
   char dir[PERSONA_PATH_MAX];
   snprintf(dir, sizeof(dir), "%s/personas", config_default_dir());
   struct stat st;
   if (stat(dir, &st) != 0)
      platform_mkdir_p(dir, 0755);
   char path[PERSONA_PATH_MAX];
   snprintf(path, sizeof(path), "%s/%s.md", dir, p->name);
   return persona_write_file(path, p);
}

int persona_delete(const char *name)
{
   if (!persona_name_valid(name))
      return -1;
   char path[PERSONA_PATH_MAX];
   snprintf(path, sizeof(path), "%s/personas/%s.md", config_default_dir(), name);
   struct stat st;
   if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      return -1;
   return unlink(path) == 0 ? 0 : -1;
}
