/* ===================================================================
 * /v1 thin-client routing: route CLI subcommands through the server's native /v1 HTTP endpoints.
 * Unported commands fail in cli_main before reaching the server.
 *
 * This file holds the memory.* family — its marshallers and its printers —
 * moved out of cli_v1_routes.c and cli_v1_routes_c.c, which had both reached
 * the 2500-line hard limit. Splitting by command family rather than by an
 * arbitrary cut keeps a memory change in one file.
 * =================================================================== */

#include "cli_v1_routes_internal.h"
#include "platform_path.h"
#include "cli_client.h"
#define V1_PROTOCOL_VERSION 1
#include "util.h"         /* safe_exec_capture (workspace.mirror-sync ships the client diff) */
#include "aimee_client.h" /* aimee_client_request: transport-agnostic /v1 client (Windows path) */
#include "code_collect.h" /* code_collect_files + code_collect_discover_repos (thin-client push) */
#if !defined(_WIN32) && !defined(_WIN64)
#include "aimee_home.h"
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif /* !_WIN32 (preamble guard) */

/* Every ordered memory command carries the thin client's identity to the
 * server.  Explicit project/workspace values are useful for detached clients;
 * cwd is the normal active-project source and is never resolved on the KB
 * service host. */
static void marshal_add_memory_scope(cJSON *req, const rpc_opts_t *opts)
{
   const char *v;
   if ((v = rpc_get(opts, "project")))
      cJSON_AddStringToObject(req, "project", v);
   if ((v = rpc_get(opts, "workspace")))
      cJSON_AddStringToObject(req, "workspace", v);
   if ((v = rpc_get(opts, "scope")))
      cJSON_AddStringToObject(req, "scope", v);
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
}

cJSON *marshal_memory_search(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("memory.search");

   cJSON *kw = cJSON_CreateArray();
   for (int i = 0; i < opts.pos_count; i++)
      cJSON_AddItemToArray(kw, cJSON_CreateString(opts.positional[i]));
   cJSON_AddItemToObject(req, "keywords", kw);
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 10));
   marshal_add_memory_scope(req, &opts);
   return req;
}

/* `aimee memory recall [task] [--task T] [--query Q] [--session-start]
 * [--limit-tokens N]` -> POST /v1/memory/recall. task_hint is required by the
 * endpoint; fall back to a generic hint so the command always succeeds. */
cJSON *marshal_memory_recall(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("memory.recall");

   const char *task = rpc_get(&opts, "task");
   if (!task && opts.pos_count > 0)
      task = opts.positional[0];
   /* --query asks the same question task_hint answers, so it feeds the same
    * field rather than a second one.
    *
    * It used to be marshalled as its own `query` key that NOTHING read:
    * handle_memory_recall takes task_hint, limit_tokens and session_start, and
    * the only other jo_str(req,"query") in the tree belongs to kb.search. So
    * `aimee memory recall --query "..."` sent the text, had it ignored, and fell
    * back to the "session start" hint -- returning the recency bundle while
    * looking like it had searched. Verified live: a query matching three
    * memories almost verbatim returned none of them.
    *
    * An explicit --task or positional still wins; --query only fills the hint
    * when nothing else did, so no existing invocation changes. */
   if (!task)
      task = rpc_get(&opts, "query");
   cJSON_AddStringToObject(req, "task_hint", task && task[0] ? task : "session start");
   if (rpc_has_flag(&opts, "session-start"))
      cJSON_AddBoolToObject(req, "session_start", 1);
   int lt = rpc_get_int(&opts, "limit-tokens", 0);
   if (lt > 0)
      cJSON_AddNumberToObject(req, "limit_tokens", lt);
   marshal_add_memory_scope(req, &opts);
   return req;
}

/* Join positionals[from..] with single spaces, or NULL when there are none.
 *
 * A memory's content is prose, so an operator or an agent that forgets to quote
 * it hands us one positional per WORD. Reading positional[from] alone stored the
 * first word and threw the rest away -- with exit 0 and "stored memory 60", so
 * nothing anywhere said the memory had been gutted. Observed live: `aimee memory
 * store k one two three four five` stored "one".
 *
 * That is the worst failure a memory system has, because it is silent and it is
 * on the WRITE path: the loss is not discovered when it happens, it is
 * discovered later as a memory that does not match what was meant, or as a
 * search that cannot find what was stored. Joining reconstructs the intended
 * text in the ordinary forgotten-quotes case, and is strictly better than
 * discarding it in every other case.
 *
 * Whitespace runs collapse to one space, which the shell had already destroyed
 * before argv reached us. Caller frees. */
static char *positionals_joined(const rpc_opts_t *opts, int from)
{
   if (!opts || from >= opts->pos_count)
      return NULL;
   size_t need = 1;
   for (int i = from; i < opts->pos_count; i++)
      need += strlen(opts->positional[i]) + 1;
   char *out = malloc(need);
   if (!out)
      return NULL;
   out[0] = '\0';
   size_t pos = 0;
   for (int i = from; i < opts->pos_count; i++)
   {
      if (i > from && pos + 1 < need)
         out[pos++] = ' ';
      const size_t n = strlen(opts->positional[i]);
      if (pos + n < need)
      {
         memcpy(out + pos, opts->positional[i], n);
         pos += n;
      }
   }
   out[pos] = '\0';
   return out;
}

cJSON *marshal_memory_store(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("memory.store");

   /* Accept both positional (`memory store <key> <content>`) and flag
    * (`--key` / `--content`) forms; positional wins when present. Previously the
    * flags were silently ignored, yielding a confusing "missing key or content". */
   const char *key = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "key");
   /* Every positional after the key is content, not just the first. */
   char *joined = positionals_joined(&opts, 1);
   const char *content = joined ? joined : rpc_get(&opts, "content");
   if (key)
      cJSON_AddStringToObject(req, "key", key);
   if (content)
      cJSON_AddStringToObject(req, "content", content);
   free(joined);

   const char *v;
   if ((v = rpc_get(&opts, "tier")))
      cJSON_AddStringToObject(req, "tier", v);
   if ((v = rpc_get(&opts, "kind")))
      cJSON_AddStringToObject(req, "kind", v);
   if ((v = rpc_get(&opts, "session")))
      cJSON_AddStringToObject(req, "session_id", v);
   if ((v = rpc_get(&opts, "confidence")))
      cJSON_AddNumberToObject(req, "confidence", atof(v));
   return req;
}

/* Shared marshaler for the db1 user-capture commands. Requires <key> <value>
 * positionals, rejects a key that would truncate under the prefix (avoids
 * silent collisions), and dispatches as the server op memory.user_capture with
 * kind + prefixed key + tier L2 so recall surfaces it. Returns NULL (a clear
 * usage/limit error) on bad input so cli_v1_forward reports it. */
static cJSON *marshal_user_capture(const char *cmd, const char *kind, const char *prefix,
                                   const char *tier, int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   /* Content may be positional OR --content=<value>. The flag form is required
    * for arbitrary bodies (e.g. the .md migration): rpc_parse would otherwise
    * mis-read a value starting with `--` (frontmatter `---`) as a flag, whereas
    * a --content=... value is taken verbatim after the first '='. */
   const char *keyarg = opts.pos_count > 0 ? opts.positional[0] : NULL;
   /* All remaining positionals, for the same reason as memory.store: an
    * unquoted value arrives one word per positional, and keeping only the first
    * silently stored a fragment of what the operator said about themselves. */
   char *joined = positionals_joined(&opts, 1);
   const char *content = joined ? joined : rpc_get(&opts, "content");
   if (!keyarg || !keyarg[0] || !content || !content[0])
   {
      fprintf(stderr,
              "aimee: usage: aimee memory %s <key> <value>   (or: %s <key> --content=<value>)\n",
              cmd, cmd);
      free(joined);
      return NULL;
   }
   char key[512];
   int need = snprintf(key, sizeof(key), "%s%s", prefix, keyarg);
   if (need < 0 || (size_t)need >= sizeof(key))
   {
      fprintf(stderr, "aimee: memory %s: key too long (max %zu chars)\n", cmd,
              sizeof(key) - strlen(prefix) - 1);
      free(joined);
      return NULL;
   }
   cJSON *req = marshal_no_args("memory.user_capture");
   cJSON_AddStringToObject(req, "kind", kind);
   cJSON_AddStringToObject(req, "tier", tier);
   cJSON_AddStringToObject(req, "key", key);
   cJSON_AddStringToObject(req, "content", content);
   free(joined);
   return req;
}

/* `aimee memory identity <key> <value>` — a per-user identity fact in db1. */
cJSON *marshal_memory_identity(int argc, char **argv)
{
   return marshal_user_capture("identity", "fact", "identity:", "L2", argc, argv);
}

/* `aimee memory prefer <key> <value>` — a per-user preference in db1. */
cJSON *marshal_memory_prefer(int argc, char **argv)
{
   return marshal_user_capture("prefer", "preference", "pref:", "L2", argc, argv);
}

/* `aimee memory archive <name> <body>` — preserve a memory in db1 as a private,
 * NON-RECALLABLE archival row (kind='archive' + tier L1 both keep it out of the
 * L2/fact|preference recall selectors). The .md-retirement migration writes here
 * so nothing is lost and nothing leaks to org (db2) before operator
 * classification. */
cJSON *marshal_memory_archive(int argc, char **argv)
{
   return marshal_user_capture("archive", "archive", "archive:", "L1", argc, argv);
}

cJSON *marshal_memory_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("memory.list");

   const char *v;
   if ((v = rpc_get(&opts, "tier")))
      cJSON_AddStringToObject(req, "tier", v);
   if ((v = rpc_get(&opts, "kind")))
      cJSON_AddStringToObject(req, "kind", v);
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   marshal_add_memory_scope(req, &opts);
   return req;
}

/* `aimee memory get <id> [--as-of <timestamp>]`
 *
 * --as-of asks the EVENT-time question: was this memory in force then?
 * lifecycle_state cannot answer it -- a superseded row looks identically
 * superseded whether it stopped being true yesterday or last year -- so the
 * server reads the valid_from/valid_until interval instead. rpc_parse keeps the
 * id as positional[0], so the flag cannot be mistaken for it. */
cJSON *marshal_memory_get(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("memory.get");
   if (opts.pos_count > 0)
      cJSON_AddNumberToObject(req, "id", atoll(opts.positional[0]));
   const char *as_of = rpc_get(&opts, "as-of");
   if (!as_of)
      as_of = rpc_get(&opts, "as_of");
   if (as_of && as_of[0])
      cJSON_AddStringToObject(req, "as_of", as_of);
   return req;
}

cJSON *marshal_memory_delete(int argc, char **argv)
{
   cJSON *req = marshal_no_args("memory.delete");
   if (argc > 0)
      cJSON_AddNumberToObject(req, "id", atoll(argv[0]));
   return req;
}

/* `aimee memory supersede <old_id> <new_content> [--confidence=N] [--session=S]`
 * — mirrors mem_supersede()'s argument shape so the thin client and the
 * server-host command take the same thing. */
cJSON *marshal_memory_supersede(int argc, char **argv)
{
   cJSON *req = marshal_no_args("memory.supersede");
   int positional = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strncmp(argv[i], "--confidence=", 13) == 0)
         cJSON_AddNumberToObject(req, "confidence", atof(argv[i] + 13));
      else if (strncmp(argv[i], "--session=", 10) == 0)
         cJSON_AddStringToObject(req, "session_id", argv[i] + 10);
      else if (argv[i][0] == '-')
         continue;
      else if (positional == 0)
      {
         cJSON_AddNumberToObject(req, "old_id", atoll(argv[i]));
         positional++;
      }
      else if (positional == 1)
      {
         cJSON_AddStringToObject(req, "new_content", argv[i]);
         positional++;
      }
   }
   return req;
}

cJSON *marshal_memory_read(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("memory.read");
   marshal_add_memory_scope(req, &opts);
   return req;
}

/* Add `field` to req as an absolute path. The benchmark asset paths are
 * resolved against the client's cwd because aimee-server may run with a
 * different working directory; absolute paths are forwarded unchanged. */
static void marshal_add_abs_path(cJSON *req, const char *field, const char *path)
{
   if (!path || !path[0])
      return;
   if (path[0] == '/')
   {
      cJSON_AddStringToObject(req, field, path);
      return;
   }
   char cwd[4096];
   if (!getcwd(cwd, sizeof(cwd)))
   {
      cJSON_AddStringToObject(req, field, path);
      return;
   }
   char abs[4608];
   snprintf(abs, sizeof(abs), "%s/%s", cwd, path);
   cJSON_AddStringToObject(req, field, abs);
}

/* Marshal `aimee memory benchmark [code-graph-fusion] [--arm NAME]
 * [--corpus PATH] [--matrix PATH] [--fusion-state off|shadow|on]` into the
 * memory.benchmark request. Asset paths default on the server to the committed
 * benchmark files; when given here they are made absolute for the server. */
cJSON *marshal_memory_benchmark(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("memory.benchmark");

   const char *suite = (opts.pos_count >= 1) ? opts.positional[0] : "code-graph-fusion";
   cJSON_AddStringToObject(req, "suite", suite);
   const char *arm = rpc_get(&opts, "arm");
   if (arm)
      cJSON_AddStringToObject(req, "arm", arm);
   const char *fstate = rpc_get(&opts, "fusion-state");
   if (fstate)
      cJSON_AddStringToObject(req, "fusion_state", fstate);
   marshal_add_abs_path(req, "corpus", rpc_get(&opts, "corpus"));
   marshal_add_abs_path(req, "matrix", rpc_get(&opts, "matrix"));
   return req;
}

static void print_memory_row(cJSON *m)
{
   if (!cJSON_IsObject(m))
      return;
   cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
   const char *tier = json_str(m, "tier");
   const char *kind = json_str(m, "kind");
   const char *key = json_str(m, "key");
   const char *content = json_str(m, "content");
   printf("%lld  %-3s %-12s %s", cJSON_IsNumber(id) ? (long long)id->valuedouble : 0, tier, kind,
          key[0] ? key : "(no key)");
   if (content[0])
      printf(": %s", content);
   putchar('\n');
}

static void print_memory_search(cJSON *resp)
{
   cJSON *facts = cJSON_GetObjectItemCaseSensitive(resp, "facts");
   cJSON *windows = cJSON_GetObjectItemCaseSensitive(resp, "windows");
   int fact_count = cJSON_IsArray(facts) ? cJSON_GetArraySize(facts) : 0;
   int window_count = cJSON_IsArray(windows) ? cJSON_GetArraySize(windows) : 0;
   if (fact_count == 0 && window_count == 0)
   {
      printf("No memories found.\n");
      return;
   }
   if (fact_count > 0)
   {
      printf("Facts:\n");
      cJSON *m;
      cJSON_ArrayForEach(m, facts)
      {
         printf("  ");
         print_memory_row(m);
      }
   }
   if (window_count > 0)
   {
      printf("Conversation windows:\n");
      cJSON *w;
      cJSON_ArrayForEach(w, windows)
      {
         cJSON *score = cJSON_GetObjectItemCaseSensitive(w, "score");
         printf("  %.4f  %s:%lld  %s\n", cJSON_IsNumber(score) ? score->valuedouble : 0.0,
                json_str(w, "session_id"),
                (long long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(w, "seq")),
                json_str(w, "summary"));
      }
   }
}

static void print_memory_list(cJSON *resp)
{
   cJSON *memories = cJSON_GetObjectItemCaseSensitive(resp, "memories");
   if (!cJSON_IsArray(memories) || cJSON_GetArraySize(memories) == 0)
   {
      printf("No memories.\n");
      return;
   }
   cJSON *m;
   cJSON_ArrayForEach(m, memories)
   {
      print_memory_row(m);
   }
}

static void print_memory_stats(cJSON *resp)
{
   cJSON *stats = cJSON_GetObjectItemCaseSensitive(resp, "stats");
   if (!cJSON_IsObject(stats))
   {
      printf("No memory statistics available.\n");
      return;
   }
   cJSON *total = cJSON_GetObjectItemCaseSensitive(stats, "total");
   cJSON *conflicts = cJSON_GetObjectItemCaseSensitive(stats, "conflicts");
   printf("Memory statistics:\n");
   printf("  total:     %lld\n", (long long)(cJSON_IsNumber(total) ? total->valuedouble : 0));
   printf("  conflicts: %lld\n",
          (long long)(cJSON_IsNumber(conflicts) ? conflicts->valuedouble : 0));
   cJSON *tiers = cJSON_GetObjectItemCaseSensitive(stats, "tier_counts");
   if (cJSON_IsArray(tiers) && cJSON_GetArraySize(tiers) > 0)
   {
      printf("  tier counts:");
      cJSON *t;
      cJSON_ArrayForEach(t, tiers)
          printf(" %lld", (long long)(cJSON_IsNumber(t) ? t->valuedouble : 0));
      printf("\n");
   }
   cJSON *kinds = cJSON_GetObjectItemCaseSensitive(stats, "kind_counts");
   if (cJSON_IsArray(kinds) && cJSON_GetArraySize(kinds) > 0)
   {
      printf("  kind counts:");
      cJSON *k;
      cJSON_ArrayForEach(k, kinds)
          printf(" %lld", (long long)(cJSON_IsNumber(k) ? k->valuedouble : 0));
      printf("\n");
   }
}

void pt_print_memory_search(const char *method, cJSON *resp)
{
   print_memory_search(resp);
}
void pt_print_memory_store(const char *method, cJSON *resp)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "id");
   if (cJSON_IsNumber(id))
      printf("stored memory %lld\n", (long long)id->valuedouble);
   else
      printf("stored memory\n");
}
void pt_print_memory_list(const char *method, cJSON *resp)
{
   print_memory_list(resp);
}
void pt_print_memory_get(const char *method, cJSON *resp)
{
   print_memory_row(resp);
   /* Present only when --as-of was asked. "unknown" is a real third answer: the
    * server could not tell, which is not the same as "not in force". */
   cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "valid_at");
   cJSON *at = cJSON_GetObjectItemCaseSensitive(resp, "as_of");
   if (v && cJSON_IsString(at))
      printf("valid at %s: %s\n", at->valuestring,
             cJSON_IsString(v) ? v->valuestring : (cJSON_IsTrue(v) ? "yes" : "no"));
}
void pt_print_memory_read(const char *method, cJSON *resp)
{
   cJSON *context = cJSON_GetObjectItemCaseSensitive(resp, "context");
   if (cJSON_IsString(context))
      printf("%s", context->valuestring);
}
void pt_print_memory_stats(const char *method, cJSON *resp)
{
   print_memory_stats(resp);
}
