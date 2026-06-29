/* kb_tsr_sidecar.h: optional table-structure-recognition (TSR) sidecar client
 * (structured-PDF Phase B). Same call-out pattern as the embedder: an operator points
 * the KB at a local TSR service (tsr_command config / AIMEE_TSR_URL); when absent, ingest
 * degrades to text-only and lookup_table reports tsr_status='unavailable'. The sidecar
 * runs INSIDE the KB trust perimeter with no egress; it never retains document content. */
#ifndef AIMEE_KB_TSR_SIDECAR_H
#define AIMEE_KB_TSR_SIDECAR_H

#include "config.h"

typedef struct
{
   int row;
   int col;
   char text[2048];
   int confidence;     /* TSR model confidence 0-100 */
   int line_index;     /* source line within the page (-1 if the sidecar did not supply one) */
   char subject[256];  /* optional entity-linkable triple (empty when the sidecar omits it) */
   char relation[128];
   char object[512];
} kb_tsr_cell_t;

/* Resolved TSR endpoint URL: cfg->tsr_command, else $AIMEE_TSR_URL, else "" (disabled).
 * Mirrors config_embedding_command's resolution order. */
const char *kb_tsr_endpoint(const config_t *cfg);

/* Recognise table structure for one page's regions.
 *   page_json : a JSON array of regions, e.g.
 *               [{"text":"..","x0":..,"y0":..,"x1":..,"y1":..,"line_index":..}, ...]
 * On a recognised table, *cells_out / *n_out receive a heap array (free with
 * kb_tsr_free_cells). Returns: 1 = table recognised (cells present), 0 = no table
 * (not_a_table), -1 = unavailable/error (sidecar absent, unreachable, or bad response). */
int kb_tsr_recognize(const char *endpoint, int page_no, const char *page_json,
                     kb_tsr_cell_t **cells_out, int *n_out);

void kb_tsr_free_cells(kb_tsr_cell_t *cells, int n);

#endif /* AIMEE_KB_TSR_SIDECAR_H */
