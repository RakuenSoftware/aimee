/* kb_ocr_sidecar.h: optional OCR sidecar client (structured-PDF Phase D). For PDFs with no/low
 * extractable text layer (scanned / image-only), an operator-deployed local OCR service turns a
 * rendered page image into text + per-line geometry, which feeds the SAME §1/§2 ingest path as
 * native text (so citations work identically). Same call-out pattern + trust posture as the TSR
 * sidecar: in-perimeter, no egress, no content retention; absent → the doc degrades to
 * asset-only (crops, no text). */
#ifndef AIMEE_KB_OCR_SIDECAR_H
#define AIMEE_KB_OCR_SIDECAR_H

#include "config.h"

typedef struct
{
   char text[1024];
   double x0, y0, x1, y1; /* normalized [0,1], top-left origin (same frame as kb_doc_regions) */
} kb_ocr_line_t;

/* Resolved OCR endpoint URL: cfg->ocr_command, else $AIMEE_OCR_URL, else "" (disabled). */
const char *kb_ocr_endpoint(const config_t *cfg);

/* OCR one rendered page image (PNG bytes). On success the out-params receive a heap array
 * (free with kb_ocr_free_lines). Returns: 1 = text recognised, 0 = no text, -1 =
 * unavailable/error (sidecar absent, unreachable, oversized, or bad response). */
int kb_ocr_recognize(const char *endpoint, int page_no, const unsigned char *png, int png_len,
                     kb_ocr_line_t **lines_out, int *n_out);

void kb_ocr_free_lines(kb_ocr_line_t *lines, int n);

#endif /* AIMEE_KB_OCR_SIDECAR_H */
