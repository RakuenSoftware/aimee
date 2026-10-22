#ifndef AIMEE_SERVER_NATIVE_LAUNCH_H
#define AIMEE_SERVER_NATIVE_LAUNCH_H
/* Resolve an operator's registered command to the actual held native program.
 * Codex npm registrations resolve their matching native package without running
 * JavaScript. The digest measures the executable descriptor, never metadata. */
int server_native_launch_open(const char *sdk, const char *command, char digest[65],
                              char error[512]);
#endif
