/*
 * lsp.h: LSP (Language Server Protocol) subsystem public API.
 *
 * Manages LSP server lifecycles per workspace, exposes diagnostics,
 * definitions, and references. Servers are started lazily on first
 * file touch and shut down cleanly on session end.
 *
 * Layer 0 (Foundation) — no agent/command layer dependencies.
 */
#ifndef DEC_LSP_H
#define DEC_LSP_H 1

#include <stddef.h>

/* Maximum length of a single diagnostic message (no embedded newlines) */
#define LSP_DIAG_MSG_MAX 512

/* Maximum path length for LSP file URIs */
#define LSP_PATH_MAX 4096

/* Cap on diagnostics / symbol entries returned in prompt renders */
#define LSP_RENDER_MAX_DIAG 12
#define LSP_RENDER_MAX_SYM  12

/* LSP diagnostic severity levels (mirrors the LSP spec) */
typedef enum
{
   LSP_SEV_ERROR = 1,
   LSP_SEV_WARNING = 2,
   LSP_SEV_INFO = 3,
   LSP_SEV_HINT = 4,
} lsp_severity_t;

/* A single diagnostic entry */
typedef struct
{
   char file[LSP_PATH_MAX];
   int line; /* 0-based */
   int col;  /* 0-based */
   lsp_severity_t severity;
   char message[LSP_DIAG_MSG_MAX];
} lsp_diag_t;

/* A single symbol location (definition or reference) */
typedef struct
{
   char symbol[256];
   char file[LSP_PATH_MAX];
   int line; /* 0-based */
   int col;  /* 0-based */
} lsp_location_t;

/* -----------------------------------------------------------------------
 * lsp_manager: per-workspace LSP lifecycle
 * ----------------------------------------------------------------------- */

/*
 * lsp_manager_init() — initialize the LSP manager subsystem.
 * Safe to call multiple times; idempotent.
 */
void lsp_manager_init(void);

/*
 * lsp_manager_shutdown_all() — terminate all active LSP servers and
 * release resources. Called at session end or atexit.
 */
void lsp_manager_shutdown_all(void);

/*
 * lsp_manager_diagnostics() — retrieve stored diagnostics for |file|
 * (or all files in |workspace| when |file| is NULL).
 *
 * Writes up to |max| entries into |out|.
 * Returns the number of entries written.
 */
int lsp_manager_diagnostics(const char *workspace, const char *file, lsp_diag_t *out, int max);

/*
 * lsp_manager_definition() — resolve the definition of the symbol at
 * |file|:|line|:|col| (0-based) in |workspace|.
 *
 * Writes up to |max| location entries into |out|.
 * Returns the number of entries written, or -1 on error (check errbuf).
 */
int lsp_manager_definition(const char *workspace, const char *file, int line, int col,
                           lsp_location_t *out, int max, char *errbuf, size_t errbuf_size);

/*
 * lsp_manager_references() — find all references to the symbol at
 * |file|:|line|:|col| (0-based) in |workspace|.
 *
 * Writes up to |max| location entries into |out|.
 * Returns the number of entries written, or -1 on error (check errbuf).
 */
int lsp_manager_references(const char *workspace, const char *file, int line, int col,
                           lsp_location_t *out, int max, char *errbuf, size_t errbuf_size);

/*
 * lsp_manager_rename() — perform a workspace-wide symbol rename via LSP.
 * Sends textDocument/rename for the symbol at |file|:|line|:|col| (0-based)
 * with |new_name|, applies the resulting WorkspaceEdit to disk, and writes
 * a human-readable summary of changed files into |out|.
 *
 * Returns the number of files modified, or -1 on error (check errbuf).
 */
int lsp_manager_rename(const char *workspace, const char *file, int line, int col,
                       const char *new_name, char *out, size_t out_size, char *errbuf,
                       size_t errbuf_size);

/*
 * lsp_manager_diag_summary() — aggregate diagnostic counts and active server
 * count across all workspaces.
 *
 * Writes the counts of errors, warnings, and active servers into the
 * corresponding out-parameters (all may be NULL to ignore).
 */
void lsp_manager_diag_summary(int *errors, int *warnings, int *active_servers);

/* -----------------------------------------------------------------------
 * lsp_client: low-level JSON-RPC framing helpers (used by lsp_manager)
 * ----------------------------------------------------------------------- */

/*
 * lsp_frame_write() — write a JSON-RPC Content-Length framed message
 * to |fd|.  |json| must be a NUL-terminated JSON string.
 * Returns 0 on success, -1 on error.
 */
int lsp_frame_write(int fd, const char *json);

/*
 * lsp_frame_read() — read the next Content-Length framed message from |fd|
 * into |buf| (NUL-terminated).  Returns the number of bytes read, or -1
 * on EOF/error, or -2 if the buffer is too small.
 */
int lsp_frame_read(int fd, char *buf, size_t buf_size);

/*
 * lsp_parse_diagnostics() — parse a LSP textDocument/publishDiagnostics
 * notification JSON into |out|.  |json| is the full notification object.
 * Writes up to |max| entries.  Returns the number parsed.
 */
int lsp_parse_diagnostics(const char *json, const char *file_path, lsp_diag_t *out, int max);

/*
 * lsp_render_context() — format arrays of diagnostics, definitions, and
 * references into the standard prompt markdown block.
 * Returns the number of bytes written (excluding NUL terminator).
 */
int lsp_render_context(const char *focus_file, const lsp_diag_t *diags, int ndiags,
                       const lsp_location_t *defs, int ndefs, const lsp_location_t *refs, int nrefs,
                       char *out, size_t out_size);

/*
 * lsp_severity_label() — return the human-readable label for a severity.
 */
const char *lsp_severity_label(lsp_severity_t sev);

#endif /* DEC_LSP_H */
