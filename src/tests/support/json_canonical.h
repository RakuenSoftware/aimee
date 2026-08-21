/* json_canonical.h: one spelling for a JSON column, whichever engine returned it.
 *
 * DB2's JSON columns (artifacts.payload, feature_rows.features, ...) are JSONB on
 * Postgres, and Postgres normalises jsonb on the way back out: it prints a space
 * after every colon and reorders object keys. Under the sqlite test shim the same
 * column is plain TEXT holding the exact bytes cJSON emitted, so a needle like
 * "\"score\":1" matched there and matches nothing at all against the real engine.
 * Those assertions were testing the serializer's formatting, not the row's content.
 *
 * json_canonical() re-serialises the value through cJSON, which emits one canonical
 * compact spelling, so a substring assertion means the same thing on both backends.
 * ARRAY order is preserved by jsonb and therefore by this; OBJECT key order is not,
 * so a needle spanning two keys of the same object is still engine-dependent -- for
 * that, parse and assert on values.
 *
 * Header-only, and it uses a static buffer: an assertion's call completes before the
 * next one starts, and the test binaries are single-threaded.
 */
#ifndef AIMEE_TEST_JSON_CANONICAL_H
#define AIMEE_TEST_JSON_CANONICAL_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"

#ifndef JSON_CANONICAL_MAX
#define JSON_CANONICAL_MAX 16384
#endif

static inline const char *json_canonical(const char *json)
{
   static char buf[JSON_CANONICAL_MAX];
   cJSON *doc = cJSON_Parse(json ? json : "");
   assert(doc && "json_canonical: value did not parse as JSON");
   char *text = cJSON_PrintUnformatted(doc);
   assert(text);
   snprintf(buf, sizeof(buf), "%s", text);
   free(text);
   cJSON_Delete(doc);
   return buf;
}

#endif /* AIMEE_TEST_JSON_CANONICAL_H */
