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
#include <unistd.h> /* getcwd */

/* Field value sources. */
#define SRC_FLAG            "flag"
#define SRC_POSITIONAL      "positional"
#define SRC_POSITIONAL_FLAG "positional_or_flag"
/* The SAME two sources, opposite precedence: the flag wins and a positional is
 * the fallback. kb.build reads `--path` first and positional[0] only if the
 * flag is absent. Both orders are in use and they differ whenever both are
 * supplied, so a spec has to say which one it means. */
#define SRC_FLAG_POSITIONAL "flag_or_positional"
#define SRC_ARGV_JOINED     "argv_joined"
/* Every argv word as a JSON array of strings. The model and agent command
 * families pass their arguments through verbatim this way -- one shape,
 * ten-odd methods, and no interpretation of what the words mean. */
#define SRC_ARGV_ARRAY "argv_array"
/* One RAW argv word, before flag parsing. `aimee memory delete --x` therefore
 * sends id from "--x", because the marshaller reads argv[0] and not a parsed
 * positional.
 *
 * I refused to describe this once, arguing the counting argument that admitted
 * empty:"drop" and number_lenient "does not apply to a sample of one". Rarity
 * was the wrong reason. The principle actually being held is DESCRIBE, do not
 * reform: whether these three methods should read a parsed positional instead
 * is a real question and a change to the CLI, not to a file whose job is to say
 * what the CLI does. Refusing to describe it did not fix it -- it only left the
 * client compiling in what the server could have told it. */
#define SRC_ARGV_INDEX "argv_index"
/* EVERY occurrence of one flag, as an array of its values. `aimee cron add ...
 * --skill a --skill b` sends "skills": ["a","b"], and the field is omitted
 * entirely when the flag never appears. Still a rule about one field and its
 * own flag: no other field is consulted, and no branch decides which fields
 * exist. */
#define SRC_REPEATED_FLAG "repeated_flag"
/* Two facts only the CLIENT can know, named by the SERVER.
 *
 * I refused these for most of this work, on the grounds that "a thin client
 * reading its own disk to build a request is doing its own job, not obeying the
 * server". That conflated two different things. The client DECIDING that a
 * field needs the working directory is client-side knowledge, and it is exactly
 * what forces a rebuild when a new command wants it. The client SUPPLYING the
 * working directory because a served spec asked for it is the thin-client model
 * working as intended: the server decides, the client provides the one value it
 * alone holds.
 *
 * Under the old reading, a new cwd-taking command could not be added without
 * shipping a new client -- which is the precise failure this whole exercise
 * exists to remove. 26 methods sat behind that mistake.
 *
 * They are NAMED, FIXED facts, not a general escape. There is deliberately no
 * `{"from": "env", "name": <anything>}`: that would let a server -- or anyone
 * who could answer as one -- ask the client to read AWS_SECRET_ACCESS_KEY and
 * post it back. A spec can ask for the working directory and the session id
 * because those are the two the CLI already puts on the wire, and for nothing
 * else. Adding a third is a deliberate act, not a configuration. */
/* An ordered cascade for ONE field: the first source that yields a value wins,
 * and `default` supplies a literal when none does. memory.recall's task_hint is
 * --task, then positional[0], then --query, then the literal "session start".
 *
 * positional_or_flag and flag_or_positional are the two-source special cases of
 * this; they stay because they are what most marshallers do and reading them is
 * easier than reading a list. Still one field consulting its own flags and its
 * own positional: no other field is involved, and no branch decides which
 * fields exist. */
/* Every PARSED positional as an array of strings. memory.search sends its
 * keywords this way. argv_array is the raw-argv sibling; this one respects
 * flag parsing, which is what the marshallers building it do. */
/* A value fixed by the METHOD, not by the input. skill.pin sends
 * pinned=true and skill.unpin sends pinned=false from one shared
 * marshaller; per-method specs make that a constant rather than a branch. */
#define SRC_CONST            "const"
#define SRC_POSITIONAL_ARRAY "positional_array"
#define SRC_FIRST_OF         "first_of"
#define SRC_CWD              "cwd"
#define SRC_SESSION          "session"

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
/* Two more lenient parses, because "lenient" was never one convention. The
 * marshallers use atoi(), atoll() and atof(), and they disagree on inputs that
 * actually occur: atoi() cannot carry a memory id above 2^31, and atof() keeps
 * a fractional confidence that atoi() would floor to 0.
 *
 * memory.delete was SHIPPING with this wrong -- its spec said number_lenient
 * while the marshaller used atoll(), so a large id would have been truncated by
 * the thin client and would have addressed a different row. The differential
 * test did not catch it because every generated id was small. That is the same
 * blind spot as user_capture's length limit, and the same fix: samples chosen
 * to straddle the boundary, not samples derived from the spec. */
/* strtoul(): eval.run parses --seed this way, and it differs from atoll on a
 * negative (which wraps rather than staying negative). */
#define TYPE_NUMBER_LAX_ULONG "number_lenient_ulong"
#define TYPE_NUMBER_LAX_I64   "number_lenient_int64"
#define TYPE_NUMBER_LAX_REAL  "number_lenient_real"
#define TYPE_BOOL_INVERTED    "bool_inverted"
/* A constant STRING emitted when a flag is present: --review sends
 * "status": "ambiguous". Exactly true_if_set with a literal other than true,
 * so the same reasoning admits it -- one field, its own flag, no branch. The
 * literal travels in `value`. */
#define TYPE_CONST_BOOL   "const_bool"
#define TYPE_CONST_IF_SET "const_if_set"
/* One field, two flags, three states: --surprise sends true, --no-surprise
 * sends false, neither sends nothing. `flag` carries the true spelling and
 * `false_flag` the false one, with the true one winning if both are given --
 * which is what the `else if` in the marshaller means.
 *
 * Still one field consulting nothing but its own flags. The line this
 * vocabulary holds is that no field's presence may depend on ANOTHER field, and
 * no branch may decide which fields exist; a field with two of its own spellings
 * crosses neither. */
#define TYPE_TRISTATE_FLAG "tristate_flag"

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

/* `min`/`max` clamp a number into a range the server will accept:
 * insights.overview pins --days into [1, 365], so `--days 0` means 1 and
 * `--days 9999` means 365. Like omit_if_nonpositive, this is a rule about ONE
 * field's own value with no reference to any other field, which is the line
 * this vocabulary holds. It applies to the default too, because the marshaller
 * clamps after cli_args_get_int() has supplied it. */
static double clamped(const cJSON *field, double v)
{
   const cJSON *lo = cJSON_GetObjectItemCaseSensitive(field, "min");
   const cJSON *hi = cJSON_GetObjectItemCaseSensitive(field, "max");
   if (cJSON_IsNumber(lo) && v < lo->valuedouble)
      v = lo->valuedouble;
   if (cJSON_IsNumber(hi) && v > hi->valuedouble)
      v = hi->valuedouble;
   return v;
}

static const char *field_str(const cJSON *field, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(field, key);
   return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static int known_source(const char *from)
{
   return from &&
          (!strcmp(from, SRC_FLAG) || !strcmp(from, SRC_POSITIONAL) ||
           !strcmp(from, SRC_POSITIONAL_FLAG) || !strcmp(from, SRC_FLAG_POSITIONAL) ||
           !strcmp(from, SRC_ARGV_JOINED) || !strcmp(from, SRC_ARGV_ARRAY) ||
           !strcmp(from, SRC_ARGV_INDEX) || !strcmp(from, SRC_REPEATED_FLAG) ||
           !strcmp(from, SRC_CWD) || !strcmp(from, SRC_SESSION) || !strcmp(from, SRC_FIRST_OF) ||
           !strcmp(from, SRC_POSITIONAL_ARRAY) || !strcmp(from, SRC_CONST));
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
          !strcmp(type, TYPE_NUMBER_LAX) || !strcmp(type, TYPE_NUMBER_LAX_I64) ||
          !strcmp(type, TYPE_NUMBER_LAX_REAL) || !strcmp(type, TYPE_NUMBER_LAX_ULONG) ||
          !strcmp(type, TYPE_BOOL) || !strcmp(type, TYPE_BOOL_INVERTED) ||
          !strcmp(type, TYPE_CONST_IF_SET) || !strcmp(type, TYPE_CONST_BOOL) ||
          !strcmp(type, TYPE_TRISTATE_FLAG) || !strcmp(type, TYPE_TRUE_IF_SET);
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
         if (ty && !strcmp(ty, TYPE_TRISTATE_FLAG) && !field_str(f, "false_flag"))
            return 0;
         if (ty && !strcmp(ty, TYPE_CONST_IF_SET))
         {
            const cJSON *lv = cJSON_GetObjectItemCaseSensitive(f, "value");
            if (!lv || !(cJSON_IsString(lv) || cJSON_IsBool(lv)))
               return 0; /* a constant field with no usable constant */
         }
      }
      /* A source must carry what it reads from, or the row means nothing. */
      if ((!strcmp(from, SRC_FLAG) || !strcmp(from, SRC_POSITIONAL_FLAG) ||
           !strcmp(from, SRC_FLAG_POSITIONAL)) &&
          !field_str(f, "flag"))
         return 0;
      if (!strcmp(from, SRC_POSITIONAL) || !strcmp(from, SRC_POSITIONAL_FLAG) ||
          !strcmp(from, SRC_FLAG_POSITIONAL))
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
static const char *field_value(const cJSON *field, const cli_args_t *opts, const char *joined,
                               int argc, char **argv)
{
   const char *from = field_str(field, "from");
   const char *flag = field_str(field, "flag");

   if (!strcmp(from, SRC_FIRST_OF))
   {
      const cJSON *srcs = cJSON_GetObjectItemCaseSensitive(field, "sources");
      /* Advance on ABSENT, not on empty. `--task ""` is a value the operator
       * typed: it stops the cascade, and the empty string then loses to the
       * default at the emit step. Skipping it here would let --query win, which
       * is a different request from the one the compiled marshaller sends. */
      for (const cJSON *s = cJSON_IsArray(srcs) ? srcs->child : NULL; s; s = s->next)
      {
         const char *v = field_value(s, opts, joined, argc, argv);
         if (v)
         {
            if (v[0])
               return v;
            /* Present but empty: a default replaces it if the marshaller has
             * one (memory.recall falls back to "session start"), and otherwise
             * the empty string IS the value -- session.attach sends "" for an
             * empty positional rather than dropping the field. */
            /* Two conventions, both real. memory.recall tests `task && task[0]`
             * so an empty value takes the default; memory.benchmark tests
             * `pos_count >= 1` so an empty positional IS the value. The spec has
             * to say which, and standing is the commoner one. */
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(field, "default_on_empty")))
               return v;
            const char *d = field_str(field, "default");
            return d ? d : v;
         }
      }
      return field_str(field, "default");
   }

   if (!strcmp(from, SRC_CWD))
   {
      /* One buffer, because a request carries this field at most once and the
       * marshalling path is single-threaded. getcwd() failing means the field
       * is omitted, which is what every compiled marshaller does. */
      static char cwd[4096];
      return getcwd(cwd, sizeof(cwd)) ? cwd : NULL;
   }

   if (!strcmp(from, SRC_SESSION))
   {
      /* resolve_session_env(): --session, then $AIMEE_SESSION_ID, then the
       * literal "default". A fixed precedence over one flag and one named
       * variable -- no branch decides which fields exist, and no other field is
       * consulted. */
      const char *s = cli_args_get(opts, flag && flag[0] ? flag : "session");
      if (s)
         return s;
      s = getenv("AIMEE_SESSION_ID");
      if (s && s[0])
         return s;
      return "default";
   }

   if (!strcmp(from, SRC_ARGV_JOINED))
      return joined && joined[0] ? joined : NULL;

   if (!strcmp(from, SRC_ARGV_INDEX))
   {
      const cJSON *ai = cJSON_GetObjectItemCaseSensitive(field, "index");
      int aidx = cJSON_IsNumber(ai) ? (int)ai->valuedouble : 0;
      /* Same empty-guard question as a positional, and both answers are in
       * use: memory.delete guards on `argc > 0` alone and so sends id from an
       * empty word, while provider.set also tests argv[0][0] and drops it. */
      /* Some raw-argv reads refuse a word that looks like a flag:
       * mcp.recheck tests `argv[0][0] != '-'`. Per-field, and about this
       * field's own value, so it travels with the field. */
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(field, "skip_if_dash")) && argc > aidx &&
          argv[aidx] && argv[aidx][0] == '-')
         return NULL;
      const char *ae = field_str(field, "empty");
      int argv_emit_empty = ae && !strcmp(ae, EMPTY_EMIT);
      if (argc <= aidx || !argv[aidx])
         return NULL;
      return (argv_emit_empty || argv[aidx][0]) ? argv[aidx] : NULL;
   }

   /* `alt_flag` is a SECOND SPELLING of the same field: memory.get accepts
    * --as-of and --as_of, and the marshaller tries them in that order. One
    * field consulting nothing but its own flags -- the same shape tristate_flag
    * already has, and it crosses nothing the line forbids. */
   const char *alt = field_str(field, "alt_flag");

   if (!strcmp(from, SRC_FLAG))
   {
      const char *v = cli_args_get(opts, flag);
      if ((!v || !v[0]) && alt)
      {
         const char *av = cli_args_get(opts, alt);
         if (av)
            return av;
      }
      return v;
   }

   if (!strcmp(from, SRC_FLAG_POSITIONAL))
   {
      const char *v = cli_args_get(opts, flag);
      if (v && v[0])
         return v;
      const cJSON *fi = cJSON_GetObjectItemCaseSensitive(field, "index");
      int fidx = cJSON_IsNumber(fi) ? (int)fi->valuedouble : 0;
      return (opts->pos_count > fidx && opts->positional[fidx] && opts->positional[fidx][0])
                 ? opts->positional[fidx]
                 : NULL;
   }

   const cJSON *idx = cJSON_GetObjectItemCaseSensitive(field, "index");
   int i = (int)idx->valuedouble;
   /* `"from_end": true` counts back from the last positional, so index 0 is the
    * last one. delegate.backend_exec takes its command from positional
    * [pos_count - 1] because the command is typically quoted into a single
    * slot and any number of flags may precede it. Still only WHERE the value
    * comes from -- no other field is consulted. */
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(field, "from_end")))
   {
      if (opts->pos_count <= i)
         return NULL;
      i = opts->pos_count - 1 - i;
   }
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

   /* `count_min` / `count_max` gate a field on how many POSITIONALS were typed.
    * index.hybrid sends "query" for exactly one and "queries" (an array) for
    * more; skill.pin reaches its constant only past `if (argc < 1) return`.
    *
    * I refused this shape for most of this work as "a branch decides which
    * fields exist". That was the wrong line, in the same way "the client must
    * never read its own cwd" was: this vocabulary is FULL of conditionals --
    * empty:"drop", omit_if_nonpositive, omit_below, max_positionals, the
    * first_of cascade -- and none of them made a spec into a program. What
    * would is a rule that consults ANOTHER FIELD's value, or computes. An arity
    * gate consults the invocation's shape, which is exactly what
    * max_positionals already does one level up, and it is bounded, declarative
    * and inspectable.
    *
    * The differential test is what keeps this honest: with the arity samples it
    * now carries, a spec that gates on the wrong count disagrees with the
    * marshaller on the count it got wrong. */
   /* `argc_min` gates on the RAW argv count, which is not the same number as
    * the positional count and not interchangeable with it: skill.pin guards on
    * `argc < 1` and then reads argv[0], so `aimee skill pin --x y` has argc 2,
    * pos_count 0, and sends name="--x". A count_min gate would have dropped it. */
   const cJSON *amin = cJSON_GetObjectItemCaseSensitive(field, "argc_min");
   if (cJSON_IsNumber(amin) && argc < (int)amin->valuedouble)
      return 0;
   const cJSON *cmin = cJSON_GetObjectItemCaseSensitive(field, "count_min");
   const cJSON *cmax = cJSON_GetObjectItemCaseSensitive(field, "count_max");
   if (cJSON_IsNumber(cmin) && opts->pos_count < (int)cmin->valuedouble)
      return 0;
   if (cJSON_IsNumber(cmax) && opts->pos_count > (int)cmax->valuedouble)
      return 0;
   const char *type = field_str(field, "type");
   int required = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(field, "required"));

   const char *from = field_str(field, "from");
   /* `equals` / `not_equals`: emit only when the field's OWN source value does
    * or does not match a literal. skill.lint reads argv[0] and sends `all: true`
    * when it is exactly "--all" and `name` when it is anything else -- the
    * marshaller compares raw argv because it never calls cli_args_parse.
    *
    * A rule about this field's own value, like skip_if_dash, which is the same
    * test with a prefix instead of a literal. NOT a licence to consult another
    * slot: skill.archive gates on argv[1] while reading argv[2], and stays
    * refused, because that is another field's value. */
   {
      const char *eq = field_str(field, "equals");
      const char *ne = field_str(field, "not_equals");
      if (eq || ne)
      {
         const char *v = field_value(field, opts, joined, argc, argv);
         if (eq && (!v || strcmp(v, eq) != 0))
            return 0;
         if (ne && v && strcmp(v, ne) == 0)
            return 0;
      }
   }

   if (type && !strcmp(type, TYPE_CONST_BOOL))
   {
      cJSON_AddBoolToObject(req, json_name,
                            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(field, "value")));
      return 0;
   }
   if (from && !strcmp(from, SRC_POSITIONAL_ARRAY))
   {
      cJSON *arr = cJSON_CreateArray();
      if (!arr)
         return -1;
      for (int i = 0; i < opts->pos_count; i++)
         cJSON_AddItemToArray(arr, cJSON_CreateString(opts->positional[i]));
      cJSON_AddItemToObject(req, json_name, arr);
      return 0;
   }
   if (from && !strcmp(from, SRC_REPEATED_FLAG))
   {
      const char *want = field_str(field, "flag");
      cJSON *arr = cJSON_CreateArray();
      if (!arr)
         return -1;
      for (int i = 0; i < opts->flag_count; i++)
      {
         const char *raw = opts->flags[i].raw;
         const char *eq = raw ? strchr(raw, '=') : NULL;
         size_t rlen = eq ? (size_t)(eq - raw) : (raw ? strlen(raw) : 0);
         if (raw && want && rlen == strlen(want) && memcmp(raw, want, rlen) == 0 &&
             opts->flags[i].value && opts->flags[i].value[0])
            cJSON_AddItemToArray(arr, cJSON_CreateString(opts->flags[i].value));
      }
      /* Omitted rather than empty when the flag never appeared: an empty array
       * and an absent field are different requests. */
      if (cJSON_GetArraySize(arr) > 0)
         cJSON_AddItemToObject(req, json_name, arr);
      else
         cJSON_Delete(arr);
      return 0;
   }

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

   if (type && !strcmp(type, TYPE_TRISTATE_FLAG))
   {
      const char *ff = field_str(field, "false_flag");
      if (cli_args_get(opts, field_str(field, "flag")))
         cJSON_AddTrueToObject(req, json_name);
      else if (ff && cli_args_get(opts, ff))
         cJSON_AddFalseToObject(req, json_name);
      return 0;
   }

   if (type && !strcmp(type, TYPE_CONST_IF_SET))
   {
      /* The constant may be a STRING or a BOOLEAN: --review sends
       * "status": "ambiguous", --no-scan sends "scan": false. Same rule, and
       * splitting it into two types would only make a caller pick between
       * them by the shape of the literal they already wrote. */
      const cJSON *lit = cJSON_GetObjectItemCaseSensitive(field, "value");
      if (!lit || !(cJSON_IsString(lit) || cJSON_IsBool(lit)))
         return -1;
      if (cli_args_get(opts, field_str(field, "flag")))
      {
         if (cJSON_IsString(lit))
            cJSON_AddStringToObject(req, json_name, lit->valuestring);
         else
            cJSON_AddBoolToObject(req, json_name, cJSON_IsTrue(lit));
      }
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
   /* `omit_below` is the same idea with the threshold named: session.attach
    * takes --subscribe defaulting to -1 and sends it only when >= 0, so ZERO is
    * a real value there and omit_if_nonpositive would have swallowed it. */
   const cJSON *omit_below = cJSON_GetObjectItemCaseSensitive(field, "omit_below");
   const char *value = field_value(field, opts, joined, argc, argv);
   /* `default` reproduces cli_args_get_int(opts, name, def): the field is
    * emitted even when the flag is absent, carrying the default. Absent means
    * "omit when absent", which is what every other field does. */
   const cJSON *dflt = cJSON_GetObjectItemCaseSensitive(field, "default");
   /* ABSENT takes the default; present-but-empty is a value the operator typed
    * and is converted like any other. The two were conflated here until
    * insights.overview showed the difference: `--days ""` reaches
    * cli_args_get_int(), which atoi()s "" to 0 and then clamps it to 1, while
    * omitting --days entirely yields 30. A single `!value[0]` test cannot tell
    * those apart, and with empty:"emit" it silently dropped the default. */
   if (cJSON_IsNumber(dflt) && (!value || (!value[0] && !emit_empty)))
   {
      if (!(omit_nonpositive && dflt->valuedouble <= 0) &&
          !(cJSON_IsNumber(omit_below) && dflt->valuedouble < omit_below->valuedouble))
         cJSON_AddNumberToObject(req, json_name, clamped(field, dflt->valuedouble));
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
      if (cJSON_IsNumber(omit_below) && n < (int)omit_below->valuedouble)
         return 0;
      cJSON_AddNumberToObject(req, json_name, clamped(field, (double)n));
   }
   else if (!strcmp(type, TYPE_NUMBER_LAX_I64))
   {
      /* atoll(): the same leniency, 64 bits wide. */
      long long n = atoll(value);
      if (omit_nonpositive && n <= 0)
         return 0;
      cJSON_AddNumberToObject(req, json_name, clamped(field, (double)n));
   }
   else if (!strcmp(type, TYPE_NUMBER_LAX_ULONG))
   {
      unsigned long n = strtoul(value, NULL, 10);
      if (omit_nonpositive && n == 0)
         return 0;
      cJSON_AddNumberToObject(req, json_name, clamped(field, (double)n));
   }
   else if (!strcmp(type, TYPE_NUMBER_LAX_REAL))
   {
      /* atof(): leading real, 0.0 for anything else, no refusal. */
      double n = atof(value);
      if (omit_nonpositive && n <= 0)
         return 0;
      cJSON_AddNumberToObject(req, json_name, clamped(field, n));
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
      cJSON_AddNumberToObject(req, json_name, clamped(field, d));
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

   /* `max_positionals` refuses an invocation that carries MORE positionals than
    * the method accepts. delegate.log takes none at all and says so, pointing
    * the operator at `aimee jobs logs <job_id>` instead of quietly ignoring the
    * id they typed.
    *
    * This is an ARITY rule, not a field rule, which is why it sits on the spec
    * rather than on a field. It crosses neither half of the line: no field's
    * presence depends on another field, and no branch decides which fields
    * exist -- there are none to decide. */
   int missing = 0;
   const cJSON *maxpos = cJSON_GetObjectItemCaseSensitive(spec, "max_positionals");
   if (cJSON_IsNumber(maxpos) && opts.pos_count > (int)maxpos->valuedouble)
      missing = 1;

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
