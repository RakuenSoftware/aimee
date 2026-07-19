/* audit_action.c: governed-action audit primitives. See audit_action.h for the
 * args_hash contract. */
#include "audit_action.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee_home.h"
#include "cJSON.h"
#include "platform_path.h"
#include "platform_random.h" /* platform_random_bytes (portable CSPRNG) */
#include "wfe_def.h"         /* wfe_sha256_raw */

/* O_NOFOLLOW / O_CLOEXEC are POSIX hardening flags absent on some toolchains
 * (e.g. MinGW). Degrade to no-ops there — the audit key is server-side (POSIX);
 * on Windows this build path is the client, where the flags do not apply. */
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define AUDIT_KEY_LEN 32

/* Bounds. Truncation markers and length prefixes are folded INTO the hash input
 * so the digest stays stable and verifiable when a limit is hit. */
#define AUDIT_ARGS_MAX_INPUT  (256 * 1024)             /* skip parsing beyond this */
#define AUDIT_VALUE_MAX_BYTES 8192                     /* per allowlisted value */
#define AUDIT_CANON_ALLOC     (64 * 1024)              /* canon buffer allocation */
#define AUDIT_CANON_LIMIT     (AUDIT_CANON_ALLOC - 32) /* logical fill limit (marker reserve) */

/* Component separator. NOTE: canonical components are LENGTH-PREFIXED (see
 * canon_add), so injectivity does NOT depend on this byte being absent from a
 * value — a value may contain it (e.g. via a  JSON escape) without forging
 * a boundary. The separator is retained only for readability of the hash input. */
#define SEP "\x1f"

/* ---- per-tool allowlist -------------------------------------------------- */

typedef struct
{
   const char *tool;
   const char *fields[6]; /* NULL-terminated; decision-relevant args only */
} tool_allowlist_t;

/* Only decision-relevant fields per governed tool. A tool absent here hashes its
 * NAME ONLY. Field order here IS the canonical order — do not reorder without
 * bumping the version prefix. */
static const tool_allowlist_t ALLOWLIST[] = {
    {"Write", {"file_path", "content", NULL}},
    {"Edit", {"file_path", "old_string", "new_string", NULL}},
    {"NotebookEdit", {"notebook_path", "new_source", NULL}},
    {"Read", {"file_path", NULL}},
    {"Bash", {"command", NULL}},
    {"execute_script", {"command", "script", NULL}},
    {"WebFetch", {"url", NULL}},
    {"WebSearch", {"query", NULL}},
};

static const tool_allowlist_t *allowlist_for(const char *tool)
{
   if (!tool)
      return NULL;
   for (size_t i = 0; i < sizeof(ALLOWLIST) / sizeof(ALLOWLIST[0]); i++)
      if (strcmp(ALLOWLIST[i].tool, tool) == 0)
         return &ALLOWLIST[i];
   return NULL;
}

/* ---- key management (mirrors wfe_approval_ensure_key) --------------------- */

static void audit_key_path(char *buf, size_t cap)
{
   snprintf(buf, cap, "%s/.audit-key", aimee_home());
}

static int audit_load_key(unsigned char key[AUDIT_KEY_LEN])
{
   char path[1024];
   audit_key_path(path, sizeof path);
   int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return -1;
   ssize_t n = read(fd, key, AUDIT_KEY_LEN);
   close(fd);
   return n == (ssize_t)AUDIT_KEY_LEN ? 0 : -1;
}

int audit_ensure_key(void)
{
   unsigned char key[AUDIT_KEY_LEN];
   if (audit_load_key(key) == 0)
      return 0;
   char path[1024];
   audit_key_path(path, sizeof path);
   /* Atomic, 0600-from-creation, no-symlink-follow: no world-readable window and
    * no first-run race corrupting the key. */
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0)
   {
      /* Someone else created it concurrently — accept iff it now loads. */
      return audit_load_key(key) == 0 ? 0 : -1;
   }
   int ok = 0;
   if (platform_random_bytes(key, AUDIT_KEY_LEN) == 0)
   {
      ssize_t w = write(fd, key, AUDIT_KEY_LEN);
      ok = (w == (ssize_t)AUDIT_KEY_LEN);
   }
   close(fd);
   if (!ok)
   {
      unlink(path);
      return -1;
   }
   platform_set_permissions(path, 0600);
   return 0;
}

/* ---- HMAC-SHA256 over wfe_sha256_raw ------------------------------------- */

/* Returns 0 on success, -1 on failure. On failure the caller MUST NOT emit a
 * digest (never an unkeyed hash). */
static int hmac_sha256(const unsigned char *key, size_t keylen, const unsigned char *msg,
                       size_t mlen, unsigned char mac[32])
{
   unsigned char k[64];
   memset(k, 0, sizeof k);
   if (keylen > 64)
      wfe_sha256_raw(key, keylen, k); /* key = H(key): 32 bytes, rest zero */
   else
      memcpy(k, key, keylen);

   unsigned char ipad[64], opad[64];
   for (int i = 0; i < 64; i++)
   {
      ipad[i] = k[i] ^ 0x36;
      opad[i] = k[i] ^ 0x5c;
   }

   if (mlen > SIZE_MAX - 64)
      return -1; /* addition overflow guard */
   unsigned char *inner_in = malloc(64 + mlen);
   if (!inner_in)
      return -1; /* hard fail — never fall back to an unkeyed digest */
   memcpy(inner_in, ipad, 64);
   if (mlen)
      memcpy(inner_in + 64, msg, mlen);
   unsigned char inner[32];
   wfe_sha256_raw(inner_in, 64 + mlen, inner);
   free(inner_in);

   unsigned char outer_in[96]; /* opad(64) || inner(32) */
   memcpy(outer_in, opad, 64);
   memcpy(outer_in + 64, inner, 32);
   wfe_sha256_raw(outer_in, 96, mac);
   return 0;
}

int audit_hmac_sha256_testonly(const unsigned char *key, size_t keylen, const unsigned char *msg,
                               size_t mlen, unsigned char mac[32])
{
   return hmac_sha256(key, keylen, msg, mlen, mac);
}

/* ---- canonical projection (length-prefixed, injective) ------------------- */

/* Append `len` bytes at canon[*pos], capped at AUDIT_CANON_LIMIT. On overflow,
 * fold a stable "<trunc>" marker into the reserved slack (still within the
 * allocation) and freeze so no later component is appended. */
static void canon_append(char *canon, size_t *pos, const char *s, size_t len)
{
   if (*pos >= AUDIT_CANON_LIMIT)
      return; /* frozen */
   size_t room = AUDIT_CANON_LIMIT - *pos;
   size_t take = len < room ? len : room;
   memcpy(canon + *pos, s, take);
   *pos += take;
   if (take < len)
   {
      static const char mark[] = SEP "<trunc>";
      size_t mlen = sizeof(mark) - 1; /* fits in the ALLOC-LIMIT reserve */
      memcpy(canon + *pos, mark, mlen);
      *pos += mlen; /* now >= AUDIT_CANON_LIMIT -> frozen; marker IS hashed */
   }
}

/* Append one length-prefixed component: SEP <declen> ":" <bytes>. The length
 * precedes the bytes, so no in-value byte can forge a component boundary — the
 * canonical form is injective over (tool, projected-fields) regardless of value
 * content. */
static void canon_add(char *canon, size_t *pos, char tag, const char *bytes, size_t len)
{
   char pre[40];
   int n = snprintf(pre, sizeof pre, SEP "%c%zu:", tag, len);
   if (n < 0)
      n = 0;
   if ((size_t)n > sizeof pre)
      n = (int)sizeof pre;
   canon_append(canon, pos, pre, (size_t)n);
   canon_append(canon, pos, bytes, len);
}

/* Materialize a cJSON value to bounded bytes and append it length-prefixed. The
 * 'T'/'F' tag records whether the value was truncated, so a capped 8 KiB value
 * is distinct from an exact 8 KiB value. */
static void canon_add_value(char *canon, size_t *pos, const cJSON *v)
{
   char *owned = NULL;
   const char *text;
   size_t len;
   char numbuf[64];
   if (cJSON_IsString(v) && v->valuestring)
   {
      text = v->valuestring;
      len = strlen(text);
   }
   else if (cJSON_IsNumber(v))
   {
      snprintf(numbuf, sizeof numbuf, "%.17g", v->valuedouble);
      text = numbuf;
      len = strlen(numbuf);
   }
   else if (cJSON_IsBool(v))
   {
      text = cJSON_IsTrue(v) ? "true" : "false";
      len = strlen(text);
   }
   else if (cJSON_IsNull(v))
   {
      text = "null";
      len = 4;
   }
   else
   {
      /* Non-scalar (array/object): compact-print. Bounded by the parsed input
       * (<= AUDIT_ARGS_MAX_INPUT); only AUDIT_VALUE_MAX_BYTES enter the hash. */
      owned = cJSON_PrintUnformatted(v);
      text = owned ? owned : "";
      len = strlen(text);
   }
   char tag = 'F';
   if (len > AUDIT_VALUE_MAX_BYTES)
   {
      len = AUDIT_VALUE_MAX_BYTES;
      tag = 'T';
   }
   canon_add(canon, pos, tag, text, len);
   if (owned)
      free(owned);
}

int audit_args_hash(const char *tool_name, const char *args_json, char *out, size_t out_sz)
{
   /* Stable sentinel for any failure path (never a forgeable/unkeyed digest). */
   if (out && out_sz >= AUDIT_ARGS_HASH_LEN)
   {
      memcpy(out, "v1-", 3);
      memset(out + 3, '0', 64);
      out[67] = '\0';
   }
   if (!out || out_sz < AUDIT_ARGS_HASH_LEN)
      return -1;

   unsigned char key[AUDIT_KEY_LEN];
   if (audit_load_key(key) != 0)
      return -1; /* no key -> caller skips the row (never HMAC-over-empty) */

   char *canon = malloc(AUDIT_CANON_ALLOC);
   if (!canon)
      return -1;
   size_t pos = 0;

   /* Always lead with the (length-prefixed) tool name; name-only for unknown tools. */
   canon_add(canon, &pos, 'N', tool_name ? tool_name : "", tool_name ? strlen(tool_name) : 0);

   const tool_allowlist_t *al = allowlist_for(tool_name);
   if (al && args_json && *args_json)
   {
      size_t jlen = strnlen(args_json, AUDIT_ARGS_MAX_INPUT + 1);
      if (jlen > AUDIT_ARGS_MAX_INPUT)
      {
         /* Oversize input: fold a stable marker, skip parsing (DoS bound). Uses
          * strnlen so we never scan past the cap. */
         canon_add(canon, &pos, 'O', "oversize", 8);
      }
      else
      {
         cJSON *root = cJSON_Parse(args_json);
         if (root)
         {
            for (const char *const *f = al->fields; *f; f++)
            {
               cJSON *item = cJSON_GetObjectItemCaseSensitive(root, *f);
               if (!item)
                  continue; /* absent field contributes nothing */
               canon_add(canon, &pos, 'K', *f, strlen(*f));
               canon_add_value(canon, &pos, item);
            }
            cJSON_Delete(root); /* cJSON_Delete(NULL) is a no-op; safe */
         }
         /* Unparseable JSON: tool-name only (values never guessed). */
      }
   }

   unsigned char mac[32];
   int hrc = hmac_sha256(key, AUDIT_KEY_LEN, (const unsigned char *)canon, pos, mac);
   free(canon);
   if (hrc != 0)
      return -1; /* keep the sentinel; never emit an unkeyed digest */

   static const char hx[] = "0123456789abcdef";
   memcpy(out, "v1-", 3);
   for (int i = 0; i < 32; i++)
   {
      out[3 + i * 2] = hx[(mac[i] >> 4) & 0xf];
      out[3 + i * 2 + 1] = hx[mac[i] & 0xf];
   }
   out[67] = '\0';
   return 0;
}

/* ---- arg-free command preview (human-readable audit row field) ----------- */

#define AUDIT_CMDNAME_MAX  64 /* per emitted program basename */
#define AUDIT_PREVIEW_CMDS 8  /* max programs surfaced per command line */

/* A shell control operator: the token after one is again in COMMAND position.
 * A newline is deliberately NOT here: it is treated as plain whitespace (a token
 * separator, not a command separator) so that a heredoc / multi-line DATA body —
 * whose lines are arguments, not commands — can never place a data token in
 * command position and leak it. Parentheses are deliberately NOT here either:
 * treating `(` as a command boundary let a `NAME=(v1 v2)` array-assignment VALUE
 * reach command position; leaving them out folds the whole `NAME=(...)` into one
 * env-assignment token (which is skipped), while a genuine subshell `(cmd ...)`
 * still surfaces its command via the leading-paren strip in emit_cmd_basename.
 * Real multi-command lines still split on the explicit ; | & operators. */
static int is_cmd_delim(char c)
{
   return c == '|' || c == '&' || c == ';';
}

/* Whitespace between tokens: spaces, tabs, and newlines/CR (see is_cmd_delim on
 * why a newline is a separator, not a command boundary). */
static int is_cmd_space(char c)
{
   return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Characters we are willing to surface in a program name. A crafted `command`
 * value can therefore never inject whitespace, quotes, or control bytes into the
 * audit row: an out-of-set byte simply ends the emitted name. */
static int is_cmdname_char(unsigned char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
          c == '_' || c == '-' || c == '+' || c == '$' || c == '@' || c == ':';
}

/* True if tok[0..len) is a leading env-assignment (NAME=...): NAME is
 * [A-Za-z_][A-Za-z0-9_]* followed by '='. Such a token in command position is an
 * assignment (its VALUE may be a secret, e.g. AWS_SECRET=...) and is redacted:
 * the command is the first NON-assignment token. */
static int is_env_assignment(const char *tok, size_t len)
{
   if (len == 0 ||
       !((tok[0] >= 'A' && tok[0] <= 'Z') || (tok[0] >= 'a' && tok[0] <= 'z') || tok[0] == '_'))
      return 0;
   size_t j = 0;
   while (j < len && ((tok[j] >= 'A' && tok[j] <= 'Z') || (tok[j] >= 'a' && tok[j] <= 'z') ||
                      (tok[j] >= '0' && tok[j] <= '9') || tok[j] == '_'))
      j++;
   if (j < len && tok[j] == '+') /* append-assignment NAME+=... (arrays/strings) */
      j++;
   return j < len && tok[j] == '=';
}

/* Classify a shell reserved word / compound-command keyword at command position.
 * These are never programs, so they are skipped (never emitted); the return value
 * says what the NEXT token is:
 *   RW_COMMAND (1): a command follows (if/then/do/... ) -> STAY at command
 *      position so the real governed command surfaces (`if grep x; then rm y; fi`
 *      -> "grep ; rm", not "if ; then ; fi").
 *   RW_OPERAND (2): a NON-command operand follows -> DROP to argument position so
 *      it is never emitted. This is the security-critical class: after `case`
 *      the next token is the SUBJECT (which may be a literal value, e.g. a PII
 *      bareword); after `for`/`select` it is the loop variable; after `function`
 *      it is the function name. None are programs and none may leak.
 *   0: not a reserved word.
 * Case PATTERNS are separately kept out of command position (the `;;` terminator
 * is not a command boundary), so no pattern value can be emitted either. */
enum
{
   RW_NONE = 0,
   RW_COMMAND = 1,
   RW_OPERAND = 2
};
static int reserved_word_kind(const char *tok, size_t len)
{
   static const char *const RW_CMD[] = {"if",    "then", "elif", "else", "fi", "while",
                                        "until", "do",   "done", "esac", "in", "time",
                                        "!",     "{",    "}",    NULL};
   /* `case` is handled explicitly (case-depth suppression), not here. */
   static const char *const RW_OPD[] = {"for", "select", "function", NULL};
   for (const char *const *w = RW_OPD; *w; w++)
      if (strlen(*w) == len && memcmp(tok, *w, len) == 0)
         return RW_OPERAND;
   for (const char *const *w = RW_CMD; *w; w++)
      if (strlen(*w) == len && memcmp(tok, *w, len) == 0)
         return RW_COMMAND;
   return RW_NONE;
}

/* True if tok[0..len) is all ASCII digits (a leading file-descriptor number on a
 * redirection, e.g. the `2` in `2>err.log`) — skipped so it is neither emitted
 * nor mistaken for the command. */
static int is_all_digits(const char *tok, size_t len)
{
   if (len == 0)
      return 0;
   for (size_t k = 0; k < len; k++)
      if (tok[k] < '0' || tok[k] > '9')
         return 0;
   return 1;
}

/* Append the sanitized basename of the command-position token tok[0..len) to
 * out (" ; "-separated). Only is_cmdname_char bytes after the last '/' are
 * copied, bounded to AUDIT_CMDNAME_MAX; an empty result emits nothing. */
static void emit_cmd_basename(const char *tok, size_t len, char *out, size_t *op, size_t out_sz)
{
   /* A token beginning with '(' is an opaque subshell/group span (e.g.
    * `(cd /x && make)`); its interior is a command LIST, not a single program, and
    * taking a basename across it would surface an inner path segment. Suppress it
    * rather than dive in — commands outside the group still surface. */
   if (len > 0 && tok[0] == '(')
      return;
   size_t bstart = 0; /* start of the basename (past the last '/') */
   for (size_t k = 0; k < len; k++)
      if (tok[k] == '/')
         bstart = k + 1;
   /* Skip leading non-name bytes (e.g. an opening quote on "\"/usr/bin/x\""). */
   while (bstart < len && !is_cmdname_char((unsigned char)tok[bstart]))
      bstart++;
   char name[AUDIT_CMDNAME_MAX + 1];
   size_t np = 0;
   size_t k = bstart;
   for (; k < len && np < AUDIT_CMDNAME_MAX; k++)
   {
      unsigned char ch = (unsigned char)tok[k];
      if (!is_cmdname_char(ch))
         break;
      name[np++] = (char)ch;
   }
   if (np == 0)
      return;
   /* A name glued directly to '(' is a function DEFINITION (`name() {...}`) or a
    * command substitution / arithmetic glued to the name (`$(...)`, `$((...))`),
    * not a program invocation. Suppress it so a function/def name — which is not
    * a governed command — is never emitted. */
   if (k < len && tok[k] == '(')
      return;
   const char *sep = *op > 0 ? " ; " : "";
   size_t seplen = strlen(sep);
   if (*op + seplen + np + 1 >= out_sz)
      return; /* out of room: drop this (and, implicitly, later) commands */
   memcpy(out + *op, sep, seplen);
   *op += seplen;
   memcpy(out + *op, name, np);
   *op += np;
   out[*op] = '\0';
}

/* Scan a shell command line, emitting the basename of each command-position
 * program (skipping env-assignments; dropping all arguments and redirect
 * targets). Best-effort parse: quotes are opaque spans; the arg-free invariant
 * holds regardless because only command-position tokens are ever emitted. */
static void extract_shell_commands(const char *s, char *out, size_t out_sz)
{
   size_t op = 0;
   int at_cmd = 1;     /* start of line is a command position */
   int case_depth = 0; /* >0 while inside a case…esac (all emission suppressed) */
   int ncmds = 0;
   size_t i = 0;
   while (s[i])
   {
      char c = s[i];
      if (is_cmd_space(c))
      {
         i++;
         continue;
      }
      /* A `#` at a word start begins a comment: the rest of the line is free-form
       * text (potentially data), not commands — skip it. (`#` inside a word, e.g.
       * `foo#bar`, is handled by the token reader, not here.) */
      if (c == '#')
      {
         while (s[i] && s[i] != '\n')
            i++;
         continue;
      }
      /* `;;` terminates a case branch: the NEXT token is a case PATTERN, not a
       * command, so it must NOT become a command position (else a pattern value
       * could be emitted). A single `;` is an ordinary command separator. */
      if (c == ';' && s[i + 1] == ';')
      {
         at_cmd = 0;
         i += 2;
         continue;
      }
      if (is_cmd_delim(c))
      {
         at_cmd = 1;
         i++;
         continue;
      }
      /* Heredoc (`<<` / `<<-`, but NOT the `<<<` here-string): its BODY is
       * free-form data on subsequent lines and can contain any shell operator, so
       * no position heuristic can keep body content out of command position. Stop
       * scanning entirely — commands before the heredoc are already emitted; we
       * refuse to parse past it rather than risk leaking a body value. */
      if (c == '<' && s[i + 1] == '<' && s[i + 2] != '<')
         break;
      /* Redirection: consume '>' / '>>' / '<' plus an optional '&fd' and the
       * target word so the (arg) target is never mistaken for a command. Command
       * position is left unchanged. */
      if (c == '<' || c == '>')
      {
         i++;
         if (s[i] == '>' || s[i] == '<')
            i++;
         while (is_cmd_space(s[i]))
            i++;
         if (s[i] == '&')
            i++;
         while (s[i] && !is_cmd_space(s[i]) && !is_cmd_delim(s[i]) && s[i] != '<' && s[i] != '>')
            i++;
         continue;
      }
      /* Read a token, treating quoted spans as opaque. */
      size_t start = i;
      int quoted = 0;
      char q = 0;
      while (s[i])
      {
         char ch = s[i];
         /* A backslash escapes the next byte, keeping it — and any otherwise
          * word-breaking metacharacter (`\;` `\|` `\ `) or an in-quote `\"` — part
          * of THIS token, so a data value like `echo foo\;DATA` or
          * `echo "a\"; DATA"` stays one argument and never reaches command
          * position. Checked before the quote toggle and applied in every context:
          * over-escaping only ever pulls MORE into the current token (a possible
          * missed command), never LESS (which is what could leak). */
         if (ch == '\\')
         {
            i++;
            if (s[i])
               i++;
            continue;
         }
         if (quoted)
         {
            if (ch == q)
               quoted = 0;
            i++;
            continue;
         }
         if (ch == '\'' || ch == '"')
         {
            quoted = 1;
            q = ch;
            i++;
            continue;
         }
         /* Any unquoted parenthesis group is an opaque balanced span: array
          * assignments `NAME=(v1 v2 v3)`, command substitution `$(...)`,
          * arithmetic `$((...))`, and subshells `(a; b)`. Consuming it whole keeps
          * a space inside from splitting its data (array elements, subst args)
          * into a token that reaches command position. A leading-`(` subshell
          * still surfaces its FIRST command via emit_cmd_basename's paren strip. */
         if (ch == '(')
         {
            i++;
            int depth = 1;
            while (s[i] && depth)
            {
               if (s[i] == '(')
                  depth++;
               else if (s[i] == ')')
                  depth--;
               i++;
            }
            continue;
         }
         /* Backtick command substitution: opaque until the closing backtick. */
         if (ch == '`')
         {
            i++;
            while (s[i] && s[i] != '`')
               i++;
            if (s[i])
               i++;
            continue;
         }
         if (is_cmd_space(ch) || ch == '<' || ch == '>' || is_cmd_delim(ch))
            break;
         i++;
      }
      size_t tok_len = i - start;
      int is_case = tok_len == 4 && memcmp(s + start, "case", 4) == 0;
      int is_esac = tok_len == 4 && memcmp(s + start, "esac", 4) == 0;
      /* Inside a case…esac, every token — the subject, the patterns (including
       * `a|b` alternations and the `)` that closes them) and the branch bodies —
       * is suppressed. case pattern grammar cannot be parsed safely enough to
       * distinguish a pattern VALUE from a command, so nothing inside is emitted;
       * we only track nesting until the matching esac. */
      if (case_depth > 0)
      {
         if (is_case)
            case_depth++;
         else if (is_esac)
            case_depth--;
         continue;
      }
      if (!at_cmd)
         continue; /* an argument: redacted (never emitted) */
      if (is_env_assignment(s + start, tok_len))
         continue; /* leading assignment: redacted, still awaiting the command */
      /* A leading file-descriptor on a redirection (the `2` in `2>err`): the '>'
       * broke the token, so `s[i]` is the redirect. Skip the fd; stay awaiting
       * the real command (which the redirect branch will pass through to). */
      if (is_all_digits(s + start, tok_len) && (s[i] == '<' || s[i] == '>'))
         continue;
      if (is_case)
      {
         case_depth = 1; /* open a case: suppress everything through its esac */
         continue;
      }
      int rw = reserved_word_kind(s + start, tok_len);
      if (rw == RW_OPERAND)
      {
         at_cmd = 0; /* keyword's operand (loop var / func name) is not a command
                        and must never be emitted */
         continue;
      }
      if (rw == RW_COMMAND)
         continue; /* keyword, not a program: skip but stay at command position */
      if (ncmds < AUDIT_PREVIEW_CMDS)
         emit_cmd_basename(s + start, tok_len, out, &op, out_sz);
      ncmds++;
      at_cmd = 0;
   }
}

void audit_command_preview(const char *tool_name, const char *args_json, char *out, size_t out_sz)
{
   if (out && out_sz)
      out[0] = '\0';
   if (!out || out_sz < 2 || !tool_name)
      return;
   /* Only shell tools carry a command line. Every other tool's action is named
    * by the row's `tool` field; its arguments are never surfaced. */
   if (strcmp(tool_name, "Bash") != 0 && strcmp(tool_name, "execute_script") != 0)
      return;
   if (!args_json || !*args_json)
      return;
   if (strnlen(args_json, AUDIT_ARGS_MAX_INPUT + 1) > AUDIT_ARGS_MAX_INPUT)
      return; /* oversize: skip parsing (DoS bound) */
   cJSON *root = cJSON_Parse(args_json);
   if (!root)
      return;
   cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
   if (cJSON_IsString(cmd) && cmd->valuestring)
      extract_shell_commands(cmd->valuestring, out, out_sz);
   cJSON_Delete(root);
}
