/* kb_curator_sidecar.h: shared LLM-sidecar invocation for curator passes.
 *
 * Several curator passes (judge, synthesize) shell out to a configured command
 * that reads a JSON request on stdin and writes a JSON response on stdout. This
 * factors out the temp-file + popen + capture plumbing they share.
 * No DB access. */
#ifndef KB_CURATOR_SIDECAR_H
#define KB_CURATOR_SIDECAR_H

#include <stddef.h>

/* Run `cmd` with `json_input` piped on stdin; capture up to `out_cap`-1 bytes
 * of stdout into a freshly malloc'd, NUL-terminated buffer (caller frees).
 * Returns the buffer on success, or NULL on any failure (spawn error, non-zero
 * exit, OOM) with a human-readable reason written to errbuf. out_cap <= 0 uses
 * a sensible default. */
char *kb_curator_sidecar_run(const char *cmd, const char *json_input, int out_cap, char *errbuf,
                             size_t errlen);

/* Quote `in` as a single sh word (POSIX single-quote form), so it can be
 * interpolated into a shell command line without the shell re-parsing its
 * contents. Exposed for testing.
 *
 * Needed because bounding a sidecar with timeout(1) means handing the configured
 * command to a SECOND shell: interpolating it raw into `sh -c "<cmd>"` would let
 * an embedded quote, $, backtick or backslash change the command's meaning —
 * commands that worked under a bare popen. Returns 0 on success, -1 if the
 * quoted form does not fit (caller must treat that as a hard error, never as a
 * reason to fall back to the raw string). */
int kb_curator_shell_quote(const char *in, char *out, size_t outlen);

/* Render a pclose(3) wait status into an operator-legible reason. Shared with
 * callers that run their own popen (the code-unit stage wraps its command in
 * timeout(1)); exposed for testing.
 *
 * pclose returns a wait(2)-encoded status, not an exit code — reporting it raw
 * logged "sidecar exited 256" for a plain exit(1). Distinguishes a non-zero
 * exit, a signal kill (OOM), and a timeout. Pass timeout_s > 0 only if the
 * command was wrapped in coreutils timeout(1) (whose cap shows as exit 124);
 * callers that do not wrap pass 0. */
void kb_curator_describe_wait_status(int status, int timeout_s, char *errbuf, size_t errlen);

/* Append the sidecar's OWN error to errbuf, if it wrote one before exiting.
 *
 * curator-extract.py's emit_error() prints {"status":"error","error":<why>} to
 * stdout and exits 1 — so the reason ("LLM returned non-JSON: …", "unknown
 * role", the provider's error) is sitting in the captured output at the exact
 * moment the caller is about to throw it away and report a bare "sidecar exited
 * 1". Call this after kb_curator_describe_wait_status to keep the reason. A
 * no-op when the output is absent or is not a structured error. Exposed for
 * testing. */
void kb_curator_append_sidecar_error(const char *out, char *errbuf, size_t errlen);

#endif /* KB_CURATOR_SIDECAR_H */
