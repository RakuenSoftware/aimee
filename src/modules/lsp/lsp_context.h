#ifndef AIMEE_LSP_CONTEXT_H
#define AIMEE_LSP_CONTEXT_H

#include "cJSON.h"
#include "lsp.h"
#include <stddef.h>

typedef struct
{
   const char *provider;
   const char *root;
   const char *project;
   const char *worktree;
   void *ctx;
   int (*authorize)(void *ctx, const char *relative, char *resolved, size_t resolved_cap);
   int (*read_file)(void *ctx, const char *path, char **out, size_t *out_len);
   int (*sync)(void *ctx, const char *workspace, const char *file, const char *text,
               int *version_out, unsigned long *generation_out, char *errbuf, size_t errbuf_size);
   int (*query)(void *ctx, const char *operation, const char *workspace, const char *file, int line,
                int column, lsp_location_t *out, int max, char *errbuf, size_t errbuf_size);
   cJSON *(*source)(void *ctx, const char *relative, int line_start, int line_end, int max_lines);
} lsp_context_provider_t;

/* Build the typed, batched S1 semantic-context envelope. All filesystem and
 * provider authority enters through the closed callback table above so the
 * complete response contract can be exercised without the full server. */
cJSON *lsp_context_execute(const lsp_context_provider_t *provider, const cJSON *args);

#endif
