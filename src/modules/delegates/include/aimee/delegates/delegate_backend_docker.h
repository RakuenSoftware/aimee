/* delegate_backend_docker.h: docker execution backend.
 *
 * Long-term shape (per delegate-execution-backends.md):
 *   acquire   ensure container `aimee-delegate-<task_id>` exists; if
 *             not, `docker create` from the image hint in cfg, mount
 *             ~/.cache/aimee/delegate/<task_id>/ so files survive
 *             container restart. `docker start` if it exists but
 *             stopped.
 *   exec      `docker exec aimee-delegate-<task_id> bash -c '<wrapped>'`
 *   release   hibernate=1 -> `docker stop` (no rm); next acquire does
 *             `docker start`. hibernate=0 -> `docker rm -f`.
 *
 * Helper functions below expose pure string-building seams for tests;
 * production code uses the registered backend vtable. */
#ifndef DEC_DELEGATE_BACKEND_DOCKER_H
#define DEC_DELEGATE_BACKEND_DOCKER_H 1

#include <aimee/delegates/delegate_backend.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Register the docker backend in the process-global registry under
    * the name "docker". Returns 0 on success, -1 on duplicate-name
    * collision (existing entry wins) or other registry failure. */
   int delegate_backend_register_docker(void);

   /* Test seam — get the static docker backend instance directly,
    * bypassing the registry. */
   delegate_backend_t *delegate_backend_docker_get(void);

   /* Build the canonical container name for `task_id`. Container name
    * follows docker's [a-zA-Z0-9_.-]+ rules; characters outside that
    * set in `task_id` are replaced with '_'. The "aimee-delegate-"
    * prefix makes the container easy to spot in `docker ps`.
    *
    * Returns 0 + writes the name into `out`, or -1 on NULL/empty
    * task_id or undersized buffer. */

   /* Remove delegate containers left running by an earlier server process.
    * Server startup calls this before its worker pools exist, so every matching
    * container is necessarily orphaned. Returns removed count or -1. */
   int delegate_backend_docker_remove_orphans(void);

   /* Periodic runtime reap of aged delegate containers (older than max_age_secs).
    * Unlike remove_orphans (startup-only), this runs while the server is live to
    * reclaim containers leaked when a delegate is stale-cancelled/crashes and its
    * release() never fires. max_age_secs must exceed the in-tool job cap so a live
    * turn is never removed. Returns removed count or -1. */
   int delegate_backend_docker_reap_aged(int max_age_secs);

   /* Build the docker exec command-string used by exec(). Pure
    * string assembly. Shape:
    *   docker exec -i <container_name> bash -c '<base64-encoded-script>'
    * The script is base64-encoded so quoting in `wrapped_script`
    * (single quotes, backticks, dollar signs) survives transit
    * through docker's shell unmolested. Same pattern as the SSH
    * backend's b64 envelope.
    *
    * Returns 0 + sets *out_cmd (caller frees), or -1 on NULL/empty
    * inputs or allocation failure. */
   int delegate_backend_docker_build_exec_command(const char *container_name,
                                                  const char *wrapped_script, char **out_cmd);

   /* Translate a path as seen inside the server container into the Docker-host
    * source path that backs it. `mount_table` is one destination<TAB>source pair
    * per line, as emitted by Docker inspect. The longest containing destination
    * wins. Returns 1 when translated, 0 when no mount contains the path (and
    * copies the original path), or -1 for invalid/truncated input. */
   int delegate_backend_docker_translate_mount_path(const char *container_path,
                                                    const char *mount_table, char *out,
                                                    size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DELEGATE_BACKEND_DOCKER_H */
