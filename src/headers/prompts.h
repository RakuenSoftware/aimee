#ifndef AIMEE_PROMPTS_H
#define AIMEE_PROMPTS_H 1

typedef struct config config_t;

/* Tiered system prompt profiles.
 *
 * MINIMAL  — low-token prompt for simple/delegate tasks
 * STANDARD — full autonomous engineer prompt (default)
 * EXTENDED — structured reasoning + verification framework
 *
 * Selection order (highest wins):
 *   1. --prompt <file>  CLI flag (custom file overrides tier entirely)
 *   2. --prompt-tier    CLI flag
 *   3. prompt_tier      config.json field
 *   4. PROMPT_STANDARD  (built-in default)
 *
 * After the tier prompt is built, .aimee/prompt.md in the project root
 * is appended when present.
 */

typedef enum
{
   PROMPT_MINIMAL,  /* concise, low-token */
   PROMPT_STANDARD, /* full autonomous engineer (default) */
   PROMPT_EXTENDED, /* standard + structured reasoning/verification */
} prompt_tier_t;

/* Operating mode for the primary agent's persona and shared principles.
 * ENGINEER (default) preserves all existing software-engineering behavior;
 * NOVEL reframes the agent as a creative-writing author/worldbuilder, and
 * SONGWRITER as a lyricist/songwriter. The four reviewer modes (QA, SECURITY,
 * REVIEWER, ARCHITECT) reframe the agent as a read-only senior reviewer that
 * surfaces findings rather than editing. The mode is a property the agent-exec
 * paths respect (brief, delegates, compute agents), not a one-off prompt swap. */
typedef enum
{
   AIMEE_MODE_ENGINEER = 0,              /* default: autonomous software engineer */
   AIMEE_MODE_NOVEL = 1,                 /* creative-writing author / worldbuilder */
   AIMEE_MODE_SONGWRITER = 2,            /* songwriter / lyricist */
   AIMEE_MODE_QA = 3,                    /* senior QA / test reviewer */
   AIMEE_MODE_SECURITY = 4,              /* senior application-security reviewer */
   AIMEE_MODE_REVIEWER = 5,              /* senior, thorough, contrarian code reviewer */
   AIMEE_MODE_ARCHITECT = 6,             /* software-architecture reviewer */
   AIMEE_MODE_REVIEWER_CONSTRUCTIVE = 7, /* constructive reviewer: assess as written */
   AIMEE_MODE_TECH_WRITER = 8,           /* technical writer / documentation reviewer */
} aimee_mode_t;

/* Parse a mode name ("engineer", "novel", "songwriter", "qa", "security",
 * "reviewer", "architect"); returns AIMEE_MODE_ENGINEER for NULL/empty/
 * unrecognised values. */
aimee_mode_t aimee_mode_from_string(const char *name);

/* Canonical lowercase name for a mode ("engineer", "novel"). Never NULL. */
const char *aimee_mode_to_string(aimee_mode_t mode);

/* Convert tier name string ("MINIMAL", "STANDARD", "EXTENDED") to enum.
 * Returns PROMPT_STANDARD for unrecognised values. */
prompt_tier_t prompt_tier_from_string(const char *name);

/* Convert tier enum to canonical name string ("MINIMAL", "STANDARD", "EXTENDED"). */
const char *prompt_tier_to_string(prompt_tier_t tier);

/* Shared principles block for the given mode (engineer -> Code Principles,
 * novel -> Craft Principles). Leads primary-agent startup context and
 * delegate/compute-agent system prompts. Never NULL. */
const char *prompt_principles_text(aimee_mode_t mode);

/* The STANDARD-tier persona text (the "You are ..." system prompt body, with a
 * %s cwd placeholder) for a built-in mode. Used to dump built-in personas to
 * editable files. Never NULL. */
const char *prompt_persona_text(aimee_mode_t mode);

/* Heap-allocated copy of base_prompt with the mode's principles prepended.
 * Idempotent: if base_prompt already starts with the block, returns a plain
 * duplicate. Caller must free(). Returns NULL only on OOM. */
char *prompt_prepend_principles(aimee_mode_t mode, const char *base_prompt);

/* Shared coding principles (the AIMEE_MODE_ENGINEER form of
 * prompt_principles_text). */
const char *prompt_code_principles_text(void);

/* Engineer-mode form of prompt_prepend_principles. Caller must free(). */
char *prompt_prepend_code_principles(const char *base_prompt);

/* Build a system prompt string.
 *
 * Parameters:
 *   tier        — prompt tier to use
 *   cwd         — working directory to embed (must not be NULL)
 *   custom_file — path to a file whose content replaces the tier prompt entirely;
 *                 pass NULL to use the tier template
 *
 * The function appends <cwd>/.aimee/prompt.md when it exists (project override).
 *
 * Returns a heap-allocated string; caller must free().
 * Returns NULL only on OOM.
 */
char *prompt_build(prompt_tier_t tier, const char *cwd, const char *custom_file);

/* Mode-aware variant of prompt_build. For AIMEE_MODE_NOVEL it selects the
 * author/worldbuilder persona for the STANDARD/EXTENDED tiers; MINIMAL and the
 * custom-file/.aimee-override paths are mode-independent. prompt_build() is the
 * AIMEE_MODE_ENGINEER form and is byte-identical to its prior behavior.
 * Returns a heap-allocated string; caller must free(). NULL only on OOM. */
char *prompt_build_mode(aimee_mode_t mode, prompt_tier_t tier, const char *cwd,
                        const char *custom_file);

/* Append lower-priority disposition guidance derived from config.
 * Returns a heap-allocated prompt string; caller must free().
 * If no dispositions are configured, returns a duplicate of base_prompt. */
char *prompt_apply_dispositions(const char *base_prompt, const config_t *cfg);

/* Prepend the operator-authored charter at the top of the prompt so
 * it has highest precedence over dispositions, the working profile,
 * and turn/session context. The charter block only appears when at
 * least one of the four arrays has entries. Returns a heap-allocated
 * string; caller must free(). If no charter is configured, returns a
 * duplicate of base_prompt. */
char *prompt_apply_charter(const char *base_prompt, const config_t *cfg);

/* Append a `## Working Profile` section to the base prompt describing
 * the committed learned-preference state — soft-constraint framing so
 * explicit turn-level instructions override it. Gated by
 * `identity_working_profile_injection_enabled` (default off); when
 * enabled with an empty allow list, every committed canonical field is
 * injected; when enabled with a non-empty allow list, only those
 * fields are. No-op when disabled or no committed rows match the
 * allow list. Returns a heap-allocated string; caller must free(). */
char *prompt_apply_working_profile(const char *base_prompt, const config_t *cfg);

#endif /* AIMEE_PROMPTS_H */
