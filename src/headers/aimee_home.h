/* aimee_home.h: per-profile config-root resolver for the
 * aimee-profiles-and-usage-insights proposal.
 *
 * Resolution order (first match wins):
 *   1. AIMEE_HOME env var (absolute path)
 *   2. $HOME/.config/aimee/profiles/<AIMEE_PROFILE>/ if AIMEE_PROFILE set
 *   3. $HOME/.config/aimee/
 *
 * Pure helper. Returns a pointer into a static buffer that's stable
 * across calls within a process (re-resolved each call, but always
 * lands at the same address). Returns NULL only when $HOME is unset
 * and no AIMEE_HOME override is provided — a broken environment.
 *
 * Callers migrate from platform_config_dir() and ad-hoc
 * "$HOME/.config/aimee" snprintfs to this helper as part of the
 * audit step (separate iter, gated by check_aimee_home.sh). */
#ifndef DEC_AIMEE_HOME_H
#define DEC_AIMEE_HOME_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Resolve the active aimee config-root path. NULL on unrecoverable
    * env failure (no HOME and no AIMEE_HOME override). */
   const char *aimee_home(void);
   /* Pure per-call variant; no environment mutation. */
   const char *aimee_home_profile(const char *profile);

   /* Resolve the profiles container directory (parent of all named
    * profiles). Honours AIMEE_HOME. NULL on env failure. */
   const char *aimee_profiles_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_AIMEE_HOME_H */
