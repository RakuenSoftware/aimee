#ifndef DEC_UTIL_H
#define DEC_UTIL_H 1

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Free the `count` strings in a shlex_split() token array (not the array). */
static inline void util_free_tokens(char **tokens, int count)
{
   for (int i = 0; i < count; i++)
      free(tokens[i]);
}

/* True when `p` is an absolute path in either POSIX (/...) or Windows
 * (C:\... / C:/...) form. The detached reverse-channel carries the CLIENT's cwd,
 * which may be a Windows drive-letter path even though the server is POSIX, so
 * cwd-binding/guard checks that must accept it use this rather than `p[0]=='/'`. */
static inline int aimee_path_is_absolute(const char *p)
{
   if (!p || !p[0])
      return 0;
   if (p[0] == '/')
      return 1;
   return (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
           (p[2] == '\\' || p[2] == '/'));
}

/* 1 if `token` is a shell control/redirection operator (| || & && ; > >> < 2> 2>> >&). */
static inline int util_token_is_shell_operator(const char *token)
{
   if (!token || !token[0])
      return 0;
   return strcmp(token, "|") == 0 || strcmp(token, "||") == 0 || strcmp(token, "&") == 0 ||
          strcmp(token, "&&") == 0 || strcmp(token, ";") == 0 || strcmp(token, ">") == 0 ||
          strcmp(token, ">>") == 0 || strcmp(token, "<") == 0 || strcmp(token, "2>") == 0 ||
          strcmp(token, "2>>") == 0 || strcmp(token, ">&") == 0;
}

/* timegm() is a GNU/BSD extension; declare it so str_parse_iso8601() below
 * compiles in TUs that include util.h without _GNU_SOURCE (e.g. some tests). */
time_t timegm(struct tm *tm);

/* Standard base64-encode `in_len` bytes of `in` into `out` (cap `out_cap`,
 * NUL-terminated). Returns the encoded length, or -1 if it wouldn't fit. */
static inline int util_b64_encode(const char *in, size_t in_len, char *out, size_t out_cap)
{
   static const char b64_alphabet[] =
       "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   size_t out_len = ((in_len + 2) / 3) * 4;
   if (out_len + 1 > out_cap)
      return -1;
   size_t o = 0;
   for (size_t i = 0; i < in_len; i += 3)
   {
      unsigned int v = ((unsigned char)in[i]) << 16;
      if (i + 1 < in_len)
         v |= ((unsigned char)in[i + 1]) << 8;
      if (i + 2 < in_len)
         v |= (unsigned char)in[i + 2];
      out[o++] = b64_alphabet[(v >> 18) & 0x3f];
      out[o++] = b64_alphabet[(v >> 12) & 0x3f];
      out[o++] = (i + 1 < in_len) ? b64_alphabet[(v >> 6) & 0x3f] : '=';
      out[o++] = (i + 2 < in_len) ? b64_alphabet[v & 0x3f] : '=';
   }
   out[o] = '\0';
   return (int)o;
}

/* Lowercase-copy `src` into `dst` (<= dst_len, always NUL-terminated). */
static inline void util_lower_copy(const char *src, char *dst, size_t dst_len)
{
   if (!dst || dst_len == 0)
      return;
   size_t o = 0;
   for (size_t i = 0; src && src[i] && o + 1 < dst_len; i++)
      dst[o++] = (char)tolower((unsigned char)src[i]);
   dst[o] = '\0';
}

/* Trim leading/trailing whitespace in place; returns the new start (NULL-safe). */
static inline char *util_trim(char *s)
{
   if (!s)
      return s;
   while (*s && isspace((unsigned char)*s))
      s++;
   char *end = s + strlen(s);
   while (end > s && isspace((unsigned char)end[-1]))
      *--end = '\0';
   return s;
}

/* Monotonic clock in milliseconds (CLOCK_MONOTONIC). For elapsed-time / timeout
 * measurement. Inline so it adds no link dependency — many TUs open-coded this. */
static inline long long util_now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

/* Parse an ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ" or "YYYY-MM-DD
 * HH:MM:SS") to a time_t. Returns (time_t)0 on NULL/empty/unparseable input.
 * Inline so it adds no link dependency — several TUs open-coded this. */
static inline time_t str_parse_iso8601(const char *s)
{
   if (!s || !s[0])
      return (time_t)0;
   int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
   if (sscanf(s, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &min, &sec) != 6 &&
       sscanf(s, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &min, &sec) != 6)
      return (time_t)0;
   struct tm tm_buf;
   memset(&tm_buf, 0, sizeof(tm_buf));
   tm_buf.tm_year = year - 1900;
   tm_buf.tm_mon = month - 1;
   tm_buf.tm_mday = day;
   tm_buf.tm_hour = hour;
   tm_buf.tm_min = min;
   tm_buf.tm_sec = sec;
   tm_buf.tm_isdst = 0;
   return timegm(&tm_buf);
}

/* Case-insensitive substring test: 1 if `needle` occurs in `haystack` ignoring
 * ASCII case, else 0 (empty/NULL needle or NULL haystack -> 0). Inline so it
 * adds no link dependency — ~13 TUs each open-coded this identical search. */
static inline int str_contains_ci(const char *haystack, const char *needle)
{
   if (!haystack || !needle || !needle[0])
      return 0;
   size_t nlen = strlen(needle);
   for (const char *p = haystack; *p; p++)
   {
      size_t i = 0;
      while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
         i++;
      if (i == nlen)
         return 1;
   }
   return 0;
}

/* Normalize a key: lowercase, strip filler words, collapse whitespace.
 * Result written to buf (max buf_len). Returns buf. */
char *normalize_key(const char *key, char *buf, size_t buf_len);

/* Trigram Jaccard similarity between two strings. Returns 0.0-1.0. */
double trigram_similarity(const char *a, const char *b);

/* Basic Porter-like stemming. Result written to buf. Returns buf. */
char *stem_word(const char *word, char *buf, size_t buf_len);

/* Canonical fingerprint: normalize + stem + sort words. */
char *canonical_fingerprint(const char *text, char *buf, size_t buf_len);

/* Word-set Jaccard similarity. */
double word_similarity(const char *a, const char *b);

/* Check if two strings are likely contradictions. */
int is_contradiction(const char *a, const char *b);

/* POSIX-like shell token splitting. Returns token count.
 * Tokens written to out array (max max_tokens). Caller frees each token. */
int shlex_split(const char *command, char **out, int max_tokens);

/* Extract file paths from a shell command. Returns path count.
 * Paths written to out array. Caller frees each path. */
int extract_paths_shlex(const char *command, char **out, int max_paths);

/* Check if a token looks like a file path. */
int is_likely_path(const char *tok);

/* Tokenize text for search: lowercase, split, filter stops. Returns token count.
 * Tokens written to out array. Caller frees each token. */
int tokenize_for_search(const char *text, char **out, int max_tokens);

/* Conservative detector for low-information acknowledgement chatter. */
int is_noise_utterance(const char *text);

/* Expand terms for lexical indexing (includes camelCase/snake_case splits). */
char *expand_terms_for_search(char **terms, int count, char *buf, size_t buf_len);

/* Split camelCase into parts. Returns part count. */
int split_camel_case(const char *s, char **out, int max_parts);

/* --- Option parsing --- */

#define OPT_MAX_POSITIONAL 16
#define OPT_MAX_FLAGS      32

typedef struct
{
   const char *name;  /* e.g., "weight" for --weight=N */
   const char *value; /* points into argv, NULL if not found */
   int is_bool;       /* 1 if flag-only (no value), 0 if key=value */
} opt_flag_t;

typedef struct
{
   opt_flag_t flags[OPT_MAX_FLAGS];
   int flag_count;
   const char *positional[OPT_MAX_POSITIONAL];
   int pos_count;
} opt_parsed_t;

/* Parse argc/argv into structured flags and positional args.
 * Handles --flag=value, --flag value, --bool-flag, and positionals.
 * bool_flags is a NULL-terminated list of flags that take no value. */
void opt_parse(int argc, char **argv, const char **bool_flags, opt_parsed_t *out);

/* Get a flag value by name. Returns NULL if not found. */
const char *opt_get(const opt_parsed_t *opts, const char *name);

/* Check if a boolean flag is set. */
int opt_has(const opt_parsed_t *opts, const char *name);

/* Get positional arg by index. Returns NULL if out of range. */
const char *opt_pos(const opt_parsed_t *opts, int index);

/* Get flag value as int. Returns default_val if not found. */
int opt_get_int(const opt_parsed_t *opts, const char *name, int default_val);

/* Get boolean flag: returns 1 if the flag was present, 0 otherwise. */
int opt_get_flag(const opt_parsed_t *opts, const char *name);

/* --- Command execution --- */

/* Run a shell command, capture stdout+stderr. Returns allocated buffer (caller frees).
 * Sets *exit_code to process exit status. Returns NULL on popen failure.
 * Respects the thread-local CWD set by run_cmd_set_cwd(). */
char *run_cmd(const char *cmd, int *exit_code);

/* Like run_cmd(), but runs the command via `/bin/sh -c` under execve() with an
 * explicit environment `envp` (NULL = inherit), capturing combined
 * stdout+stderr. The environment — including any secret it carries, e.g.
 * GH_TOKEN for the forge-credential broker — crosses ONLY into the child
 * process: never the process-global environment (so it is thread-safe under
 * concurrent turns) and never the command line (ps-safe). Honors the
 * thread-local run_cmd CWD. Caller frees the returned buffer. */
char *run_cmd_env(const char *cmd, char *const envp[], int *exit_code);

/* Set (or clear with NULL) the working directory for run_cmd() on this thread.
 * Thread-safe: each thread has its own value. Use instead of chdir() in server
 * pool threads so concurrent sessions do not race on the process-global CWD. */
void run_cmd_set_cwd(const char *cwd);

/* Read the thread-local CWD set by run_cmd_set_cwd. Returns NULL when
 * unset. Used by fork-and-exec tools (tool_bash, sandbox_exec) so the
 * spawned child chdirs to the same directory run_cmd would use, instead
 * of inheriting the server process's cwd. */
const char *run_cmd_get_cwd(void);

/* --- Regex helpers --- */

/* One-shot regex match: compile pattern, test against text, free.
 * Returns 1 on match, 0 on no match or compile error.
 * flags are passed to regcomp (e.g. REG_EXTENDED | REG_ICASE). */
int regex_match(const char *pattern, const char *text, int flags);

/* --- Security helpers --- */

/* Fork/exec with argv array (no shell). Capture stdout into *out_buf (caller frees).
 * Returns child exit code, or -1 on fork/pipe failure. */
int safe_exec_capture(const char *const argv[], char **out_buf, size_t max_out);

/* Like safe_exec_capture, but the child runs under an explicit environment
 * `envp` (NULL = inherit the parent's). The env — including any secret it
 * carries (e.g. GH_TOKEN for the forge-credential broker) — is set ONLY in the
 * child (never the process-global environment, so it is thread-safe under
 * concurrent turns; never the command line, so it is ps-safe). Returns the
 * child exit code, or -1 on fork/pipe failure. */
int safe_exec_capture_env(const char *const argv[], char *const envp[], char **out_buf,
                          size_t max_out);

/* Distinct return code when the child exceeded its wall-clock timeout (killed).
 * Chosen to not collide with a real 0-255 exit status or the -1 fork/pipe error. */
#define SAFE_EXEC_TIMEOUT (-2)

/* Like safe_exec_capture_env, but also runs the child in `cwd` (NULL = inherit)
 * and enforces a wall-clock `timeout_ms` (<=0 = no limit). On timeout the child
 * is SIGKILLed, reaped, and SAFE_EXEC_TIMEOUT is returned. Captured output up to
 * the kill is still returned in *out_buf. A failed chdir aborts the child (127).
 * Used by operator command blocks: repo-pinned cwd, sanitized env, bounded run. */
int safe_exec_capture_cwd_env_timeout(const char *const argv[], const char *cwd, char *const envp[],
                                      char **out_buf, size_t max_out, int timeout_ms);

/* --- Internal git network ops (NOT the user-facing `aimee git` CLI) ---
 *
 * Automated paths (session start, indexing, verify, branch orchestration) must
 * never hang on an unreachable remote or block on an interactive credential /
 * SSH-passphrase prompt. Two reusable forms:
 *   - GIT_SAFE_ENV:        shell-command prefix for run_cmd()-style call sites.
 *   - GIT_SAFE_SSH_COMMAND: the bare GIT_SSH_COMMAND / core.sshCommand value,
 *                           for argv/exec call sites (e.g. git_net_exec()).
 * GIT_TERMINAL_PROMPT=0 fails fast instead of prompting for HTTP credentials;
 * BatchMode=yes does the same for SSH auth; ConnectTimeout caps a dead-host
 * connect. */
#define GIT_SAFE_SSH_COMMAND "ssh -o BatchMode=yes -o ConnectTimeout=5"
#define GIT_SAFE_ENV "GIT_TERMINAL_PROMPT=0 GIT_SSH_COMMAND='" GIT_SAFE_SSH_COMMAND "' "

/* Wall-clock cap (ms) for an internal git network op. Generous enough not to
 * kill a slow-but-working fetch, short enough that a stalled remote can never
 * freeze a session start. */
#define GIT_NET_TIMEOUT_MS 30000

/* Run an internal git network command, non-interactively and hang-proof.
 * `git_argv` is the git subcommand and its args (e.g.
 * {"fetch","--quiet","origin","main",NULL}); the repo is selected by chdir to
 * `cwd`, or the thread-local run_cmd CWD when `cwd` is NULL. Runs under a copy of
 * the parent environment with GIT_SSH_COMMAND (BatchMode/ConnectTimeout) and
 * GIT_TERMINAL_PROMPT=0 forced, so SSH/HTTP auth fail fast instead of blocking on
 * a passphrase or credential prompt regardless of what the parent exported, and
 * hard-capped at GIT_NET_TIMEOUT_MS wall-clock (SIGKILL on overrun) as a backstop
 * for any other stall. Combined stdout+stderr (truncated to max_out, or a default
 * when 0) is returned in *out_buf when non-NULL (caller frees). Returns the child
 * exit code, SAFE_EXEC_TIMEOUT, or -1 on failure. POSIX-only, like run_cmd(). */
int git_net_exec(const char *cwd, const char *const *git_argv, char **out_buf, size_t max_out);

/* Returns 1 if string contains shell metacharacters (;|&$`(){}><\n\r'"\\). */
int has_shell_metachar(const char *s);

/* Shell-escape a string for use inside single quotes (' -> '\''). Caller frees. */
char *shell_escape(const char *raw);

/* printf-append into buf at pos, returning the new pos clamped to [0, cap].
 * Use instead of the unsafe `pos += snprintf(buf + pos, cap - pos, ...)` idiom:
 * snprintf returns the would-be length, so that idiom lets pos run past cap and
 * the next (cap - pos) wraps to a huge size_t, writing out of bounds. This
 * clamps so a full buffer truncates instead of overflowing. No-op if pos>=cap. */
int str_appendf(char *buf, int pos, int cap, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/* Safe strdup: aborts on allocation failure. */
char *safe_strdup(const char *s);

/* Strip local-model private reasoning scaffolds from a response string. */
char *strip_llm_private_scaffold(const char *text);

/* Sanitize a string to only contain safe characters (alphanumeric, dash, underscore, dot).
 * Replaces all other characters with underscores in-place. */
void sanitize_shell_token(char *s);

/* Returns 1 if string contains only [A-Za-z0-9_-] and length 1-128. */
int is_safe_id(const char *s);

/* Append actionable fix guidance for known delegation errors into buf.
 * Returns 1 if a matching pattern was found, 0 otherwise. */
int delegation_error_guidance(const char *error, char *buf, size_t len);

/* Standard base64 (RFC 4648, '=' padded). Shared binary-safe codec for framing
 * raw bytes over text channels (e.g. PTY bytes over SSE/NDJSON). */
size_t aimee_base64_encoded_len(size_t in_len); /* bytes needed incl. NUL */
size_t aimee_base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_cap);
/* Decode NUL-terminated base64 (whitespace skipped); returns decoded byte count
 * or (size_t)-1 on malformed input / insufficient out_cap. */
size_t aimee_base64_decode(const char *in, unsigned char *out, size_t out_cap);

#endif /* DEC_UTIL_H */
