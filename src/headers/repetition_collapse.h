/* repetition_collapse.h: verbatim periodicity detector with structural-region
 * and JSON-fragment allowlist suppression.
 *
 * Detects degenerate repetition loops in a model-output byte stream
 * (the "repetition collapse" failure mode of small reasoning models) without
 * flagging legitimate repeating structures - fenced code blocks, list bodies,
 * markdown tables, and grammar-valid JSON fragments.
 *
 * Contract:
 *   - Pure function over an in-memory byte buffer. No I/O, no global state.
 *   - Deterministic: identical inputs produce byte-equal outputs across runs.
 *   - The structural parser and the periodicity check share the same byte
 *     stream; regions are emitted in a single pass and consumed in a second.
 *   - False-positive-averse: repeats whose byte range overlaps any marked
 *     structural region (fenced / list body / indented code block / table) OR
 *     a JSON grammar-matched fragment span are suppressed. Heterogeneous
 *     objects (input that is NOT valid JSON per the in-module grammar) are
 *     NEVER suppressed and are passed straight to the periodicity check.
 */
#ifndef DEC_REPETITION_COLLAPSE_H
#define DEC_REPETITION_COLLAPSE_H 1

#include <stddef.h>

/* Region kinds emitted by the structural parser. */
typedef enum
{
   RC_REGION_NONE = 0,
   RC_REGION_FENCE,        /* fenced code block body (```/~~~) */
   RC_REGION_LIST_BODY,    /* content past a markdown list marker */
   RC_REGION_CODE_BLOCK,   /* indented (>=4 spaces / tab) code block line */
   RC_REGION_TABLE,        /* line is inside a markdown pipe-table row */
   RC_REGION_COUNT
} rc_region_kind_t;

/* A region marker - half-open byte range [start, end) in the source buffer. */
typedef struct
{
   rc_region_kind_t kind;
   size_t           start;
   size_t           end;
} rc_region_t;

#define RC_MAX_REGIONS 256

typedef struct
{
   rc_region_t regions[RC_MAX_REGIONS];
   size_t      count;        /* number of regions actually written */
   size_t      truncated;    /* 0 normally; 1 if more regions existed than RC_MAX_REGIONS */
} rc_region_set_t;

/* A JSON-fragment match emitted by the in-module grammar. Heterogeneous
 * (non-grammar) input yields no matches - those ranges fall through to the
 * periodicity check unmodified. */
typedef struct
{
   size_t start;  /* first byte of the matched fragment (inclusive) */
   size_t end;    /* byte past the last matched byte (exclusive) */
} rc_json_span_t;

#define RC_MAX_JSON_SPANS 64

typedef struct
{
   rc_json_span_t spans[RC_MAX_JSON_SPANS];
   size_t         count;        /* number of spans actually written */
   size_t         truncated;    /* 0 normally; 1 if more spans existed than RC_MAX_JSON_SPANS */
} rc_json_span_set_t;

/* Detector result.
 *
 * loop_start_offset is the offset of the FIRST byte of the repeating span
 * (the loop-start boundary). loop_span_bytes is the length of one period of
 * the repeating span. When hit is 0, both fields are 0 and the buffer is
 * treated as clean. */
typedef struct
{
   int    hit;               /* 1 if a verbatim loop was found and not suppressed */
   size_t loop_start_offset; /* byte offset of the first byte of the repeating span */
   size_t loop_span_bytes;   /* length of one period of the repeating span */
   size_t repeats;           /* number of back-to-back repeats of that span */
} rc_result_t;

/* Default thresholds (rung 1 of the accepted proposal). */
#define RC_DEFAULT_MIN_SPAN_BYTES 60
#define RC_DEFAULT_MIN_REPEATS    4

size_t rc_parse_regions(const char *buf, size_t len, rc_region_set_t *out);
size_t rc_parse_json_spans(const char *buf, size_t len, rc_json_span_set_t *out);
int   rc_range_overlap(size_t a_start, size_t a_end, size_t b_start, size_t b_end);

void rc_detect(const char *buf, size_t len,
               size_t min_span_bytes, size_t min_repeats,
               rc_result_t *out);

void rc_metrics(int tp, int fp, int tn, int fn,
                double *out_precision,
                double *out_recall,
                double *out_specificity);

/* Precision floor enforced by CI. */
#define RC_REQUIRED_PRECISION 0.98

#endif /* DEC_REPETITION_COLLAPSE_H */
