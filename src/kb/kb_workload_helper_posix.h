#ifndef AIMEE_KB_WORKLOAD_HELPER_POSIX_H
#define AIMEE_KB_WORKLOAD_HELPER_POSIX_H

#include <stddef.h>

#define KB_WORKLOAD_HELPER_FRAME_MAX      65536U
#define KB_WORKLOAD_HELPER_TIMEOUT_MAX_MS 5000

typedef enum
{
   KB_WORKLOAD_HELPER_OK = 0,
   KB_WORKLOAD_HELPER_INVALID = 1,
   KB_WORKLOAD_HELPER_DISABLED = 2,
   KB_WORKLOAD_HELPER_UNAVAILABLE = 3,
   KB_WORKLOAD_HELPER_TIMEOUT = 4,
   KB_WORKLOAD_HELPER_INTEGRITY = 5,
} kb_workload_helper_result_t;

/* Race-free absolute-path walk from `/`. Every component is opened with
 * O_NOFOLLOW and must be root-owned and not group/world writable. The leaf
 * must be a regular root-owned file. When require_exec_elf is true it must
 * additionally be non-setid, executable, and a native ELF image. */
kb_workload_helper_result_t kb_workload_checked_root_file_open(const char *path,
                                                               int require_exec_elf, int *fd_out);

static inline kb_workload_helper_result_t kb_workload_helper_open(const char *path, int *fd_out)
{
   return kb_workload_checked_root_file_open(path, 1, fd_out);
}

/* Execute the already-checked descriptor, never its old pathname. One absolute
 * monotonic deadline covers setup, duplex I/O, termination, and exact reap.
 * The child receives only stdin/stdout and an empty environment. response_len
 * is zeroed on entry and all non-OK exits. */
kb_workload_helper_result_t kb_workload_helper_invoke(int helper_fd, const unsigned char *request,
                                                      size_t request_len, unsigned char *response,
                                                      size_t response_cap, size_t *response_len,
                                                      int timeout_ms);

#endif
