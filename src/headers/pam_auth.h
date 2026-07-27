/* pam_auth.h — the one PAM credential check, shared by every login surface.
 *
 * Extracted from posix/dashboard.c so aimee-kb's password-login route and the
 * dashboard's HTTP Basic auth ask the SAME question of the SAME PAM service
 * rather than each carrying its own stack. A second implementation would mean two
 * places deciding what a valid host login is, and only one of them would get
 * fixed. */
#ifndef DEC_PAM_AUTH_H
#define DEC_PAM_AUTH_H

/* Authenticate `user` with `password` against the "aimee" PAM service. Returns 1
 * on success, 0 on any failure — including a build without PAM, which rejects
 * every credential rather than falling open. */
int pam_check_credentials(const char *user, const char *password);

#endif /* DEC_PAM_AUTH_H */
