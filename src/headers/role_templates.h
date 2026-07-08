#ifndef DEC_ROLE_TEMPLATES_H
#define DEC_ROLE_TEMPLATES_H 1

#define ROLE_TEMPLATE_MAX_SIZE  4096 /* max bytes per template file */
#define ROLE_TEMPLATE_MAX_ROLES 32   /* max roles returned by list */
#define ROLE_TEMPLATE_NAME_MAX  32   /* max role name length */
#define ROLE_TEMPLATE_PATH_MAX  512  /* max path length */

/* Resolve the effective template path for a role.
 * Resolution order: project (.aimee/role_templates/<role>.md),
 * user (~/.config/aimee/role_templates/<role>.md),
 * bundled (role_templates/ in install prefix, not usually available at runtime).
 * Returns 0 if a template file was found (path written to buf), -1 if not found. */
int role_template_path(const char *project_root, const char *role, char *buf, size_t bufsz);

/* Build a system prompt from the role template, substituting {{TASK}} and {{CONTEXT}}.
 * project_root may be NULL (skips project-level lookup).
 * task and context may be NULL (omitted from substitution).
 * Returns a malloc'd string on success (caller must free), or NULL on error/not found. */
char *role_template_build(const char *project_root, const char *role, const char *task,
                          const char *context);

/* List available role names (combining project and user templates, deduped).
 * Writes up to max_names names into names_out (each a static const string).
 * Returns count written. */
int role_template_list(const char *project_root, char names_out[][ROLE_TEMPLATE_NAME_MAX],
                       int max_names);

/* Write default templates for all built-in roles into dir (dir must exist).
 * Skips files that already exist. Returns number of files written, -1 on error. */
int role_template_install_defaults(const char *dir);

/* 1 if `role` is a filename-safe single segment (letters, digits, '.', '_',
 * '-'; non-empty; no path separators or leading dot). Guards write/delete. */
int role_template_name_valid(const char *role);

/* Per-role turn cap from the role template's `max_turns:` frontmatter, or -1
 * (INFINITE — the default for every role) when unset. Edited under the Personas
 * tab. */
int role_template_max_turns(const char *role);

/* Return the RAW template body for `role` (placeholders intact, not built):
 * project file -> user file -> built-in default. Heap (caller frees), or NULL
 * if no such role. */
char *role_template_read_raw(const char *project_root, const char *role);

/* Write `content` as the user-level template <config>/role_templates/<role>.md,
 * creating the dir as needed and overwriting any existing file. Returns 0 on
 * success, -1 on error (bad name, oversized, or I/O failure). */
int role_template_write(const char *role, const char *content);

/* Remove the user-level template file for `role`. For a built-in this resets it
 * to the code default. Returns 0 if removed, -1 if none existed or on error. */
int role_template_delete(const char *role);

#endif /* DEC_ROLE_TEMPLATES_H */
