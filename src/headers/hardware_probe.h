#ifndef DEC_HARDWARE_PROBE_H
#define DEC_HARDWARE_PROBE_H 1

#include <stddef.h>

typedef struct
{
   int detected;
   int vram_mb;
   char name[128];
   char vendor[32];
   char memory_kind[32];
   char error[256];
} hardware_probe_result_t;

/* One discrete GPU on a host (an entry in an enumerated inventory). */
typedef struct
{
   int index;       /* the vendor tool's device ordinal (nvidia-smi index / drm cardN) */
   char name[128];  /* product name */
   char vendor[32]; /* "nvidia" | "amd" */
   int vram_mb;     /* total VRAM in MiB */
} hardware_gpu_t;

#define HARDWARE_MAX_GPUS 16

/* An enumerated GPU inventory for one host (local or remote). `error` is set (and
 * count 0) when the probe could not run; count 0 with no error means "no GPU". */
typedef struct
{
   int count;
   hardware_gpu_t gpus[HARDWARE_MAX_GPUS];
   char error[256];
} hardware_gpu_list_t;

/* The single shell probe run BOTH locally (popen) and remotely (over ssh) so one
 * parser covers both. It prints one CSV line per GPU: "<index>, <name>, <vram_mb>"
 * — NVIDIA via nvidia-smi, else AMD via /sys/class/drm. Empty output = no GPU. */
extern const char *const HARDWARE_GPU_PROBE_CMD;

/* Parse the uniform "<index>, <name>, <vram_mb>" CSV (one GPU per line) into a
 * list, ignoring blank/malformed lines. Returns the number of GPUs parsed. */
int hardware_probe_parse_gpu_csv_list(const char *csv, const char *vendor_hint,
                                      hardware_gpu_list_t *out);
/* Enumerate the LOCAL host's GPUs by running HARDWARE_GPU_PROBE_CMD via popen. */
int hardware_probe_list_local(hardware_gpu_list_t *out);
/* Enumerate a REMOTE host's GPUs over ssh. `ssh_target` is "user@host" (or "host");
 * `port` <= 0 uses the default. Read-only (a device query); BatchMode, no prompts.
 * The AIMEE_SSH_BIN env override (shared with the ssh delegate backend) lets tests
 * substitute a fixture. On ssh/transport failure, out->error is set and count 0. */
int hardware_probe_list_remote(const char *ssh_target, int port, hardware_gpu_list_t *out);

#endif /* DEC_HARDWARE_PROBE_H */
