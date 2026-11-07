#ifndef AIMEE_SERVER_NATIVE_CONTAINMENT_H
#define AIMEE_SERVER_NATIVE_CONTAINMENT_H
#include "cli_native_spawn_guard.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>
typedef struct server_native_containment server_native_containment_t;
/* Explicit delegated cgroup-v2 root only. NULL/empty configuration is optional,
 * not a qualification; callers preserve their existing direct-launch path. */
/* Harmless disposable child exercises the SAME setup/barrier before optional
 * launch selection. Zero means supported now; never a proof for another tree. */
int server_native_containment_probe(const char *root, const uint16_t *tcp_ports, size_t port_count,
                                    char error[512]);
server_native_containment_t *server_native_containment_open(const char *root,
                                                            const uint16_t *tcp_ports,
                                                            size_t port_count, char error[512]);
const cJSON *server_native_containment_manifest(const server_native_containment_t *owner);
int server_native_containment_sha256(const server_native_containment_t *owner, char out[65]);
/* Bind an independently materialized fresh credential HOME before first spawn.
 * Only HOME and its exact SDK state interior become writable; cwd stays separate.
 * Child reopens paths after unshare and checks these same identities. */
int server_native_containment_bind_home(server_native_containment_t *owner, const char *home);
const cli_native_spawn_guard_t *server_native_containment_guard(server_native_containment_t *owner);
/* Reopening proves identity only, never proves that an SDK was admitted. */
server_native_containment_t *server_native_containment_reopen(const cJSON *manifest,
                                                              char error[512]);
int server_native_containment_empty(server_native_containment_t *owner);
int server_native_containment_quiesce(server_native_containment_t *owner, int timeout_ms);
/* Remove only an exact, empty boundary after its canonical owner is reconciled.
 * Otherwise retain its immutable name/inode for restart proof. */
void server_native_containment_close(server_native_containment_t *owner, int remove_empty);
#endif
