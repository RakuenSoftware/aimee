/* posix/memory.c: POSIX memory quality gates and content scanning. */
#include "aimee.h"
#include "memory.h"
#include "modules/memory/memory_platform.h"
#include "lifecycle.h"
#include <pthread.h>
#include <regex.h>
#include <string.h>
#include <strings.h>

/* --- Precompiled regex patterns (compiled once via pthread_once) --- */

static pthread_once_t regex_once = PTHREAD_ONCE_INIT;

/* Gate: sensitive content patterns */
#define SENSITIVE_PATTERN_COUNT 4
static regex_t sensitive_re[SENSITIVE_PATTERN_COUNT];
static regex_t sensitive_re_cap[SENSITIVE_PATTERN_COUNT]; /* same patterns, with capture */
static int sensitive_compiled[SENSITIVE_PATTERN_COUNT];

/* Gate: ephemeral content patterns */
static regex_t ephemeral_re[2];
static int ephemeral_compiled[2];

/* Gate: evidence markers */
static regex_t evidence_re;
static int evidence_compiled;

/* Scan: content safety rules */
#define SCAN_RE_MAX 16
static regex_t scan_re[SCAN_RE_MAX];
static int scan_compiled[SCAN_RE_MAX];

static void compile_regex_patterns(void)
{
   static const char *sensitive_patterns[] = {
       "(api[_-]?key|token|secret|password|passwd|credential)[[:space:]]*[:=][[:space:]]*"
       "[^[:space:]]+",
       "AKIA[0-9A-Z]{16}",
       "-----BEGIN[[:space:]](RSA[[:space:]]|EC[[:space:]]|DSA[[:space:]])?PRIVATE[[:space:]]"
       "KEY-----",
       "(social[_. ]security|ssn|date[_. ]of[_. ]birth|dob)[[:space:]]*[:=][[:space:]]*"
       "[^[:space:]]+",
   };
   for (int i = 0; i < SENSITIVE_PATTERN_COUNT; i++)
   {
      sensitive_compiled[i] = (regcomp(&sensitive_re[i], sensitive_patterns[i],
                                       REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0);
      /* Also compile with capture for redaction */
      if (sensitive_compiled[i])
         regcomp(&sensitive_re_cap[i], sensitive_patterns[i], REG_EXTENDED | REG_ICASE);
   }

   ephemeral_compiled[0] =
       (regcomp(&ephemeral_re[0], "[0-9]+ (lines|bytes|files)", REG_EXTENDED | REG_NOSUB) == 0);
   ephemeral_compiled[1] =
       (regcomp(&ephemeral_re[1], "(just now|currently|right now|at the moment)",
                REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0);

   evidence_compiled = (regcomp(&evidence_re,
                                "(/[a-zA-Z0-9_./]+\\.[a-z]+|`[^`]+`|https?://|error:|failed:|"
                                "output:)",
                                REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0);
}

static void ensure_regex_init(void)
{
   pthread_once(&regex_once, compile_regex_patterns);
}

/* --- Write Quality Gates --- */

int gate_check_sensitive(const char *content, char *redacted, size_t redacted_cap)
{
   if (!content || !content[0])
      return 0;

   ensure_regex_init();

   for (int i = 0; i < SENSITIVE_PATTERN_COUNT; i++)
   {
      if (!sensitive_compiled[i])
         continue;
      if (regexec(&sensitive_re[i], content, 0, NULL, 0) != 0)
         continue;

      /* Match found — redact using the capture variant. Redact the secret and
       * truncate the (post-secret) tail to fit redacted_cap, rather than
       * rejecting whenever the whole content exceeds the buffer. Otherwise any
       * benign content >= redacted_cap bytes that merely contains a keyword like
       * "secret:"/"password:" is dropped (e.g. long conversation turns), which
       * silently loses real memories.
       *
       * We still reject (return 2) when the buffer is too small to hold even the
       * text before the secret plus the [REDACTED] marker — there is no clean
       * redaction in that case, so refusing is the safe choice (relied on by
       * trajectory export with a deliberately tiny redaction buffer). */
      regmatch_t m[1];
      if (regexec(&sensitive_re_cap[i], content, 1, m, 0) == 0)
      {
         size_t so = (size_t)m[0].rm_so;
         size_t eo = (size_t)m[0].rm_eo;
         size_t content_len = strlen(content);
         static const char marker[] = "[REDACTED]";
         size_t marker_len = sizeof(marker) - 1;
         if (so + marker_len + 1 > redacted_cap)
            return 2; /* cannot fit prefix + marker — refuse */
         memcpy(redacted, content, so);
         memcpy(redacted + so, marker, marker_len);
         size_t off = so + marker_len;
         size_t rest_len = content_len > eo ? content_len - eo : 0;
         size_t space = redacted_cap - 1 - off;
         if (rest_len > space)
            rest_len = space;
         memcpy(redacted + off, content + eo, rest_len);
         redacted[off + rest_len] = '\0';
         return 1; /* redacted (tail may be truncated to fit) */
      }
      return 2; /* matched a secret but could not locate the span to redact — reject */
   }
   return 0;
}

int gate_check_ephemeral(const char *content)
{
   if (!content)
      return 0;

   ensure_regex_init();

   for (int i = 0; i < 2; i++)
   {
      if (ephemeral_compiled[i] && regexec(&ephemeral_re[i], content, 0, NULL, 0) == 0)
         return 1;
   }
   return 0;
}

int gate_has_evidence_markers(const char *content)
{
   if (!content)
      return 0;

   ensure_regex_init();
   return evidence_compiled && regexec(&evidence_re, content, 0, NULL, 0) == 0;
}

/* --- Content Safety Scanning --- */

typedef struct
{
   const char *pattern;
   const char *class_name;
   int action;
} scan_rule_t;

static const scan_rule_t scan_rules[] = {
    /* Block: never persist */
    {"-----BEGIN.*PRIVATE KEY-----", "restricted", SCAN_BLOCK},
    {"AKIA[0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z]"
     "[0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z]",
     "restricted", SCAN_BLOCK},
    {"ghp_[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]"
     "[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]",
     "restricted", SCAN_BLOCK},

    /* Redact: persist with value masked */
    {"(password|passwd|secret|api_key|api-key|apikey|token)[[:space:]]*[:=][[:space:]]*[^[:space:]]"
     "+",
     "restricted", SCAN_REDACT},

    /* Classify: persist but mark sensitive */
    {"[0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9][0-9][0-9]", "restricted", SCAN_REDACT},
    {"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z][A-Za-z]+", "sensitive", SCAN_CLASSIFY},
    {"(10\\.[0-9]+\\.[0-9]+\\.[0-9]+|192\\.168\\.[0-9]+\\.[0-9]+)", "sensitive", SCAN_CLASSIFY},

    {NULL, NULL, 0},
};

static pthread_once_t scan_regex_once = PTHREAD_ONCE_INIT;
static int scan_rule_count;

static void compile_scan_regex(void)
{
   for (int i = 0; scan_rules[i].pattern; i++)
   {
      if (i >= SCAN_RE_MAX)
         break;
      scan_compiled[i] =
          (regcomp(&scan_re[i], scan_rules[i].pattern, REG_EXTENDED | REG_ICASE) == 0);
      scan_rule_count = i + 1;
   }
}

const char *memory_scan_content(char *content, size_t content_len)
{
   if (!content || !content[0])
      return "normal";

   pthread_once(&scan_regex_once, compile_scan_regex);

   const char *highest_class = "normal";
   int class_rank = 0; /* 0=normal, 1=sensitive, 2=restricted */

   for (int i = 0; i < scan_rule_count; i++)
   {
      if (!scan_compiled[i])
         continue;

      regmatch_t match;
      if (regexec(&scan_re[i], content, 1, &match, 0) == 0)
      {
         if (scan_rules[i].action == SCAN_BLOCK)
            return NULL; /* block */

         if (scan_rules[i].action == SCAN_REDACT && match.rm_so >= 0)
         {
            /* Find the value portion (after = or :) and redact */
            for (int j = match.rm_so; j < match.rm_eo && j < (int)content_len; j++)
            {
               if (content[j] == '=' || content[j] == ':')
               {
                  /* Skip whitespace after delimiter */
                  j++;
                  while (j < match.rm_eo && content[j] == ' ')
                     j++;
                  /* Redact the value */
                  int redact_start = j;
                  int redact_end = match.rm_eo;
                  if (redact_end - redact_start > 0)
                  {
                     const char *redacted = "[REDACTED]";
                     int rlen = (int)strlen(redacted);
                     int vlen = redact_end - redact_start;
                     if (rlen <= vlen)
                     {
                        memcpy(content + redact_start, redacted, rlen);
                        memmove(content + redact_start + rlen, content + redact_end,
                                content_len - redact_end + 1);
                     }
                  }
                  break;
               }
            }
         }

         int rank = (strcmp(scan_rules[i].class_name, "restricted") == 0)  ? 2
                    : (strcmp(scan_rules[i].class_name, "sensitive") == 0) ? 1
                                                                           : 0;
         if (rank > class_rank)
         {
            class_rank = rank;
            highest_class = scan_rules[i].class_name;
         }
      }
   }

   return highest_class;
}
