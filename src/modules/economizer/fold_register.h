/* fold_register.h: register/trust grammar for assistant turns (fold §6, P3).
 *
 * Classifies an assistant message by a leading "register" tag so the fold can
 * preserve which folded turns were settled conclusions (verdict/hazard) versus
 * transient work (in-progress/executing/blocked), and so episode-seal harvesting
 * (§5, P5) can gate on settled turns only. Deterministic, ASCII/UTF-8-safe, no
 * model. Soft: an untagged turn classifies as in-progress, so the fold stays
 * correct whether or not the agent emits registers. */
#ifndef DEC_FOLD_REGISTER_H
#define DEC_FOLD_REGISTER_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      FOLD_REG_IN_PROGRESS = 0, /* default / untagged (🔍) */
      FOLD_REG_EXECUTING,       /* ▶ */
      FOLD_REG_VERDICT,         /* 🏁 settled conclusion */
      FOLD_REG_HAZARD,          /* ⚠ warning/hazard */
      FOLD_REG_BLOCKED,         /* ❓ blocked/needs input */
      FOLD_REG_COUNT
   } fold_register_t;

   /* Parse the leading register of an assistant message. Recognizes (after
    * optional leading whitespace) either a leading glyph — 🔍 ▶ 🏁 ⚠ ❓ — or a
    * bracketed word tag, case-insensitive: [verdict]/[done], [hazard]/[warning],
    * [executing]/[exec], [blocked], [in-progress]/[wip]. Anything else (incl.
    * NULL/empty) -> FOLD_REG_IN_PROGRESS. */
   fold_register_t fold_register_parse(const char *text);

   /* Short stable ASCII label (e.g. "verdict"); never NULL. */
   const char *fold_register_label(fold_register_t r);

   /* 1 if the register denotes a settled conclusion worth preserving / harvesting
    * (verdict or hazard); 0 otherwise. */
   int fold_register_is_settled(fold_register_t r);

#ifdef __cplusplus
}
#endif

#endif /* DEC_FOLD_REGISTER_H */
