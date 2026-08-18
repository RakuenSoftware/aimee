/* kb_http_json.h: scalar field scanners for KB HTTP request bodies.
 *
 * These are deliberately NOT a JSON parser. They locate "key": <scalar> by
 * substring scan and read the value in place, which is why they tolerate a
 * truncated or otherwise malformed body: a request that names the field it
 * needs still works. kb_http.c also uses cJSON for the cases that need real
 * structure (objects, arrays, type errors); these cover the flat scalars.
 *
 * The scan is safe against a key appearing inside a string VALUE, because a
 * JSON string cannot contain an unescaped double quote -- the needle carries
 * its own quotes, so "max_results" never matches \"max_results\".
 */
#ifndef KB_HTTP_JSON_H
#define KB_HTTP_JSON_H

#include <stddef.h>

struct cJSON;

void kb_http_capabilities_json(char *out, size_t out_cap, struct cJSON *surfaces);

/* Copy the string value of `key` into out (NUL-terminated, truncated to
 * out_cap). Returns 1 when the key was found and was a string, else 0 with
 * out set to "". Backslash escapes are unescaped one level. */
int kb_http_json_str(const char *body, const char *key, char *out, size_t out_cap);

/* Read the integer value of `key`, or default_val when absent or not a
 * number. Accepts a leading sign; an out-of-range literal saturates to
 * INT_MIN/INT_MAX rather than invoking atoi's undefined behaviour. Callers
 * are responsible for their own domain clamping -- this reports what was
 * sent, not what is acceptable. */
int kb_http_json_int(const char *body, const char *key, int default_val);

/* Read the boolean value of `key`, or default_val when absent or not a
 * JSON true/false literal. */
int kb_http_json_bool(const char *body, const char *key, int default_val);

#endif /* KB_HTTP_JSON_H */
