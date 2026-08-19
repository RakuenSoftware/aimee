/* cli_argspec.c: build a request body from a served argument spec.
 *
 * See headers/cli_argspec.h for the spec shape and what it deliberately cannot
 * express. The rule throughout this file is that an unrecognised anything —
 * source, type, field shape — refuses the WHOLE spec. A partially honoured
 * spec produces a request body that is wrong rather than incomplete, and the
 * server would answer it as though the operator had asked for that.
 */
#include "cli_argspec.h"

#include "cli_v1_routes_internal.h"
#include "cli_client.h" /* cli_v1_manifest_argspec */

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Field value sources. */
#define SRC_FLAG            "flag"
#define SRC_POSITIONAL      "positional"
#define SRC_POSITIONAL_FLAG "positional_or_flag"
#define SRC_ARGV_JOINED     "argv_joined"
/* Every argv word as a JSON array of strings. The model and agent command
 * families pass their arguments through verbatim this way -- one shape,
 * ten-odd methods, and no interpretation of what the words mean. */
#define SRC_ARGV_ARRAY "argv_array"

/* Whether a field present-but-empty is sent or dropped. Absent means "drop".
 *
 * Both conventions are real, and "drop" is the RARE one: 81 positional sites
 * gate on pos_count alone -- `aimee vault set "" x y` sends "agent": "" -- and
 * 2 also require a non-empty value. The spec's default followed the two.
 *
 * I first refused to add this, on the grounds that describing the common
 * convention would enshrine something that looks like a bug. That was wrong, and
 * the count is why: a vocabulary that can express 2 of 83 sites is not taking a
 * principled stand, it is incomplete. Whether those 81 sites SHOULD send an
 * empty field is a separate question, and changing them is not this file's
 * business -- describing them faithfully is.
 */
#define EMPTY_DROP "drop"
#define EMPTY_EMIT "emit"

/* How a value becomes JSON. Absent means "string". */
#define TYPE_STRING      "string"
#define TYPE_NUMBER      "number"
#define TYPE_NUMBER_LAX  "number_lenient"
#define TYPE_BOOL        "bool"
#define TYPE_TRUE_IF_SET "true_if_set"
/* A bool that is the NEGATION of a flag's presence: `compress` is true unless
 * --no-compress was given. Always emitted, like TYPE_BOOL.
 *
 * I refused this once, on the grounds that the vocabulary "deliberately has no
 * negation". That was a weaker line than the one actually being held, which is
 * that a field's rule may depend on ITS OWN value and nothing else -- the same
 * standard that admitted empty:"drop" and omit_if_nonpositive. Inversion meets
 * it: no other field is consulted, and no branch decides which fields exist. */
#define TYPE_BOOL_INVERTED "bool_inverted"
/* A constant STRING emitted when a flag is present: --review sends
 * "status": "ambiguous". Exactly true_if_set with a literal other than true,
 * so the same reasoning admits it -- one field, its own flag, no branch. The
 * literal travels in `value`. */
#define TYPE_CONST_IF_SET "const_if_set"

/* Two numeric conventions, and "number" is the RARE one: 53 sites parse with
 * atoi()/cli_args_get_int(), so "12x" becomes 12 and "abc" becomes 0, while 3
 * refuse trailing garbage outright (kb.grant's team_id, which selects an
 * authorization scope and must not be rounded into a different team).
 *
 * Same finding as `empty`, same conclusion: describing only the strict 3 left
 * the spec unable to say what almost every marshaller does. Whether those 53
 * SHOULD refuse instead of coercing is a real question and an open one -- and
 * kb.grant's comment is the argument that they should -- but that is a change
 * to the CLI, not to a file whose job is to describe it. */

static const char *field_str(const cJSON *field, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(field, key);
   return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static int known_source(const char *from)
{
   return from && (!strcmp(from, SRC_FLAG) || !strcmp(from, SRC_POSITIONAL) ||
                   !strcmp(from, SRC_POSITIONAL_FLAG) || !strcmp(from, SRC_ARGV_JOINED) ||
                   !strcmp(from, SRC_ARGV_ARRAY));
}

static int known_empty(const char *e)
{
   return !e || !strcmp(e, EMPTY_DROP) || !strcmp(e, EMPTY_EMIT);
}

static int known_type(const char *type)
{
   /* Absent is legal and means string: the commonest field should not have to
    * say so in every row. */
   return !type || !strcmp(type, TYPE_STRING) || !strcmp(type, TYPE_NUMBER) ||
          !strcmp(type, TYPE_NUMBER_LAX) || !strcmp(type, TYPE_BOOL) ||
          !strcmp(type, TYPE_BOOL_INVERTED) || !strcmp(type, TYPE_CONST_IF_SET) ||
          !strcmp(type, TYPE_TRUE_IF_SET);
}

int cli_argspec_supported(const cJSON *spec)
{
   if (!cJSON_IsObject(spec))
      return 0;
   const cJSON *fields = cJSON_GetObjectItemCaseSensitive(spec, "fields");
   /* No fields is a legal spec — it is the no-argument case written the long
    * way — but a `fields` that is present and not an array is a spec this
    * build does not understand. */
   if (fields && !cJSON_IsArray(fields))
      return 0;
   const cJSON *bools = cJSON_GetObjectItemCaseSensitive(spec, "bool_flags");
   if (bools && !cJSON_IsArray(bools))
      return 0;
   for (const cJSON *b = bools ? bools->child : NULL; b; b = b->next)
      if (!cJSON_IsString(b) || !b->valuestring)
         return 0;

   for (const cJSON *f = fields ? fields->child : NULL; f; f = f->next)
   {
      if (!cJSON_IsObject(f))
         return 0;
      const char *json_name = field_str(f, "json");
      const char *from = field_str(f, "from");
      if (!json_name || !json_name[0] || !known_source(from))
         return 0;
      if (!known_type(field_str(f, "type")))
         return 0;
      if (!known_empty(field_str(f, "empty")))
         return 0;
      {
         /* A constant field with no constant says nothing. */
         const char *ty = field_str(f, "type");
         if (ty && !strcmp(ty, TYPE_CONST_IF_SET) && !field_str(f, "value"))
            return 0;
      }
      /* A source must carry what it reads from, or the row means nothing. */
      if ((!strcmp(from, SRC_FLAG) || !strcmp(from, SRC_POSITIONAL_FLAG)) && !field_str(f, "flag"))
         return 0;
      if (!strcmp(from, SRC_POSITIONAL) || !strcmp(from, SRC_POSITIONAL_FLAG))
      {
         const cJSON *idx = cJSON_GetObjectItemCaseSensitive(f, "index");
         if (!cJSON_IsNumber(idx) || idx->valuedouble < 0 || idx->valuedouble >= V1_MAX_POS)
            return 0;
      }
   }
   return 1;
}

/* Collect the spec's bool flags into the NULL-terminated array cli_args_parse
 * expects. Returns NULL when there are none (which cli_args_parse accepts), or
 * on allocation failure — indistinguishable here, so the caller checks the
 * count itself before treating NULL as an error. */
static const char **collect_bool_flags(const cJSON *spec, int *count_out)
{
   *count_out = 0;
   const cJSON *bools = cJSON_GetObjectItemCaseSensitive(spec, "bool_flags");
   int n = bools ? cJSON_GetArraySize(bools) : 0;
   if (n <= 0)
      return NULL;
   const char **out = calloc((size_t)n + 1u, sizeof(*out));
   if (!out)
      return NULL;
   int i = 0;
   for (const cJSON *b = bools->child; b && i < n; b = b->next)
      out[i++] = b->valuestring;
   out[i] = NULL;
   *count_out = i;
   return out;
}

/* Join every argv word with single spaces. The one shape that reads the command
 * line as prose rather than as fields (`aimee help how do delegates work`). */
static char *join_argv(int argc, char **argv)
{
   size_t total = 1;
   for (int i = 0; i < argc; i++)
      total += strlen(argv[i]) + 1;
   char *out = malloc(total);
   if (!out)
      return NULL;
   out[0] = '\0';
   for (int i = 0; i < argc; i++)
   {
      if (i > 0)
         strcat(out, " ");
      strcat(out, argv[i]);
   }
   return out;
}

/* The value a field reads, or NULL when it is absent. `joined` is the lazily
 * built argv join, owned by the caller. */
static const char *field_value(const cJSON *field, const cli_args_t *opts, const char *joined)
{
   const char *from = field_str(field, "from");
   const char *flag = field_str(field, "flag");

   if (!strcmp(from, SRC_ARGV_JOINED))
      return joined && joined[0] ? joined : NULL;

   if (!strcmp(from, SRC_FLAG))
      return cli_args_get(opts, flag);

   const cJSON *idx = cJSON_GetObjectItemCaseSensitive(field, "index");
   int i = (int)idx->valuedouble;
   const char *empty = field_str(field, "empty");
   int emit_empty = empty && !strcmp(empty, EMPTY_EMIT);
   /* With "emit", presence is the COUNT alone -- an empty argument is a value
      the operator typed, and the marshallers that do this send it. */
   const char *pos = NULL;
   if (opts->pos_count > i && opts->positional[i] && (emit_empty || opts->positional[i][0]))
      pos = opts->positional[i];
   if (pos)
      return pos;
   /* positional_or_flag falls back to the named flag, which is how the compiled
    * marshallers let `aimee trigger status <id>` and `--id <id>` mean the same
    * thing. A plain positional does not fall back. */
   return !strcmp(from, SRC_POSITIONAL_FLAG) ? cli_args_get(opts, flag) : NULL;
}

/* Add one field to `req`. Returns 0 on success, -1 when a required field is
 * absent (the caller reports usage and abandons the request). */
static int add_field(cJSON *req, const cJSON *field, const cli_args_t *opts, const char *joined,
                     int argc, char **argv)
{
   const char *json_name = field_str(field, "json");
   const char *type = field_str(field, "type");
   int required = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(field, "required"));

   const char *from = field_str(field, "from");
   if (from && !strcmp(from, SRC_ARGV_ARRAY))
   {
      cJSON *arr = cJSON_CreateArray();
      if (!arr)
         return -1;
      for (int i = 0; i < argc; i++)
         cJSON_AddItemToArray(arr, cJSON_CreateString(argv[i]));
      cJSON_AddItemToObject(req, json_name, arr);
      return 0;
   }

   if (type && !strcmp(type, TYPE_CONST_IF_SET))
   {
      const char *lit = field_str(field, "value");
      if (!lit)
         return -1;
      if (cli_args_get(opts, field_str(field, "flag")))
         cJSON_AddStringToObject(req, json_name, lit);
      return 0;
   }

   if (type && !strcmp(type, TYPE_BOOL_INVERTED))
   {
      cJSON_AddBoolToObject(req, json_name, cli_args_get(opts, field_str(field, "flag")) ? 0 : 1);
      return 0;
   }

   /* true_if_set is a presence test, not a value: `--json` with no argument is
    * the whole of it, so ask the parser whether the flag was there at all. */
   if (type && !strcmp(type, TYPE_TRUE_IF_SET))
   {
      if (cli_args_get(opts, field_str(field, "flag")))
         cJSON_AddTrueToObject(req, json_name);
      return 0;
   }

   const char *empty = field_str(field, "empty");
   int emit_empty = empty && !strcmp(empty, EMPTY_EMIT);
   /* Omit a number whose value is not positive. Exactly parallel to
    * `empty: "drop"` for strings -- a rule about ONE field's own value, with no
    * reference to any other field, which is the line between data and a
    * program. Seven marshallers do `int n = ...; if (n > 0) Add(n);`. */
   int omit_nonpositive =
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(field, "omit_if_nonpositive"));
   const char *value = field_value(field, opts, joined);
   /* `default` reproduces cli_args_get_int(opts, name, def): the field is
    * emitted even when the flag is absent, carrying the default. Absent means
    * "omit when absent", which is what every other field does. */
   const cJSON *dflt = cJSON_GetObjectItemCaseSensitive(field, "default");
   if ((!value || !value[0]) && cJSON_IsNumber(dflt) && !emit_empty)
   {
      if (!(omit_nonpositive && dflt->valuedouble <= 0))
         cJSON_AddNumberToObject(req, json_name, dflt->valuedouble);
      return 0;
   }
   if (!value || (!value[0] && !emit_empty))
      return required ? -1 : 0;

   if (!type || !strcmp(type, TYPE_STRING))
      cJSON_AddStringToObject(req, json_name, value);
   else if (!strcmp(type, TYPE_NUMBER_LAX))
   /* atoi(): leading digits, 0 for anything else, no refusal. Exactly what
    * the 53 sites do -- described, not endorsed. */
   {
      int n = atoi(value);
      if (omit_nonpositive && n <= 0)
         return 0;
      cJSON_AddNumberToObject(req, json_name, (double)n);
   }
   else if (!strcmp(type, TYPE_NUMBER))
   {
      /* Strict: a field the spec called a number is refused rather than
       * coerced, so "12x" cannot quietly become 12 and address something the
       * operator did not type. */
      char *tail = NULL;
      double d = strtod(value, &tail);
      if (!tail || *tail || tail == value)
         return -1;
      cJSON_AddNumberToObject(req, json_name, d);
   }
   else /* TYPE_BOOL: the flag's presence, emitted as an explicit true. */
      cJSON_AddBoolToObject(req, json_name, cli_args_has_flag(opts, field_str(field, "flag")));
   return 0;
}

cJSON *cli_argspec_build(const char *method, const cJSON *spec, int argc, char **argv)
{
   if (!method || !method[0] || !cli_argspec_supported(spec))
      return NULL;

   int bool_count = 0;
   const char **bool_flags = collect_bool_flags(spec, &bool_count);
   const cJSON *bools = cJSON_GetObjectItemCaseSensitive(spec, "bool_flags");
   if (bools && cJSON_GetArraySize(bools) > 0 && !bool_flags)
      return NULL; /* allocation failed; do not parse with the wrong flag set */

   cli_args_t opts;
   cli_args_parse(argc, argv, bool_flags, &opts);

   char *joined = NULL;
   const cJSON *fields = cJSON_GetObjectItemCaseSensitive(spec, "fields");
   for (const cJSON *f = fields ? fields->child : NULL; f && !joined; f = f->next)
   {
      const char *from = field_str(f, "from");
      if (from && !strcmp(from, SRC_ARGV_JOINED))
         joined = join_argv(argc, argv);
   }

   cJSON *req = cJSON_CreateObject();
   if (!req)
   {
      free((void *)bool_flags);
      free(joined);
      return NULL;
   }
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);

   int missing = 0;
   for (const cJSON *f = fields ? fields->child : NULL; f && !missing; f = f->next)
      if (add_field(req, f, &opts, joined, argc, argv) != 0)
         missing = 1;

   free((void *)bool_flags);
   free(joined);

   if (missing)
   {
      /* Say what was expected. A marshal failure is a silent exit in the shared
       * forwarder, so a served command must refuse as loudly as a compiled one
       * or the operator is left unable to tell a typo from an outage. */
      const char *usage = field_str(spec, "usage");
      fprintf(stderr, "aimee: %s\n", usage && usage[0] ? usage : "missing a required argument");
      marshal_request_note_reported();
      cJSON_Delete(req);
      return NULL;
   }
   return req;
}

int cli_argspec_try_served(const char *method, int argc, char **argv, cJSON **out)
{
   /* A new command that takes arguments was unusable even once its route, its
    * catalogue entry and its dispatch row were served: the client had no way to
    * learn that `--capability` becomes "capability".
    *
    * A served spec is consulted BEFORE the compiled marshallers, so it wins.
    * That is the point, and it is safe because it cannot silently differ: every
    * shipped spec is proven byte-identical to its marshaller by
    * test_cli_argspec, which includes the same data file the server serves
    * from. A spec this build cannot interpret is refused whole and falls
    * through to the compiled path rather than sending a body it guessed. */
   *out = NULL;
   const cJSON *spec = cli_v1_manifest_argspec(method);
   if (!spec)
      return 0;
   *out = cli_argspec_build(method, spec, argc, argv);
   if (*out)
      return 1;
   /* NULL is either "a required argument is missing" — already reported, and
    * the operator must see that rather than a second opinion from a marshaller
    * — or an uninterpretable spec, which falls through. */
   return marshal_request_peek_reported() ? 1 : 0;
}
