/* persona.h: server-owned persona registry.
 *
 * A "persona" is the primary agent's identity: a system-prompt persona text,
 * a principles block, session-brief hints, advertised delegate roles, and a
 * done-gate (check) delegate role + verdict marker. The built-ins
 * (engineer/novel/songwriter plus the read-only reviewers qa/security/reviewer/
 * architect) keep their PROSE in prompts.c (reached via the aimee_mode_t enum
 * path so engineer stays byte-identical); this registry adds
 * their metadata and loads arbitrary user/project personas from single-file
 * markdown definitions:
 *
 *   <project>/.aimee/personas/<name>.md   (project-level)
 *   <config_default_dir>/personas/<name>.md   (user-level)
 *
 * with YAML frontmatter (name/description/roles/check_role/check_marker) and
 * `## Persona` / `## Principles` / `## Brief` sections.
 *
 * Server-side only: clients reach personas over the aimee-server /v1 HTTP API,
 * never by reading these files directly. */
#ifndef DEC_PERSONA_H
#define DEC_PERSONA_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PERSONA_NAME_MAX      64
#define PERSONA_DESC_MAX      256
#define PERSONA_PATH_MAX      512
#define PERSONA_MAX_NAMES     64
#define PERSONA_MAX_ROLES     16
#define PERSONA_FILE_MAX_SIZE 16384

   typedef struct
   {
      char name[PERSONA_NAME_MAX];
      char description[PERSONA_DESC_MAX];
      /* Prose fields are heap-owned (caller frees via persona_free) or NULL.
       * For built-ins they are NULL — callers use the enum path (prompts.c)
       * for built-in prose. For custom personas they come from the file. */
      char *persona_text;    /* "## Persona" body; may contain one %s (cwd) */
      char *principles_text; /* "## Principles" block */
      char *brief_text;      /* "## Brief" session hints */
      char roles[PERSONA_MAX_ROLES][PERSONA_NAME_MAX]; /* advertised delegate roles */
      int roles_count;
      char check_role[PERSONA_NAME_MAX];   /* done-gate delegate role */
      char check_marker[PERSONA_NAME_MAX]; /* verdict marker stem, e.g. "CONTINUITY" */
      /* Delegate policy: "full" (use all delegates, must delegate), "readonly"
       * (read-only delegates only), or "none" (no delegates). Defaults to
       * "full" when a persona file omits it. Config-controlled via the
       * `delegates` frontmatter key. */
      char delegates[16];
      int builtin; /* 1 if a built-in persona */
   } persona_t;

#define PERSONA_DELEGATES_FULL     "full"
#define PERSONA_DELEGATES_READONLY "readonly"
#define PERSONA_DELEGATES_NONE     "none"

   /* 1 if the persona may spawn any delegate at all (policy != none). */
   int persona_delegates_enabled(const persona_t *p);
   /* 1 if the persona may spawn a WRITE delegate (policy == full). */
   int persona_delegates_writes(const persona_t *p);

   /* Render the policy-driven "# Delegation (IMPORTANT)" block for the persona
    * into buf (n bytes): a strong must-delegate directive for full, a
    * read-only-only directive for readonly, or a no-delegates directive for
    * none. Lists the persona's roles. */
   void persona_delegation_block(const persona_t *p, char *buf, size_t n);

   /* Compose a delegate (or session) system prompt for the persona resolved
    * from `name`: the persona's principles lead, then — for a non-engineer
    * built-in or a custom persona — its identity prose (the "You are ..." body,
    * with the first %s replaced by `cwd`), then `base_prompt` (the role-template
    * or task prompt; may be NULL). The engineer built-in contributes only its
    * principles, so an engineer-persona delegate keeps the prior framing. An
    * unknown `name` falls back to the engineer built-in. Returns a heap string
    * (caller frees); "" on OOM, never NULL. */
   char *persona_compose_delegate_prompt(const char *name, const char *cwd,
                                         const char *base_prompt);

   /* Render a persona's own prose (the "You are ..." body, role, workflow, and
    * any work queue), with the first %s replaced by `cwd`, for injection at the
    * start of a primary session — uniformly for every persona. Heap-owned
    * (caller frees) or NULL if the persona has none. Built-ins use their
    * canonical code prose (prompts.c); custom personas use their file
    * `## Persona` body. Never used for delegate prompts (see
    * persona_compose_delegate_prompt). */
   char *persona_identity_prose(const persona_t *p, const char *cwd);

   /* Resolve persona `name`. Lookup: project file -> user file -> built-in.
    * Unknown name with no file falls back to the engineer built-in and still
    * returns 0 (callers always get a usable persona). Fills *out; heap fields
    * are owned by the caller (persona_free). Returns 0 on success, -1 on a hard
    * error (NULL args). */
   int persona_load(const char *project_root, const char *name, persona_t *out);

   /* Free heap fields of *p (does not free p itself). Safe on a zeroed struct. */
   void persona_free(persona_t *p);

   /* Resolve the on-disk path for a persona file (project then user). Returns 0
    * if found (path in buf), -1 otherwise. */
   int persona_path(const char *project_root, const char *name, char *buf, size_t bufsz);

   /* List available persona names (project + user files + built-ins, deduped).
    * Returns the count written to names_out. */
   int persona_list(const char *project_root, char names_out[][PERSONA_NAME_MAX], int max_names);

   /* Dump the built-in personas as <dir>/<name>.md (skip existing). Returns the
    * number written, or -1 on error. */
   int persona_install_defaults(const char *dir);

   /* 1 if `name` is a safe single-segment persona name (filename-safe: letters,
    * digits, '.', '_', '-'; non-empty; no path separators or ".."). Guards the
    * write/delete paths that accept names from HTTP/MCP clients. */
   int persona_name_valid(const char *name);

   /* Serialize persona `p` to its user-level config file
    * (<config_default_dir>/personas/<name>.md), creating the directory as needed
    * and overwriting any existing file. The filename comes from p->name (which
    * must satisfy persona_name_valid). This is how Aimee/users add or edit a
    * persona: the file becomes the source of truth on the next load. Returns 0 on
    * success, -1 on error (bad name, OOM, or I/O failure). */
   int persona_write(const persona_t *p);

   /* Remove the user-level persona file for `name`
    * (<config_default_dir>/personas/<name>.md). For a built-in this is a "reset"
    * (the built-in default and code fallback remain). Returns 0 if a file was
    * removed, -1 if none existed or on error. */
   int persona_delete(const char *name);

   /* 1 if name is a built-in persona (engineer/novel/songwriter/qa/security/
    * reviewer/architect). */
   int persona_is_builtin(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* DEC_PERSONA_H */
