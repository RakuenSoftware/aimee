/* code_match.h: locate a code-search match within file content
 * (ingress-compression P1b span enrichment).
 *
 * The code search FTS-matches the whole file; ts_headline / sqlite snippet()
 * return a fragment with the matched token wrapped in ">>>" / "<<<" markers but
 * no line offset. code_match_line() recovers the 1-based line of that match so a
 * hit can carry a span the envelope can fold into a `file:line` reference. Pure
 * (string-only), so it is unit-tested without a DB. */
#ifndef DEC_CODE_MATCH_H
#define DEC_CODE_MATCH_H 1

/* The 1-based line in `content` of the first ">>>…<<<"-marked token in
 * `marked_snippet`, or 0 when it cannot be located (no markers, empty token, or
 * the token does not occur in content — ts_headline preserves original text, so
 * a verbatim match is expected; 0 means "line unknown, fall back to no span"). */
int code_match_line(const char *content, const char *marked_snippet);

#endif /* DEC_CODE_MATCH_H */
