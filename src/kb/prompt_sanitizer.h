/* prompt_sanitizer.h: the single render-boundary sanitizer for corpus-derived
 * strings that flow into an agent's prompt (proposal
 * graph-feedback-self-audit-and-learning §4 / P0).
 *
 * Every field that originates in attacker-influenceable content — a file path, a
 * symbol label, a community name, a memory-fact body, a lesson, a correction, a
 * model-generated caption/transcript, a Markdown doc — is untrusted. Rendered raw
 * into a prompt it can carry ANSI escapes, fabricated log lines, or role/tool
 * markup that hijacks the turn. This module is the ONE place that neutralizes
 * that, so §1 findings, §2 diffs, §3 lessons, and §6 media captions all route
 * through a single audited boundary instead of ad-hoc escaping at each call site.
 *
 * Pre-audit (P0): the codebase had no render-boundary prompt sanitizer before
 * this. `strip_llm_private_scaffold` (util.c) strips model scaffolding from OUR
 * OWN output; `server/tool_schema_sanitizer.c` normalizes tool-call JSON schemas;
 * `shell_quote` (util.c) quotes for a shell. None sanitizes untrusted corpus
 * text for prompt rendering — so this is a new, owned boundary, not a fork.
 *
 * Two layers (roundtable R1): STRICT VALIDATORS reject structured fields that
 * carry control/markup rather than silently rewriting them (a rewritten path is
 * a wrong-but-plausible path); RENDER ESCAPING defangs free text in place and
 * never rejects. See sanitize_kind_t.
 *
 * Pure: no DB/network/global state, so it unit-tests standalone. */
#ifndef PROMPT_SANITIZER_H
#define PROMPT_SANITIZER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* The field kind decides the rule set and the length bound. The first four are
    * STRUCTURED (strict validators — reject on control/markup); the rest are FREE
    * TEXT (render escaping — defang in place, never reject). */
   typedef enum
   {
      /* structured — validated, reject on violation */
      SANITIZE_SYMBOL_LABEL = 0, /* a qualified symbol name                       */
      SANITIZE_FILE_PATH,        /* a normalized relative path                    */
      SANITIZE_SOURCE_LOCATION,  /* "path:line" / "path:line:col"                 */
      SANITIZE_COMMUNITY_NAME,   /* a community / module label                    */
      /* free text — escaped in place, never rejected */
      SANITIZE_MEMORY_FACT,   /* a memory-graph fact body                          */
      SANITIZE_LESSON_TEXT,   /* a §3 lessons-artifact line                        */
      SANITIZE_CORRECTION,    /* a §3 correction body                              */
      SANITIZE_IMAGE_CAPTION, /* a §6 vision-model caption (untrusted model text)  */
      SANITIZE_TRANSCRIPT,    /* a §6 STT transcript (untrusted model text)        */
      SANITIZE_MARKDOWN_DOC,  /* a Markdown/MDX body                               */
      SANITIZE_KIND_COUNT
   } sanitize_kind_t;

   typedef enum
   {
      SANITIZE_OK = 0,    /* written clean, fits out_len                          */
      SANITIZE_TRUNCATED, /* written, but the field exceeded its bound and was cut */
      SANITIZE_REJECTED   /* a structured field carried control/markup; out is "" */
   } sanitize_status_t;

   /* Reason codes accompanying a non-OK status (for audit / fail-closed callers). */
   typedef enum
   {
      SANITIZE_REASON_NONE = 0,
      SANITIZE_REASON_LENGTH,         /* exceeded the per-kind bound                */
      SANITIZE_REASON_CONTROL_CHAR,   /* embedded C0/C1/DEL or ANSI escape          */
      SANITIZE_REASON_NEWLINE,        /* newline/CR in a structured field           */
      SANITIZE_REASON_INJECTION_MARK, /* role/tool/directive markup in a structured field */
      SANITIZE_REASON_BAD_ARG         /* NULL / zero-length buffer                  */
   } sanitize_reason_t;

   /* Sanitize `field` (a NUL-terminated string) for kind `kind` into out[0..out_len).
    * out is always NUL-terminated on return (set to "" on REJECTED / bad arg).
    * `*out_reason` (if non-NULL) receives the reason for a non-OK status.
    *
    * STRUCTURED kinds: return SANITIZE_REJECTED (out="") if the raw field carries a
    * newline/CR, any C0/C1/DEL control byte, an ANSI escape, or an enumerated
    * injection marker — the caller decides how to fail closed. Otherwise the value
    * is copied verbatim (it is already clean), TRUNCATED if it exceeds the bound.
    *
    * FREE-TEXT kinds: strip control/ANSI/C1, defang enumerated role/tool/directive
    * markers in place, bound the length (TRUNCATED if cut). Never REJECTED.
    *
    * Returns SANITIZE_REJECTED with SANITIZE_REASON_BAD_ARG if out/out_len is unusable. */
   sanitize_status_t sanitize_for_prompt(const char *field, sanitize_kind_t kind, char *out,
                                         size_t out_len, sanitize_reason_t *out_reason);

   /* The per-kind maximum output length (excluding the NUL). Exposed for tests and
    * for callers sizing buffers. Returns 0 for an invalid kind. */
   size_t sanitize_kind_bound(sanitize_kind_t kind);

   /* True if `kind` is a STRUCTURED (strict-validated) kind, false if free text.
    * Exposed so the CI call-site guard can assert the right layer per field. */
   int sanitize_kind_is_structured(sanitize_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif /* PROMPT_SANITIZER_H */
